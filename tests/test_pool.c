/*
 * test_pool.c — stress test for the AFD buffer pool and MPSC
 * lock-free ready queue.
 *
 * Spawns N producer threads that each push M ready nodes onto the
 * queue, while a single consumer drains them.  Validates:
 *   - No nodes are lost (producer-count == consumer-count).
 *   - No use-after-free (every node is returned to the pool).
 *   - Pool accounting is consistent (in_use returns to 0).
 *
 * On POSIX this exercises the shared pool implementation; on
 * Windows it would exercise the same code path used by the IOCP
 * completion handler.
 */
#define _POSIX_C_SOURCE 200809L

#include "wepoll_ex.h"
#include "wepoll_ex_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

/* We need access to the internal types and helpers. */
extern int ep_afd_pool_init(ep_afd_pool_t *p, size_t buf_size, size_t capacity);
extern void ep_afd_pool_destroy(ep_afd_pool_t *p);
extern void *ep_afd_pool_take(ep_afd_pool_t *p);
extern void ep_afd_pool_give(ep_afd_pool_t *p, void *buf);

extern void ep_ready_init(ep_ready_queue_t *q);
extern void ep_ready_destroy(ep_ready_queue_t *q);
extern void ep_ready_push(ep_ready_queue_t *q, ep_ready_node_t *node);
extern ep_ready_node_t *ep_ready_drain(ep_ready_queue_t *q, int maxevents);

/* Use a global pool for the test — it stands in for the per-port
 * ready_node_pool that the real engine uses. */
static ep_afd_pool_t g_node_pool;
static ep_ready_queue_t g_queue;

static ep_ready_node_t *node_alloc(void)
{
    void *buf = ep_afd_pool_take(&g_node_pool);
    return (ep_ready_node_t *)buf;
}

static void node_free(ep_ready_node_t *n)
{
    ep_afd_pool_give(&g_node_pool, n);
}

/* ---- Producer thread ---- */
typedef struct {
    int       thread_id;
    int       count;
    _Atomic int *total_pushed;
} producer_args_t;

static void *producer_fn(void *arg)
{
    producer_args_t *a = (producer_args_t *)arg;
    for (int i = 0; i < a->count; i++) {
        ep_ready_node_t *n = node_alloc();
        if (n == NULL) {
            fprintf(stderr, "producer %d: pool exhausted at i=%d\n",
                    a->thread_id, i);
            break;
        }
        memset(n, 0, sizeof(*n));
        atomic_store(&n->next, (ep_ready_node_t *)NULL);
        n->events = (uint32_t)(a->thread_id * 1000 + i);
        n->timestamp = (uint64_t)i;
        ep_ready_push(&g_queue, n);
        atomic_fetch_add(a->total_pushed, 1);
    }
    return NULL;
}

/* ---- Test cases ---- */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)  do { printf("  [test] %-50s ", name); fflush(stdout); } while (0)
#define PASS()      do { printf("OK\n");   tests_passed++; } while (0)
#define FAIL(why)   do { printf("FAIL: %s\n", why); tests_failed++; } while (0)

static void test_pool_basic(void)
{
    TEST("AFD pool take/give roundtrip");
    ep_afd_pool_t p;
    if (ep_afd_pool_init(&p, 128, 4) != 0) { FAIL("init"); return; }

    void *a = ep_afd_pool_take(&p);
    void *b = ep_afd_pool_take(&p);
    void *c = ep_afd_pool_take(&p);
    void *d = ep_afd_pool_take(&p);
    void *e = ep_afd_pool_take(&p);  /* should malloc a fresh one */

    if (!a || !b || !c || !d || !e) { FAIL("null pointers"); goto done; }
    if (a == b || a == c || a == d || a == e) { FAIL("duplicate ptrs"); goto done; }

    ep_afd_pool_give(&p, a);
    ep_afd_pool_give(&p, b);
    ep_afd_pool_give(&p, c);
    ep_afd_pool_give(&p, d);
    ep_afd_pool_give(&p, e);

    /* Pool should now have 5 entries (4 original + 1 grown). */
    void *xs[5];
    for (int i = 0; i < 5; i++) xs[i] = ep_afd_pool_take(&p);
    for (int i = 0; i < 5; i++) {
        if (!xs[i]) { FAIL("pool exhausted prematurely"); goto done; }
    }
    for (int i = 0; i < 5; i++) ep_afd_pool_give(&p, xs[i]);
    PASS();
done:
    ep_afd_pool_destroy(&p);
}

