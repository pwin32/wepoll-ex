/*
 * test_posix_large_wait.c -- extended POSIX waits above the former 4096 cap.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "wepoll_ex.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

enum {
    LARGE_WAIT_EVENT_COUNT = 4097,
    LARGE_WAIT_FD_LIMIT = 8192,
    TEST_SKIP = 77
};

#define LARGE_WAIT_DATA_BASE UINT64_C(0x5745504f4c4c0000)

typedef struct large_wait_context {
    uint64_t marker;
    size_t index;
} large_wait_context_t;

typedef int (*large_wait_fn_t)(int epfd, struct epoll_event_ex *events,
                               int maxevents, void *argument);

static int wait_with_milliseconds(int epfd, struct epoll_event_ex *events,
                                  int maxevents, void *argument)
{
    (void)argument;
    return epoll_wait_ex(epfd, events, maxevents, 1000);
}

static int wait_with_timespec(int epfd, struct epoll_event_ex *events,
                              int maxevents, void *argument)
{
    const sigset_t *sigmask = argument;
    const struct timespec timeout = { 1, 0 };

    return epoll_pwait2_ex(epfd, events, maxevents, &timeout, sigmask);
}

static int prepare_fd_limit(struct rlimit *original)
{
    if (getrlimit(RLIMIT_NOFILE, original) != 0) {
        fprintf(stderr, "getrlimit(RLIMIT_NOFILE): %s\n", strerror(errno));
        return -1;
    }

    if (original->rlim_cur >= LARGE_WAIT_FD_LIMIT) return 0;
    if (original->rlim_max < LARGE_WAIT_FD_LIMIT) {
        printf("SKIP: RLIMIT_NOFILE hard limit is below %d\n",
               LARGE_WAIT_FD_LIMIT);
        return TEST_SKIP;
    }

    struct rlimit raised = *original;
    raised.rlim_cur = LARGE_WAIT_FD_LIMIT;
    if (setrlimit(RLIMIT_NOFILE, &raised) != 0) {
        printf("SKIP: cannot raise RLIMIT_NOFILE to %d: %s\n",
               LARGE_WAIT_FD_LIMIT, strerror(errno));
        return TEST_SKIP;
    }
    return 0;
}

static void restore_fd_limit(const struct rlimit *original)
{
    if (setrlimit(RLIMIT_NOFILE, original) != 0) {
        fprintf(stderr, "warning: cannot restore RLIMIT_NOFILE: %s\n",
                strerror(errno));
    }
}

static void initialize_output_guards(struct epoll_event_ex *storage,
                                     size_t storage_count)
{
    memset(storage, 0xcc, storage_count * sizeof(*storage));
    memset(&storage[0], 0xa5, sizeof(storage[0]));
    memset(&storage[storage_count - 1], 0x5a,
           sizeof(storage[storage_count - 1]));
}

static int verify_large_batch(const char *name,
                              struct epoll_event_ex *storage,
                              large_wait_context_t *contexts,
                              unsigned char *seen)
{
    const size_t storage_count = LARGE_WAIT_EVENT_COUNT + 2U;
    struct epoll_event_ex first_guard;
    struct epoll_event_ex last_guard;

    memset(&first_guard, 0xa5, sizeof(first_guard));
    memset(&last_guard, 0x5a, sizeof(last_guard));
    if (memcmp(&storage[0], &first_guard, sizeof(first_guard)) != 0 ||
        memcmp(&storage[storage_count - 1], &last_guard,
               sizeof(last_guard)) != 0) {
        fprintf(stderr, "%s overwrote an output guard\n", name);
        return -1;
    }

    memset(seen, 0, LARGE_WAIT_EVENT_COUNT);
    struct epoll_event_ex *events = &storage[1];
    const uint32_t required_flags = WEPOLL_FLAG_ONESHOT_FIRED |
                                    WEPOLL_FLAG_ET_DELIVERED |
                                    WEPOLL_FLAG_EDGE_ARMED;
    for (size_t i = 0; i < LARGE_WAIT_EVENT_COUNT; i++) {
        uint64_t data = events[i].data.u64;
        if (data < LARGE_WAIT_DATA_BASE ||
            data >= LARGE_WAIT_DATA_BASE + LARGE_WAIT_EVENT_COUNT) {
            fprintf(stderr, "%s returned unexpected data at %zu: 0x%"
                    PRIx64 "\n", name, i, data);
            return -1;
        }

        size_t index = (size_t)(data - LARGE_WAIT_DATA_BASE);
        if (seen[index]) {
            fprintf(stderr, "%s returned duplicate index %zu\n",
                    name, index);
            return -1;
        }
        seen[index] = 1;

        if ((events[i].events & EPOLLIN) == 0 ||
            (events[i].flags & required_flags) != required_flags ||
            events[i].user_ctx != &contexts[index]) {
            fprintf(stderr,
                    "%s metadata mismatch at output %zu (index %zu)\n",
                    name, i, index);
            return -1;
        }
    }
    return 0;
}

static int run_large_wait(const char *name, int epfd,
                          struct epoll_event_ex *storage,
                          large_wait_context_t *contexts,
                          unsigned char *seen, large_wait_fn_t wait_fn,
                          void *argument)
{
    const size_t storage_count = LARGE_WAIT_EVENT_COUNT + 2U;

    initialize_output_guards(storage, storage_count);
    int count = wait_fn(epfd, &storage[1], LARGE_WAIT_EVENT_COUNT, argument);
    if (count != LARGE_WAIT_EVENT_COUNT) {
        fprintf(stderr, "%s returned %d instead of %d: %s\n",
                name, count, LARGE_WAIT_EVENT_COUNT,
                count < 0 ? strerror(errno) : "short batch");
        return -1;
    }

    return verify_large_batch(name, storage, contexts, seen);
}

int main(void)
{
    struct rlimit original_limit;
    int limit_result = prepare_fd_limit(&original_limit);
    if (limit_result != 0) return limit_result;

    int result = 1;
    int epfd = -1;
    int *fds = malloc(LARGE_WAIT_EVENT_COUNT * sizeof(*fds));
    if (fds != NULL) {
        for (size_t i = 0; i < LARGE_WAIT_EVENT_COUNT; i++) fds[i] = -1;
    }
    large_wait_context_t *contexts = calloc(LARGE_WAIT_EVENT_COUNT,
                                             sizeof(*contexts));
    unsigned char *seen = calloc(LARGE_WAIT_EVENT_COUNT, 1);
    struct epoll_event_ex *storage = malloc(
        (LARGE_WAIT_EVENT_COUNT + 2U) * sizeof(*storage));
    if (fds == NULL || contexts == NULL || seen == NULL || storage == NULL) {
        fprintf(stderr, "large-wait allocation: %s\n", strerror(errno));
        goto cleanup;
    }

    epfd = epoll_create_ex(LARGE_WAIT_EVENT_COUNT, EPOLL_CLOEXEC);
    if (epfd < 0) {
        fprintf(stderr, "epoll_create_ex: %s\n", strerror(errno));
        goto cleanup;
    }

    for (size_t i = 0; i < LARGE_WAIT_EVENT_COUNT; i++) {
        fds[i] = eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK);
        if (fds[i] < 0) {
            if (errno == EMFILE || errno == ENFILE) {
                printf("SKIP: eventfd resources exhausted at %zu: %s\n",
                       i, strerror(errno));
                result = TEST_SKIP;
            } else {
                fprintf(stderr, "eventfd[%zu]: %s\n", i, strerror(errno));
            }
            goto cleanup;
        }

        contexts[i].marker = UINT64_C(0xc07e570000000000) | i;
        contexts[i].index = i;
        struct epoll_event event = {0};
        event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        event.data.u64 = LARGE_WAIT_DATA_BASE + i;
        if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, fds[i], &event,
                          &contexts[i]) != 0) {
            fprintf(stderr, "epoll_ctl_ctx ADD[%zu]: %s\n",
                    i, strerror(errno));
            goto cleanup;
        }
    }

    if (run_large_wait("epoll_wait_ex", epfd, storage, contexts, seen,
                       wait_with_milliseconds, NULL) != 0) {
        goto cleanup;
    }

    for (size_t i = 0; i < LARGE_WAIT_EVENT_COUNT; i++) {
        if (epoll_rearm(epfd, fds[i]) != 0) {
            fprintf(stderr, "epoll_rearm[%zu]: %s\n", i, strerror(errno));
            goto cleanup;
        }
    }

    sigset_t empty_mask;
    if (sigemptyset(&empty_mask) != 0) {
        fprintf(stderr, "sigemptyset: %s\n", strerror(errno));
        goto cleanup;
    }
    if (run_large_wait("epoll_pwait2_ex", epfd, storage, contexts, seen,
                       wait_with_timespec, &empty_mask) != 0) {
        goto cleanup;
    }

    puts("POSIX extended waits returned 4097 unique guarded events: OK");
    result = 0;

cleanup:
    if (epfd >= 0) wepoll_close(epfd);
    if (fds != NULL) {
        for (size_t i = 0; i < LARGE_WAIT_EVENT_COUNT; i++) {
            if (fds[i] >= 0) close(fds[i]);
        }
    }
    free(storage);
    free(seen);
    free(contexts);
    free(fds);
    restore_fd_limit(&original_limit);
    return result;
}
