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
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int transfer_byte(int fd, char *byte, int write_byte)
{
    for (;;) {
        ssize_t result = write_byte ? write(fd, byte, 1) : read(fd, byte, 1);
        if (result == 1) return 0;
        if (result < 0 && errno == EINTR) continue;
        if (result == 0) errno = EPIPE;
        return -1;
    }
}

static int wait_for_byte(int epfd, int expected_fd)
{
    struct epoll_event out[1];
    int result;

    do {
        result = epoll_wait(epfd, out, 1, 100);
    } while (result < 0 && errno == EINTR);

    if (result != 1) {
        if (result == 0) errno = ETIMEDOUT;
        return -1;
    }
    if (out[0].data.fd != expected_fd || !(out[0].events & EPOLLIN)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static size_t percentile_index(size_t count, unsigned numerator,
                               unsigned denominator)
{
    size_t last = count - 1;
    return (last / denominator) * numerator +
           ((last % denominator) * numerator) / denominator;
}

int main(int argc, char **argv)
{
    uint64_t iters = 100000;
    if (argc > 2) {
        fprintf(stderr, "usage: %s [iterations]\n", argv[0]);
        return 2;
    }
    if (argc == 2) {
        char *end = NULL;
        errno = 0;
        unsigned long long parsed = strtoull(argv[1], &end, 10);
        if (errno != 0 || end == argv[1] || *end != '\0' || parsed == 0 ||
            argv[1][0] == '-') {
            fprintf(stderr, "invalid iteration count: %s\n", argv[1]);
            return 2;
        }
        iters = (uint64_t)parsed;
    }
    if (iters > SIZE_MAX / sizeof(uint64_t)) {
        fprintf(stderr, "iteration count is too large\n");
        return 2;
    }

    uint64_t *samples = malloc((size_t)iters * sizeof(*samples));
    if (!samples) {
        perror("malloc samples");
        return 1;
    }

    int pair[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        perror("socketpair");
        free(samples);
        return 1;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        close(pair[0]);
        close(pair[1]);
        free(samples);
        return 1;
    }

    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLET,  /* ET — each write is one edge */
        .data.fd = pair[0]
    };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair[0], &ev) != 0) {
        perror("epoll_ctl");
        wepoll_close(epfd);
        close(pair[0]);
        close(pair[1]);
        free(samples);
        return 1;
    }

    /* Warm-up. */
    for (int i = 0; i < 1000; i++) {
        char c = 'x';
        if (transfer_byte(pair[1], &c, 1) != 0 ||
            wait_for_byte(epfd, pair[0]) != 0 ||
            transfer_byte(pair[0], &c, 0) != 0) {
            perror("warm-up iteration");
            wepoll_close(epfd);
            close(pair[0]);
            close(pair[1]);
            free(samples);
            return 1;
        }
    }

    /* Measurement. */
    long double total_ns = 0;
    uint64_t max_ns = 0;
    uint64_t min_ns = UINT64_MAX;
    uint64_t completed = 0;
    int failed = 0;

    for (uint64_t i = 0; i < iters; i++) {
        char c = 'x';
        uint64_t t0 = now_ns();
        if (t0 == 0 || transfer_byte(pair[1], &c, 1) != 0 ||
            wait_for_byte(epfd, pair[0]) != 0) {
            fprintf(stderr, "iteration %llu failed before delivery: %s\n",
                    (unsigned long long)i, strerror(errno));
            failed = 1;
            break;
        }
        uint64_t t1 = now_ns();
        if (t1 == 0 || transfer_byte(pair[0], &c, 0) != 0) {
            fprintf(stderr, "iteration %llu failed after delivery: %s\n",
                    (unsigned long long)i, strerror(errno));
            failed = 1;
            break;
        }
        uint64_t dt = t1 - t0;
        samples[completed++] = dt;
        total_ns += dt;
        if (dt > max_ns) max_ns = dt;
        if (dt < min_ns) min_ns = dt;
    }

    if (completed == 0) {
        fprintf(stderr, "no benchmark iterations completed\n");
        failed = 1;
        goto cleanup;
    }

    qsort(samples, (size_t)completed, sizeof(*samples), compare_u64);
    size_t p50 = percentile_index((size_t)completed, 50, 100);
    size_t p95 = percentile_index((size_t)completed, 95, 100);
    size_t p99 = percentile_index((size_t)completed, 99, 100);
    double avg_ns = (double)(total_ns / (long double)completed);
    printf("wepoll-ex round-trip latency over %llu iterations:\n",
           (unsigned long long)completed);
    printf("  min : %.0f ns (%.3f us)\n", (double)min_ns, min_ns / 1000.0);
    printf("  avg : %.0f ns (%.3f us)\n", avg_ns,           avg_ns / 1000.0);
    printf("  p50 : %.0f ns (%.3f us)\n",
           (double)samples[p50], samples[p50] / 1000.0);
    printf("  p95 : %.0f ns (%.3f us)\n",
           (double)samples[p95], samples[p95] / 1000.0);
    printf("  p99 : %.0f ns (%.3f us)\n",
           (double)samples[p99], samples[p99] / 1000.0);
    printf("  max : %.0f ns (%.3f us)\n", (double)max_ns, max_ns / 1000.0);
    if (total_ns > 0) {
        printf("  rate: %.0f ops/sec\n",
               (double)((long double)completed * 1e9L / total_ns));
    } else {
        printf("  rate: unavailable (clock resolution)\n");
    }
    if (completed != iters) {
        fprintf(stderr, "benchmark stopped after %llu of %llu iterations\n",
                (unsigned long long)completed,
                (unsigned long long)iters);
        failed = 1;
    }

cleanup:
    if (wepoll_close(epfd) != 0) {
        perror("wepoll_close");
        failed = 1;
    }
    if (close(pair[0]) != 0) {
        perror("close socketpair read end");
        failed = 1;
    }
    if (close(pair[1]) != 0) {
        perror("close socketpair");
        failed = 1;
    }
    free(samples);
    return failed ? 1 : 0;
}
