/*
 * test_pool.c — regression tests for the AFD buffer pool and the
 * single-consumer MPSC ready queue.
 *
 * These tests deliberately exercise empty-to-nonempty transitions,
 * concurrent pool take/give operations, producer contention, and
 * accounting.  Every stress loop has a deadline so a lost queue node
 * cannot turn a test run into an unbounded hang.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "wepoll_ex.h"
#include "wepoll_ex_internal.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern int ep_afd_pool_init(ep_afd_pool_t *p, size_t buf_size,
                            size_t capacity);
extern void ep_afd_pool_destroy(ep_afd_pool_t *p);
extern void *ep_afd_pool_take(ep_afd_pool_t *p);
extern void ep_afd_pool_give(ep_afd_pool_t *p, void *buf);

extern void ep_ready_init(ep_ready_queue_t *q);
extern void ep_ready_destroy(ep_ready_queue_t *q);
extern void ep_ready_push(ep_ready_queue_t *q, ep_ready_node_t *node);
extern ep_ready_node_t *ep_ready_drain(ep_ready_queue_t *q, int maxevents);

static int tests_passed;
static int tests_failed;

#define TEST(name) do { printf("  [test] %-58s ", (name)); fflush(stdout); } while (0)
#define PASS()     do { printf("OK\n"); tests_passed++; } while (0)
#define FAIL(why)  do { printf("FAIL: %s (errno=%d %s)\n", (why), errno, strerror(errno)); tests_failed++; } while (0)

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}

static void sleep_one_ms(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
    (void)nanosleep(&ts, NULL);
}

static int join_with_deadline(pthread_t thread, int seconds)
{
#if defined(__linux__)
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return pthread_join(thread, NULL);
    }
    deadline.tv_sec += seconds;
    return pthread_timedjoin_np(thread, NULL, &deadline);
#else
    (void)seconds;
    return pthread_join(thread, NULL);
#endif
}

typedef struct test_fixture {
    ep_afd_pool_t   pool;
    ep_ready_queue_t queue;
} test_fixture_t;

static int fixture_init(test_fixture_t *fixture, size_t capacity)
{
    memset(fixture, 0, sizeof(*fixture));
    if (ep_afd_pool_init(&fixture->pool, sizeof(ep_ready_node_t), capacity) != 0) {
        return -1;
    }
    ep_ready_init(&fixture->queue);
    if (!fixture->queue.initialized) {
        ep_afd_pool_destroy(&fixture->pool);
        return -1;
    }
    return 0;
}

static void fixture_destroy(test_fixture_t *fixture)
{
    for (;;) {
        ep_ready_node_t *chain = ep_ready_drain(&fixture->queue, INT_MAX);
        if (chain == NULL) break;
        while (chain != NULL) {
            ep_ready_node_t *next =
                atomic_load_explicit(&chain->next, memory_order_relaxed);
            ep_afd_pool_give(&fixture->pool, chain);
            chain = next;
        }
    }
    ep_ready_destroy(&fixture->queue);
    ep_afd_pool_destroy(&fixture->pool);
}

static ep_ready_node_t *node_take(test_fixture_t *fixture, uint32_t value)
{
    ep_ready_node_t *node =
        (ep_ready_node_t *)ep_afd_pool_take(&fixture->pool);
    if (node != NULL) {
        atomic_init(&node->next, (ep_ready_node_t *)NULL);
        node->sock = NULL;
        node->events = value;
        node->flags = 0;
        node->timestamp = 0;
    }
    return node;
}

static void node_give(test_fixture_t *fixture, ep_ready_node_t *node)
{
    ep_afd_pool_give(&fixture->pool, node);
}

static int consume_chain(test_fixture_t *fixture, ep_ready_node_t *chain,
                         uint8_t *seen, size_t seen_len,
                         size_t *duplicates, size_t *out_of_range,
                         uint32_t *next_sequence, size_t producer_count,
                         uint32_t messages_per_producer,
                         size_t *fifo_violations)
{
    int count = 0;
    while (chain != NULL) {
        ep_ready_node_t *next =
            atomic_load_explicit(&chain->next, memory_order_relaxed);
        uint32_t value = chain->events;
        if (seen != NULL) {
            if ((size_t)value >= seen_len) {
                (*out_of_range)++;
            } else if (seen[value] != 0) {
                (*duplicates)++;
            } else {
                seen[value] = 1;
            }

            if ((size_t)value < seen_len && next_sequence != NULL) {
                size_t producer = value / messages_per_producer;
                uint32_t sequence = value % messages_per_producer;
                if (producer >= producer_count ||
                    sequence != next_sequence[producer]) {
                    (*fifo_violations)++;
                } else {
                    next_sequence[producer]++;
                }
            }
        }
        node_give(fixture, chain);
        chain = next;
        count++;
    }
    return count;
}

static void test_pool_basic(void)
{
    TEST("pool accounting, growth, duplicate return, and ownership");

    ep_afd_pool_t pool = { 0 };
    ep_afd_pool_t other = { 0 };
    if (ep_afd_pool_init(&pool, 128, 4) != 0) {
        FAIL("init");
        return;
    }
    if (ep_afd_pool_init(&other, 128, 2) != 0) {
        ep_afd_pool_destroy(&pool);
        FAIL("init");
        return;
    }

    int ok = (pool.allocated == 4 &&
              atomic_load_explicit(&pool.in_use, memory_order_relaxed) == 0 &&
              atomic_load_explicit(&pool.peak, memory_order_relaxed) == 0);
    void *buffers[5] = { NULL, NULL, NULL, NULL, NULL };
    for (size_t i = 0; i < 5; i++) {
        buffers[i] = ep_afd_pool_take(&pool);
        if (buffers[i] == NULL) ok = 0;
        if (buffers[i] != NULL &&
            (uintptr_t)buffers[i] % _Alignof(max_align_t) != 0) {
            ok = 0;
        }
        for (size_t j = 0; j < i; j++) {
            if (buffers[i] == buffers[j]) ok = 0;
        }
    }

    if (atomic_load_explicit(&pool.in_use, memory_order_relaxed) != 5 ||
        atomic_load_explicit(&pool.peak, memory_order_relaxed) != 5 ||
        pool.allocated != 5) {
        ok = 0;
    }

    for (size_t i = 0; i < 5; i++) ep_afd_pool_give(&pool, buffers[i]);
    if (atomic_load_explicit(&pool.in_use, memory_order_relaxed) != 0) ok = 0;

    errno = 0;
    ep_afd_pool_give(&pool, buffers[0]);
    if (atomic_load_explicit(&pool.in_use, memory_order_relaxed) != 0 ||
        errno != EINVAL) {
        ok = 0;
    }

    void *foreign = ep_afd_pool_take(&other);
    if (foreign == NULL) {
        ok = 0;
    } else {
        errno = 0;
        ep_afd_pool_give(&pool, foreign);
        if (atomic_load_explicit(&pool.in_use, memory_order_relaxed) != 0 ||
            atomic_load_explicit(&other.in_use, memory_order_relaxed) != 1 ||
            errno != EINVAL) {
            ok = 0;
        }
        ep_afd_pool_give(&other, foreign);
    }

    void *busy = ep_afd_pool_take(&pool);
    errno = 0;
    ep_afd_pool_destroy(&pool);
    if (busy == NULL || pool.initialized == 0 || errno != EBUSY) ok = 0;
    ep_afd_pool_give(&pool, busy);
    ep_afd_pool_destroy(&pool);
    ep_afd_pool_destroy(&other);

    if (ok) PASS(); else FAIL("pool invariant");
}

typedef struct pool_worker_args {
    ep_afd_pool_t  *pool;
    _Atomic int    *ready;
    _Atomic int    *start;
    _Atomic int    *failures;
    int             iterations;
} pool_worker_args_t;

static void *pool_worker(void *arg)
{
    pool_worker_args_t *worker = (pool_worker_args_t *)arg;
    atomic_fetch_add_explicit(worker->ready, 1, memory_order_release);
    while (atomic_load_explicit(worker->start, memory_order_acquire) == 0) {
        sched_yield();
        pthread_testcancel();
    }

    for (int i = 0; i < worker->iterations; i++) {
        unsigned char *buf =
            (unsigned char *)ep_afd_pool_take(worker->pool);
        if (buf == NULL) {
            atomic_fetch_add_explicit(worker->failures, 1,
                                      memory_order_relaxed);
            continue;
        }
        buf[0] = (unsigned char)i;
        ep_afd_pool_give(worker->pool, buf);
        pthread_testcancel();
    }
    return NULL;
}

static void cancel_and_join(pthread_t *threads, const unsigned char *joined,
                            int count)
{
    for (int i = 0; i < count; i++) {
        if (!joined[i]) (void)pthread_cancel(threads[i]);
    }
    for (int i = 0; i < count; i++) {
        if (!joined[i]) (void)pthread_join(threads[i], NULL);
    }
}

static void test_pool_concurrent_take_give(void)
{
    TEST("pool concurrent take/give has stable accounting");

    enum { N_THREADS = 8, ITERATIONS = 20000 };
    ep_afd_pool_t pool = { 0 };
    pthread_t threads[N_THREADS];
    pool_worker_args_t args[N_THREADS];
    _Atomic int ready = 0;
    _Atomic int start = 0;
    _Atomic int failures = 0;
    int created = 0;
    unsigned char joined[N_THREADS] = { 0 };
    int pool_initialized = ep_afd_pool_init(&pool, 32, 8) == 0;
    int ok = pool_initialized;

    if (ok) {
        for (int i = 0; i < N_THREADS; i++) {
            args[i] = (pool_worker_args_t){
                .pool = &pool, .ready = &ready, .start = &start,
                .failures = &failures, .iterations = ITERATIONS
            };
            if (pthread_create(&threads[i], NULL, pool_worker, &args[i]) != 0) {
                ok = 0;
                break;
            }
            created++;
        }
    }

    uint64_t deadline = now_ns() + UINT64_C(5000000000);
    while (ok && atomic_load_explicit(&ready, memory_order_acquire) < created &&
           now_ns() < deadline) {
        sleep_one_ms();
    }
    if (ok && atomic_load_explicit(&ready, memory_order_acquire) != created) {
        ok = 0;
    }
    atomic_store_explicit(&start, 1, memory_order_release);

    for (int i = 0; i < created; i++) {
        if (join_with_deadline(threads[i], 5) != 0) {
            ok = 0;
        } else {
            joined[i] = 1;
        }
    }
    if (!ok && created > 0) cancel_and_join(threads, joined, created);

    if (pool_initialized) {
        if (atomic_load_explicit(&failures, memory_order_relaxed) != 0 ||
            atomic_load_explicit(&pool.in_use, memory_order_relaxed) != 0 ||
            atomic_load_explicit(&pool.peak, memory_order_relaxed) == 0) {
            ok = 0;
        }
        ep_afd_pool_destroy(&pool);
    }
    if (ok) PASS(); else FAIL("concurrent pool accounting");
}

static void test_queue_empty_transitions(void)
{
    TEST("MPSC queue repeated empty/non-empty transitions");

    test_fixture_t fixture;
    int ok = fixture_init(&fixture, 1) == 0;
    enum { ITERATIONS = 10000 };

    if (ok) {
        for (uint32_t i = 0; i < ITERATIONS; i++) {
            ep_ready_node_t *node = node_take(&fixture, i);
            ep_ready_node_t *chain;
            if (node == NULL) { ok = 0; break; }
            ep_ready_push(&fixture.queue, node);
            chain = ep_ready_drain(&fixture.queue, 1);
            if (chain == NULL || chain != node ||
                atomic_load_explicit(&fixture.queue.queued,
                                     memory_order_relaxed) != 0) {
                ok = 0;
                if (chain != NULL) node_give(&fixture, chain);
                break;
            }
            node_give(&fixture, chain);
        }
    }

    if (ok && ep_ready_drain(&fixture.queue, 1) != NULL) ok = 0;
    if (ok && atomic_load_explicit(&fixture.pool.in_use,
                                   memory_order_relaxed) != 0) ok = 0;
    fixture_destroy(&fixture);
    if (ok) PASS(); else FAIL("empty transition");
}

static void test_queue_bounded_fifo_and_destroy(void)
{
    TEST("MPSC bounded drain is FIFO and destroy preserves live nodes");

    enum { N_NODES = 7 };
    test_fixture_t fixture;
    int ok = fixture_init(&fixture, N_NODES) == 0;

    if (ok) {
        for (uint32_t i = 0; i < N_NODES; i++) {
            ep_ready_node_t *node = node_take(&fixture, i);
            if (node == NULL) {
                ok = 0;
                break;
            }
            ep_ready_push(&fixture.queue, node);
        }
    }

    if (ok) {
        errno = 0;
        ep_ready_destroy(&fixture.queue);
        if (!fixture.queue.initialized || errno != EBUSY) ok = 0;
    }

    uint32_t expected = 0;
    const int limits[] = { 2, 3, 2 };
    for (size_t pass = 0; ok && pass < sizeof(limits) / sizeof(limits[0]);
         pass++) {
        ep_ready_node_t *chain =
            ep_ready_drain(&fixture.queue, limits[pass]);
        int count = 0;
        while (chain != NULL) {
            ep_ready_node_t *next =
                atomic_load_explicit(&chain->next, memory_order_relaxed);
            if (chain->events != expected++) ok = 0;
            node_give(&fixture, chain);
            chain = next;
            count++;
        }
        if (count != limits[pass]) ok = 0;
    }

    if (ok && (expected != N_NODES ||
               ep_ready_drain(&fixture.queue, 1) != NULL ||
               atomic_load_explicit(&fixture.queue.queued,
                                    memory_order_relaxed) != 0 ||
               atomic_load_explicit(&fixture.pool.in_use,
                                    memory_order_relaxed) != 0)) {
        ok = 0;
    }

    fixture_destroy(&fixture);
    if (ok) PASS(); else FAIL("bounded FIFO/destroy invariant");
}

typedef struct producer_args {
    test_fixture_t *fixture;
    _Atomic int    *ready;
    _Atomic int    *start;
    _Atomic int    *done;
    _Atomic int    *pushed;
    _Atomic int    *failures;
    int             id;
    int             count;
} producer_args_t;

static void *producer_worker(void *arg)
{
    producer_args_t *producer = (producer_args_t *)arg;
    atomic_fetch_add_explicit(producer->ready, 1, memory_order_release);
    while (atomic_load_explicit(producer->start, memory_order_acquire) == 0) {
        sched_yield();
        pthread_testcancel();
    }

    for (int i = 0; i < producer->count; i++) {
        uint32_t value = (uint32_t)(producer->id * producer->count + i);
        ep_ready_node_t *node = node_take(producer->fixture, value);
        if (node == NULL) {
            atomic_fetch_add_explicit(producer->failures, 1,
                                      memory_order_relaxed);
            break;
        }
        ep_ready_push(&producer->fixture->queue, node);
        atomic_fetch_add_explicit(producer->pushed, 1, memory_order_relaxed);
        pthread_testcancel();
    }
    atomic_fetch_add_explicit(producer->done, 1, memory_order_release);
    return NULL;
}

static void test_queue_multi_producer(void)
{
    TEST("MPSC queue multi-producer no loss or duplicates");

    enum { N_PRODUCERS = 8, N_MESSAGES = 10000 };
    const size_t expected = (size_t)N_PRODUCERS * N_MESSAGES;
    test_fixture_t fixture;
    pthread_t threads[N_PRODUCERS];
    producer_args_t args[N_PRODUCERS];
    _Atomic int ready = 0;
    _Atomic int start = 0;
    _Atomic int done = 0;
    _Atomic int pushed = 0;
    _Atomic int failures = 0;
    int created = 0;
    unsigned char joined[N_PRODUCERS] = { 0 };
    int fixture_initialized = fixture_init(&fixture, 32) == 0;
    int ok = fixture_initialized;
    uint8_t *seen = NULL;
    uint32_t next_sequence[N_PRODUCERS] = { 0 };

    if (ok) {
        seen = (uint8_t *)calloc(expected, sizeof(*seen));
        if (seen == NULL) ok = 0;
    }

    if (ok) {
        for (int i = 0; i < N_PRODUCERS; i++) {
            args[i] = (producer_args_t){
                .fixture = &fixture, .ready = &ready, .start = &start,
                .done = &done, .pushed = &pushed, .failures = &failures,
                .id = i, .count = N_MESSAGES
            };
            if (pthread_create(&threads[i], NULL, producer_worker, &args[i]) != 0) {
                ok = 0;
                break;
            }
            created++;
        }
    }

    uint64_t deadline = now_ns() + UINT64_C(10000000000);
    while (ok && atomic_load_explicit(&ready, memory_order_acquire) < created &&
           now_ns() < deadline) {
        sleep_one_ms();
    }
    if (ok && atomic_load_explicit(&ready, memory_order_acquire) != created) {
        ok = 0;
    }
    atomic_store_explicit(&start, 1, memory_order_release);

    size_t drained = 0;
    size_t duplicates = 0;
    size_t out_of_range = 0;
    size_t fifo_violations = 0;
    while (ok && (atomic_load_explicit(&done, memory_order_acquire) < created ||
                  drained < (size_t)atomic_load_explicit(&pushed,
                                                         memory_order_relaxed))) {
        ep_ready_node_t *chain = ep_ready_drain(&fixture.queue, 256);
        if (chain != NULL) {
            drained += (size_t)consume_chain(&fixture, chain, seen, expected,
                                             &duplicates, &out_of_range,
                                             next_sequence, N_PRODUCERS,
                                             N_MESSAGES, &fifo_violations);
        } else {
            if (now_ns() >= deadline) {
                ok = 0;
                break;
            }
            sleep_one_ms();
        }
        if (now_ns() >= deadline) {
            ok = 0;
            break;
        }
    }

    for (int i = 0; i < created; i++) {
        if (join_with_deadline(threads[i], 5) != 0) {
            ok = 0;
        } else {
            joined[i] = 1;
        }
    }
    if (!ok && created > 0) cancel_and_join(threads, joined, created);

    /* Producers are now quiescent; collect any final published nodes. */
    if (fixture_initialized) {
        for (;;) {
            ep_ready_node_t *chain =
                ep_ready_drain(&fixture.queue, INT_MAX);
            if (chain == NULL) break;
            drained += (size_t)consume_chain(
                &fixture, chain, seen, expected, &duplicates, &out_of_range,
                next_sequence, N_PRODUCERS, N_MESSAGES, &fifo_violations);
        }
    }

    if (fixture_initialized) {
        for (size_t i = 0; i < N_PRODUCERS; i++) {
            if (next_sequence[i] != N_MESSAGES) fifo_violations++;
        }
        if ((size_t)atomic_load_explicit(&pushed,
                                         memory_order_relaxed) != expected ||
            drained != expected || duplicates != 0 || out_of_range != 0 ||
            fifo_violations != 0 ||
            atomic_load_explicit(&failures, memory_order_relaxed) != 0 ||
            atomic_load_explicit(&fixture.queue.queued,
                                 memory_order_relaxed) != 0 ||
            atomic_load_explicit(&fixture.pool.in_use,
                                 memory_order_relaxed) != 0) {
            ok = 0;
        }
    }

    free(seen);
    fixture_destroy(&fixture);
    if (ok) PASS(); else FAIL("MPSC stress invariant");
}

int main(void)
{
    /* CTest also enforces a timeout, but keep direct invocations bounded. */
    (void)alarm(45);

    printf("wepoll-ex pool + MPSC queue regression tests\n");
    printf("=============================================\n");

    test_pool_basic();
    test_pool_concurrent_take_give();
    test_queue_empty_transitions();
    test_queue_bounded_fifo_and_destroy();
    test_queue_multi_producer();

    (void)alarm(0);
    printf("\nSummary: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
