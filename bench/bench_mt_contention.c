/*
 * Multithreaded control-path contention probe.
 *
 * bench_windows is single-threaded, so it cannot observe fd_table_lock
 * contention at all.  This harness runs a wait thread that keeps an active
 * socket set continuously ready — so it is repeatedly inside a real drain
 * holding fd_table_lock — while a second thread churns epoll_ctl on a
 * disjoint set.  The reported quantity is the control thread's per-operation
 * latency distribution, which is what any change to the drain's lock scope
 * would have to move.
 *
 * Two measured caveats, both learned the hard way:
 *
 * 1. Run an A/A comparison first.  One unmodified binary against itself has
 *    been observed varying by more than 40% at ctl_mod p50 across runs, so
 *    balance A/B orderings and treat anything smaller as noise.
 *
 * 2. Per-operation numbers are NOT comparable to each other.  The churn loop
 *    issues ADD, MOD and DEL back to back on the same socket; whichever
 *    operation follows the ADD absorbs the queueing delay behind the wait
 *    thread's in-progress drain, and the timestamps attribute that stall to
 *    it.  Swapping MOD and DEL in the sequence moves the cost with the
 *    position, not with the operation: DEL measured ~3900 ns in the third
 *    slot and ~73000 ns in the second.  Compare each operation only against
 *    the same operation in the same slot of an otherwise identical build.
 *
 * Usage: bench_mt_contention [active_sockets] [churn_sockets] [seconds]
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <process.h>

typedef struct pair {
    SOCKET listener;
    SOCKET client;
    SOCKET server;
} pair_t;

static LARGE_INTEGER g_frequency;
static volatile LONG g_stop;

static uint64_t now_ticks(void)
{
    LARGE_INTEGER value;

    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
}

static double ticks_to_ns(uint64_t ticks)
{
    return (double)ticks * 1e9 / (double)g_frequency.QuadPart;
}

static int pair_open(pair_t *pair)
{
    struct sockaddr_in address;
    int length = (int)sizeof(address);
    u_long nonblocking = 1;

    pair->listener = pair->client = pair->server = INVALID_SOCKET;
    pair->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pair->listener == INVALID_SOCKET) return -1;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(pair->listener, (struct sockaddr *)&address,
             sizeof(address)) != 0 ||
        listen(pair->listener, 1) != 0 ||
        getsockname(pair->listener, (struct sockaddr *)&address,
                    &length) != 0) {
        return -1;
    }

    pair->client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pair->client == INVALID_SOCKET ||
        connect(pair->client, (struct sockaddr *)&address, length) != 0) {
        return -1;
    }
    pair->server = accept(pair->listener, NULL, NULL);
    if (pair->server == INVALID_SOCKET) return -1;
    if (ioctlsocket(pair->server, FIONBIO, &nonblocking) != 0) return -1;
    return 0;
}

static void pair_close(pair_t *pair)
{
    if (pair->server != INVALID_SOCKET) closesocket(pair->server);
    if (pair->client != INVALID_SOCKET) closesocket(pair->client);
    if (pair->listener != INVALID_SOCKET) closesocket(pair->listener);
}

typedef struct wait_context {
    int epfd;
    pair_t *active;
    unsigned active_count;
    struct epoll_event *events;
    uint64_t waits;
    uint64_t delivered;
} wait_context_t;

static unsigned __stdcall wait_thread(void *argument)
{
    wait_context_t *context = (wait_context_t *)argument;

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        int count;

        for (unsigned i = 0; i < context->active_count; i++) {
            (void)send(context->active[i].client, "x", 1, 0);
        }
        count = epoll_wait(context->epfd, context->events,
                           (int)context->active_count, 50);
        context->waits++;
        if (count <= 0) continue;

        context->delivered += (uint64_t)count;
        for (int i = 0; i < count; i++) {
            unsigned index = context->events[i].data.u32;
            char sink[64];

            if (index < context->active_count) {
                (void)recv(context->active[index].server, sink,
                           (int)sizeof(sink), 0);
            }
        }
    }
    return 0;
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return a < b ? -1 : a > b ? 1 : 0;
}

static void report(const char *label, uint64_t *samples, size_t count)
{
    long double total = 0;

    if (count == 0) {
        printf("%s,0,0,0,0,0\n", label);
        return;
    }
    qsort(samples, count, sizeof(*samples), compare_u64);
    for (size_t i = 0; i < count; i++) total += (long double)samples[i];
    printf("%s,%zu,%.0f,%.0f,%.0f,%.0f\n", label, count,
           ticks_to_ns(samples[count / 2]),
           ticks_to_ns(samples[(count * 95) / 100]),
           ticks_to_ns(samples[(count * 99) / 100]),
           (double)(total / (long double)count) * 1e9 /
               (double)g_frequency.QuadPart);
}

int main(int argc, char **argv)
{
    unsigned active_count = argc > 1 ? (unsigned)atoi(argv[1]) : 256;
    unsigned churn_count = argc > 2 ? (unsigned)atoi(argv[2]) : 32;
    unsigned seconds = argc > 3 ? (unsigned)atoi(argv[3]) : 3;
    const size_t sample_capacity = 400000;
    WSADATA wsa_data;
    pair_t *active = NULL;
    pair_t *churn = NULL;
    struct epoll_event *events = NULL;
    uint64_t *add_samples = NULL;
    uint64_t *mod_samples = NULL;
    uint64_t *del_samples = NULL;
    size_t add_count = 0, mod_count = 0, del_count = 0;
    wait_context_t context;
    HANDLE thread = NULL;
    int epfd = -1;
    int result = 1;
    uint64_t deadline;

    if (active_count == 0 || churn_count == 0 || seconds == 0) {
        fprintf(stderr, "usage: %s [active] [churn] [seconds]\n", argv[0]);
        return 2;
    }
    QueryPerformanceFrequency(&g_frequency);
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) return 1;

    active = (pair_t *)calloc(active_count, sizeof(*active));
    churn = (pair_t *)calloc(churn_count, sizeof(*churn));
    events = (struct epoll_event *)calloc(active_count, sizeof(*events));
    add_samples = (uint64_t *)calloc(sample_capacity, sizeof(uint64_t));
    mod_samples = (uint64_t *)calloc(sample_capacity, sizeof(uint64_t));
    del_samples = (uint64_t *)calloc(sample_capacity, sizeof(uint64_t));
    if (active == NULL || churn == NULL || events == NULL ||
        add_samples == NULL || mod_samples == NULL || del_samples == NULL) {
        goto cleanup;
    }

    epfd = epoll_create1(0);
    if (epfd < 0) goto cleanup;

    for (unsigned i = 0; i < active_count; i++) {
        struct epoll_event event;

        if (pair_open(&active[i]) != 0) goto cleanup;
        memset(&event, 0, sizeof(event));
        event.events = EPOLLIN;
        event.data.u32 = i;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, active[i].server, &event) != 0) {
            goto cleanup;
        }
    }
    for (unsigned i = 0; i < churn_count; i++) {
        if (pair_open(&churn[i]) != 0) goto cleanup;
    }

    memset(&context, 0, sizeof(context));
    context.epfd = epfd;
    context.active = active;
    context.active_count = active_count;
    context.events = events;

    printf("# active_sockets=%u churn_sockets=%u seconds=%u\n",
           active_count, churn_count, seconds);
    printf("operation,samples,p50_ns,p95_ns,p99_ns,mean_ns\n");

    g_stop = 0;
    thread = (HANDLE)_beginthreadex(NULL, 0, wait_thread, &context, 0, NULL);
    if (thread == NULL) goto cleanup;

    /* Let the wait thread reach steady state before sampling. */
    Sleep(200);

    deadline = GetTickCount64() + (uint64_t)seconds * 1000u;
    while (GetTickCount64() < deadline) {
        for (unsigned i = 0; i < churn_count; i++) {
            struct epoll_event event;
            uint64_t start;

            memset(&event, 0, sizeof(event));
            event.events = EPOLLIN;
            event.data.u32 = active_count + i;

            start = now_ticks();
            if (epoll_ctl(epfd, EPOLL_CTL_ADD, churn[i].server,
                          &event) != 0) {
                goto stop;
            }
            if (add_count < sample_capacity) {
                add_samples[add_count++] = now_ticks() - start;
            }

            event.events = EPOLLIN | EPOLLOUT;
            start = now_ticks();
            if (epoll_ctl(epfd, EPOLL_CTL_MOD, churn[i].server,
                          &event) != 0) {
                goto stop;
            }
            if (mod_count < sample_capacity) {
                mod_samples[mod_count++] = now_ticks() - start;
            }

            start = now_ticks();
            if (epoll_ctl(epfd, EPOLL_CTL_DEL, churn[i].server, NULL) != 0) {
                goto stop;
            }
            if (del_count < sample_capacity) {
                del_samples[del_count++] = now_ticks() - start;
            }
        }
    }

stop:
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    thread = NULL;

    report("ctl_add", add_samples, add_count);
    report("ctl_mod", mod_samples, mod_count);
    report("ctl_del", del_samples, del_count);
    fprintf(stderr, "wait_thread: waits=%" PRIu64 " delivered=%" PRIu64 "\n",
            context.waits, context.delivered);
    result = 0;

cleanup:
    if (thread != NULL) {
        InterlockedExchange(&g_stop, 1);
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
    }
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (churn != NULL) {
        for (unsigned i = 0; i < churn_count; i++) pair_close(&churn[i]);
    }
    if (active != NULL) {
        for (unsigned i = 0; i < active_count; i++) pair_close(&active[i]);
    }
    free(del_samples);
    free(mod_samples);
    free(add_samples);
    free(events);
    free(churn);
    free(active);
    WSACleanup();
    return result;
}

#else

int main(void)
{
    fprintf(stderr, "bench_mt_contention is Windows-only\n");
    return 0;
}

#endif
