/*
 * bench_wait_scaling.c — measure extension-wait overhead as registrations grow.
 *
 * The ordinary latency benchmark calls the host epoll_wait implementation on
 * POSIX.  This benchmark exercises epoll_wait_ex so changes to its temporary
 * storage and metadata lookup are measurable.
 *
 * Run with: ./bench_wait_scaling [registrations] [iterations]
 * Defaults:  1024 registrations and 20000 iterations.
 */
#define _POSIX_C_SOURCE 200809L

#include "wepoll_ex.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

enum {
    DEFAULT_REGISTRATIONS = 1024,
    DEFAULT_ITERATIONS = 20000,
    WARMUP_ITERATIONS = 1000,
    MAX_REGISTRATIONS = 100000
};

static uint64_t now_ns(void)
{
    struct timespec time;

    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) +
           (uint64_t)time.tv_nsec;
}

static uint64_t parse_count(const char *text, uint64_t fallback)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL) return fallback;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0) {
        return fallback;
    }
    return (uint64_t)value;
}

static int eventfd_write_one(int fd)
{
    uint64_t value = 1;
    ssize_t result;

    do {
        result = write(fd, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
    return result == (ssize_t)sizeof(value) ? 0 : -1;
}

static int eventfd_drain(int fd)
{
    uint64_t value;
    ssize_t result;

    do {
        result = read(fd, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
    return result == (ssize_t)sizeof(value) ? 0 : -1;
}

int main(int argc, char **argv)
{
    uint64_t registration_count = parse_count(
        argc > 1 ? argv[1] : NULL, DEFAULT_REGISTRATIONS);
    uint64_t iterations = parse_count(
        argc > 2 ? argv[2] : NULL, DEFAULT_ITERATIONS);
    int *fds = NULL;
    uintptr_t *contexts = NULL;
    int epfd = -1;
    int result = 1;

    if (registration_count > MAX_REGISTRATIONS) {
        fprintf(stderr, "registration count must be <= %d\n",
                MAX_REGISTRATIONS);
        return 2;
    }

    fds = (int *)malloc((size_t)registration_count * sizeof(*fds));
    contexts = (uintptr_t *)malloc(
        (size_t)registration_count * sizeof(*contexts));
    if (fds == NULL || contexts == NULL) {
        perror("malloc");
        goto cleanup;
    }
    for (uint64_t i = 0; i < registration_count; i++) fds[i] = -1;

    epfd = epoll_create_ex((int)registration_count, EPOLL_CLOEXEC);
    if (epfd < 0) {
        perror("epoll_create_ex");
        goto cleanup;
    }

    for (uint64_t i = 0; i < registration_count; i++) {
        struct epoll_event event = {
            .events = EPOLLIN,
            .data.u64 = i + 1
        };

        contexts[i] = (uintptr_t)(i + 1);
        fds[i] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (fds[i] < 0 ||
            epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, fds[i], &event,
                          &contexts[i]) != 0) {
            perror(fds[i] < 0 ? "eventfd" : "epoll_ctl_ctx");
            goto cleanup;
        }
    }

    if (epoll_fd_count(epfd) != (int)registration_count) {
        fprintf(stderr, "unexpected extension registration count\n");
        goto cleanup;
    }

    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        struct epoll_event_ex output;

        if (epoll_wait_ex(epfd, &output, 1, 0) != 0) {
            fprintf(stderr, "warmup returned an unexpected event\n");
            goto cleanup;
        }
    }

    uint64_t started = now_ns();
    for (uint64_t i = 0; i < iterations; i++) {
        struct epoll_event_ex output;

        if (epoll_wait_ex(epfd, &output, 1, 0) != 0) {
            fprintf(stderr, "empty wait returned an unexpected event\n");
            goto cleanup;
        }
    }
    uint64_t empty_elapsed = now_ns() - started;

    uint64_t ready_iterations = iterations > 10000 ? 10000 : iterations;
    uint64_t ready_index = registration_count - 1;
    started = now_ns();
    for (uint64_t i = 0; i < ready_iterations; i++) {
        struct epoll_event_ex output;

        if (eventfd_write_one(fds[ready_index]) != 0) {
            perror("write(eventfd)");
            goto cleanup;
        }
        int count = epoll_wait_ex(epfd, &output, 1, 1000);
        if (count != 1 || output.data.u64 != ready_index + 1 ||
            output.user_ctx != &contexts[ready_index] ||
            (output.events & EPOLLIN) == 0) {
            fprintf(stderr,
                    "ready wait mismatch: count=%d data=%llu ctx=%p events=0x%x\n",
                    count, (unsigned long long)output.data.u64,
                    output.user_ctx, output.events);
            goto cleanup;
        }
        if (eventfd_drain(fds[ready_index]) != 0) {
            perror("read(eventfd)");
            goto cleanup;
        }
    }
    uint64_t ready_elapsed = now_ns() - started;

    printf("wepoll-ex extension wait scaling with %llu registrations:\n",
           (unsigned long long)registration_count);
    printf("  empty: %.0f ns/wait over %llu iterations\n",
           (double)empty_elapsed / (double)iterations,
           (unsigned long long)iterations);
    printf("  ready: %.0f ns/event over %llu iterations\n",
           (double)ready_elapsed / (double)ready_iterations,
           (unsigned long long)ready_iterations);
    result = 0;

cleanup:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (fds != NULL) {
        for (uint64_t i = 0; i < registration_count; i++) {
            if (fds[i] >= 0) (void)close(fds[i]);
        }
    }
    free(contexts);
    free(fds);
    return result;
}
