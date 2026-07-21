/*
 * bench_latency.c — measure epoll_wait round-trip latency.
 *
 * This microbenchmark measures the time from a write on one end of a
 * socketpair to the moment epoll_wait returns the corresponding
 * EPOLLIN event.  It's a proxy for the kernel-to-user notification
 * cost that dominates high-connection-count server workloads.
 *
 * Run with: ./bench_latency [iterations]
 * Default:  100000 iterations.
 */
#define _POSIX_C_SOURCE 200809L

#include "wepoll_ex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <errno.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv)
{
    uint64_t iters = 100000;
    if (argc > 1) iters = (uint64_t)strtoull(argv[1], NULL, 10);
    if (iters == 0) iters = 100000;

    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        perror("socketpair");
        return 1;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); return 1; }

    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLET,  /* ET — each write is one edge */
        .data.fd = pair[0]
    };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair[0], &ev) != 0) {
        perror("epoll_ctl");
        return 1;
    }

    /* Warm-up. */
    for (int i = 0; i < 1000; i++) {
        char c = 'x';
        write(pair[1], &c, 1);
        struct epoll_event out[1];
        epoll_wait(epfd, out, 1, 100);
        read(pair[0], &c, 1);
    }

    /* Measurement. */
    uint64_t total_ns = 0;
    uint64_t max_ns = 0;
    uint64_t min_ns = UINT64_MAX;

    for (uint64_t i = 0; i < iters; i++) {
        char c = 'x';
        uint64_t t0 = now_ns();
        write(pair[1], &c, 1);
        struct epoll_event out[1];
        int n = epoll_wait(epfd, out, 1, 100);
        uint64_t t1 = now_ns();
        if (n != 1) {
            fprintf(stderr, "iter %llu: expected 1 event, got %d (errno=%d)\n",
                    (unsigned long long)i, n, errno);
            break;
        }
        read(pair[0], &c, 1);
        uint64_t dt = t1 - t0;
        total_ns += dt;
        if (dt > max_ns) max_ns = dt;
        if (dt < min_ns) min_ns = dt;
    }

    double avg_ns = (double)total_ns / (double)iters;
    printf("wepoll-ex round-trip latency over %llu iterations:\n",
           (unsigned long long)iters);
    printf("  min : %.0f ns (%.3f us)\n", (double)min_ns, min_ns / 1000.0);
    printf("  avg : %.0f ns (%.3f us)\n", avg_ns,           avg_ns / 1000.0);
    printf("  max : %.0f ns (%.3f us)\n", (double)max_ns, max_ns / 1000.0);
    printf("  rate: %.0f ops/sec\n",
           (double)iters / ((double)total_ns / 1e9));

    wepoll_close(epfd);
    close(pair[0]);
    close(pair[1]);
    return 0;
}