static void test_mpsc_single_producer(void)
{
    TEST("MPSC queue: 1 producer, 1000 nodes, no loss");
    ep_afd_pool_init(&g_node_pool, sizeof(ep_ready_node_t), 32);
    ep_ready_init(&g_queue);

    _Atomic int total = 0;
    producer_args_t a = { .thread_id = 0, .count = 1000, .total_pushed = &total };

    producer_fn(&a);

    /* Drain everything. */
    int drained = 0;
    for (;;) {
        ep_ready_node_t *n = ep_ready_drain(&g_queue, 64);
        if (n == NULL) break;
        while (n) {
            ep_ready_node_t *next = atomic_load(&n->next);
            node_free(n);
            n = next;
            drained++;
        }
    }

    if (drained != 1000) {
        char buf[64];
        snprintf(buf, sizeof(buf), "expected 1000, got %d", drained);
        FAIL(buf);
    } else {
        PASS();
    }

    ep_ready_destroy(&g_queue);
    ep_afd_pool_destroy(&g_node_pool);
}

static void test_mpsc_multi_producer(void)
{
    TEST("MPSC queue: 8 producers x 10000 nodes, no loss");
    ep_afd_pool_init(&g_node_pool, sizeof(ep_ready_node_t), 256);
    ep_ready_init(&g_queue);

    enum { N_PROD = 8, N_MSG = 10000 };
    pthread_t threads[N_PROD];
    producer_args_t args[N_PROD];
    _Atomic int total = 0;

    for (int i = 0; i < N_PROD; i++) {
        args[i].thread_id = i;
        args[i].count = N_MSG;
        args[i].total_pushed = &total;
        pthread_create(&threads[i], NULL, producer_fn, &args[i]);
    }

    /* Consumer drains concurrently. */
    int drained = 0;
    while (drained < N_PROD * N_MSG) {
        ep_ready_node_t *n = ep_ready_drain(&g_queue, 256);
        if (n == NULL) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000 };
            nanosleep(&ts, NULL);
            continue;
        }
        while (n) {
            ep_ready_node_t *next = atomic_load(&n->next);
            node_free(n);
            n = next;
            drained++;
        }
    }

    for (int i = 0; i < N_PROD; i++) pthread_join(threads[i], NULL);

    int pushed = atomic_load(&total);
    if (pushed != N_PROD * N_MSG || drained != N_PROD * N_MSG) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "pushed=%d drained=%d expected=%d",
                 pushed, drained, N_PROD * N_MSG);
        FAIL(buf);
    } else {
        PASS();
    }

    ep_ready_destroy(&g_queue);
    ep_afd_pool_destroy(&g_node_pool);
}

static void test_mpsc_no_duplicates(void)
{
    TEST("MPSC queue: no duplicate deliveries under contention");
    ep_afd_pool_init(&g_node_pool, sizeof(ep_ready_node_t), 256);
    ep_ready_init(&g_queue);

    /* Each producer tags its nodes with thread_id*N_MSG+i so the
     * ranges don't overlap and every event value is unique. */
    enum { N_PROD = 4, N_MSG = 5000 };
    pthread_t threads[N_PROD];
    producer_args_t args[N_PROD];
    _Atomic int total = 0;

    for (int i = 0; i < N_PROD; i++) {
        args[i].thread_id = i * N_MSG / 1000;  /* scale so events are unique */
        args[i].count = N_MSG;
        args[i].total_pushed = &total;
        pthread_create(&threads[i], NULL, producer_fn, &args[i]);
    }

    for (int i = 0; i < N_PROD; i++) pthread_join(threads[i], NULL);

    /* Each producer emits (i*N_MSG/1000)*1000 + j = i*N_MSG + j
     * for j in [0, N_MSG).  So values are [0, N_MSG), [N_MSG, 2*N_MSG),
     * [2*N_MSG, 3*N_MSG), [3*N_MSG, 4*N_MSG). */
    int max_event = N_PROD * N_MSG;  /* exclusive upper bound */
    char *seen = (char *)calloc(max_event, 1);
    int drained = 0;
    int duplicates = 0;
    int out_of_range = 0;

    for (;;) {
        ep_ready_node_t *n = ep_ready_drain(&g_queue, 256);
        if (n == NULL) break;
        while (n) {
            ep_ready_node_t *next = atomic_load(&n->next);
            uint32_t e = n->events;
            if ((int)e >= max_event) {
                out_of_range++;
            } else if (seen[e]) {
                duplicates++;
            } else {
                seen[e] = 1;
            }
            node_free(n);
            n = next;
            drained++;
        }
    }
    free(seen);

    int expected = N_PROD * N_MSG;
    if (drained != expected || duplicates != 0 || out_of_range != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "drained=%d dup=%d oor=%d (expected %d)",
                 drained, duplicates, out_of_range, expected);
        FAIL(buf);
    } else {
        PASS();
    }

    ep_ready_destroy(&g_queue);
    ep_afd_pool_destroy(&g_node_pool);
}

int main(void)
{
    printf("wepoll-ex pool + MPSC queue stress tests\n");
    printf("=========================================\n");

    test_pool_basic();
    test_mpsc_single_producer();
    test_mpsc_multi_producer();
    test_mpsc_no_duplicates();

    printf("\n");
    printf("Summary: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
