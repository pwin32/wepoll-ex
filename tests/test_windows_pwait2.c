/* Deterministic Windows epoll_pwait2 timeout-path regressions. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_SKIP 77

typedef struct wait_context {
    ep_port_t *port;
    ep_wait_timeout_t timeout;
    epoll_event_ex event;
    HANDLE started;
    HANDLE done;
    int result;
    int error;
} wait_context_t;

static DWORD WINAPI wait_thread(void *parameter)
{
    wait_context_t *context = (wait_context_t *)parameter;

    (void)SetEvent(context->started);
    errno = 0;
    context->result = ep_port_wait_timeout(
        context->port, &context->event, 1, &context->timeout, NULL);
    context->error = errno;
    (void)SetEvent(context->done);
    return 0;
}

static int start_wait(wait_context_t *context, HANDLE *thread_out,
                      ep_port_t *port, const struct timespec *timespec)
{
    memset(context, 0, sizeof(*context));
    context->port = port;
    context->result = -2;
    context->started = CreateEventW(NULL, TRUE, FALSE, NULL);
    context->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (context->started == NULL || context->done == NULL ||
        ep_wait_timeout_from_timespec(timespec, &context->timeout) != 0) {
        return -1;
    }
    *thread_out = CreateThread(NULL, 0, wait_thread, context, 0, NULL);
    if (*thread_out == NULL ||
        WaitForSingleObject(context->started, 2000) != WAIT_OBJECT_0) {
        return -1;
    }
    return 0;
}

/* Return 1 when the precise timer arms, 0 when the coarse fallback completes,
 * and -1 when neither state becomes observable within the test deadline. */
static int wait_for_timer_or_completion(wait_context_t *context)
{
    ULONGLONG deadline = GetTickCount64() + 2000;

    do {
        if (atomic_load_explicit(&context->port->precise_timeout_armed,
                                 memory_order_acquire)) {
            return 1;
        }
        if (WaitForSingleObject(context->done, 0) == WAIT_OBJECT_0) {
            return 0;
        }
        Sleep(1);
    } while (GetTickCount64() < deadline);
    return -1;
}

static void close_wait_context(wait_context_t *context, HANDLE thread)
{
    if (thread != NULL) {
        (void)WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
    }
    if (context->done != NULL) CloseHandle(context->done);
    if (context->started != NULL) CloseHandle(context->started);
}

static int check_timeout(const char *name, const struct timespec *timespec,
                         uint64_t milliseconds, uint64_t intervals_100ns,
                         int infinite, int precise)
{
    ep_wait_timeout_t timeout;

    errno = 0;
    if (ep_wait_timeout_from_timespec(timespec, &timeout) != 0 ||
        timeout.milliseconds != milliseconds ||
        timeout.intervals_100ns != intervals_100ns ||
        timeout.infinite != infinite || timeout.precise != precise) {
        fprintf(stderr,
                "%s: ms=%llu ticks=%llu infinite=%u precise=%u errno=%d\n",
                name, (unsigned long long)timeout.milliseconds,
                (unsigned long long)timeout.intervals_100ns,
                (unsigned)timeout.infinite, (unsigned)timeout.precise, errno);
        return -1;
    }
    return 0;
}

static int test_conversion(void)
{
    struct timespec one_ns = { 0, 1 };
    struct timespec hundred_ns = { 0, 100 };
    struct timespec hundred_one_ns = { 0, 101 };
    struct timespec one_ms_one_ns = { 0, 1000001 };
    struct timespec one_second = { 1, 0 };
    struct timespec long_wait = { INT_MAX, 0 };
    struct timespec precise_overflow = {
        (time_t)(UINT64_MAX / UINT64_C(10000000) + UINT64_C(1)), 0
    };
    struct timespec invalid = { -1, 0 };
    ep_wait_timeout_t timeout;

    if (check_timeout("infinite", NULL, 0, 0, 1, 0) != 0 ||
        check_timeout("one ns", &one_ns, 1, 1, 0, 1) != 0 ||
        check_timeout("hundred ns", &hundred_ns, 1, 1, 0, 1) != 0 ||
        check_timeout("hundred-one ns", &hundred_one_ns, 1, 2, 0, 1) != 0 ||
        check_timeout("one ms plus one ns", &one_ms_one_ns,
                      2, UINT64_C(10001), 0, 1) != 0 ||
        check_timeout("one second", &one_second,
                      UINT64_C(1000), UINT64_C(10000000), 0, 1) != 0 ||
        check_timeout("long wait", &long_wait,
                      (uint64_t)INT_MAX * UINT64_C(1000),
                      (uint64_t)INT_MAX * UINT64_C(10000000), 0, 1) != 0 ||
        check_timeout("precise overflow", &precise_overflow,
                      (uint64_t)precise_overflow.tv_sec * UINT64_C(1000),
                      UINT64_MAX, 0, 0) != 0) {
        return -1;
    }

    errno = 0;
    if (ep_wait_timeout_from_timespec(&invalid, &timeout) != -1 ||
        errno != EINVAL) {
        return -1;
    }
    invalid.tv_sec = 0;
    invalid.tv_nsec = 1000000000L;
    errno = 0;
    if (ep_wait_timeout_from_timespec(&invalid, &timeout) != -1 ||
        errno != EINVAL) {
        return -1;
    }
    ep_wait_timeout_from_milliseconds(-1, &timeout);
    if (!timeout.infinite || timeout.precise) return -1;
    ep_wait_timeout_from_milliseconds(INT_MAX, &timeout);
    if (timeout.infinite || timeout.precise ||
        timeout.milliseconds != (uint64_t)INT_MAX ||
        timeout.intervals_100ns !=
            (uint64_t)INT_MAX * UINT64_C(10000)) {
        return -1;
    }
    puts("conversion: OK");
    return 0;
}

static int test_generation_deadline(void)
{
    const struct timespec duration = { 0, 250000000L };
    wait_context_t context;
    ep_port_t *port = NULL;
    HANDLE thread = NULL;
    ULONGLONG started = GetTickCount64();
    int timer_state;
    int result = -1;

    memset(&context, 0, sizeof(context));
    if (ep_port_create(0, 0, &port) != 0 ||
        start_wait(&context, &thread, port, &duration) != 0) {
        goto cleanup;
    }
    timer_state = wait_for_timer_or_completion(&context);
    if (timer_state < 0) goto cleanup;
    if (timer_state == 0) {
        result = TEST_SKIP;
        goto cleanup;
    }
    {
        ULONG_PTR generation = atomic_load_explicit(
            &port->precise_timeout_active_generation, memory_order_acquire);
        ULONG_PTR stale = generation == (ULONG_PTR)1
            ? (ULONG_PTR)2 : generation - (ULONG_PTR)1;

        if (generation == 0 ||
            !PostQueuedCompletionStatus(
                port->iocp, 0, stale, &port->precise_timeout_overlapped)) {
            goto cleanup;
        }
        Sleep(20);
        if (WaitForSingleObject(context.done, 0) == WAIT_OBJECT_0 ||
            !PostQueuedCompletionStatus(
                port->iocp, 0, generation,
                &port->precise_timeout_overlapped)) {
            goto cleanup;
        }
        Sleep(30);
        if (WaitForSingleObject(context.done, 0) == WAIT_OBJECT_0) {
            goto cleanup;
        }
    }
    if (WaitForSingleObject(context.done, 3000) != WAIT_OBJECT_0 ||
        context.result != 0 || GetTickCount64() - started < 150 ||
        atomic_load_explicit(&port->precise_timeout_armed,
                             memory_order_acquire) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    close_wait_context(&context, thread);
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    if (result == 0) {
        puts("generation-deadline: OK");
    } else if (result == TEST_SKIP) {
        puts("generation-deadline: SKIPPED (high-resolution timer unavailable)");
    }
    return result;
}

static int test_readiness_wins(void)
{
    const struct timespec duration = { 1, 0 };
    wait_context_t context;
    ep_port_t *port = NULL;
    HANDLE event_handle = NULL;
    HANDLE thread = NULL;
    epoll_data_t data;
    int registered = 0;
    int timer_state;
    int result = -1;

    memset(&context, 0, sizeof(context));
    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0x707761697432);
    event_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (event_handle == NULL || ep_port_create(0, 0, &port) != 0 ||
        ep_port_register(port, (SOCKET)event_handle, EPOLLIN, 0,
                         data, NULL) != 0) {
        goto cleanup;
    }
    registered = 1;
    if (start_wait(&context, &thread, port, &duration) != 0) goto cleanup;
    timer_state = wait_for_timer_or_completion(&context);
    if (timer_state < 0) goto cleanup;
    if (timer_state == 0) {
        result = TEST_SKIP;
        goto cleanup;
    }
    {
        ULONG_PTR generation = atomic_load_explicit(
            &port->precise_timeout_active_generation, memory_order_acquire);

        if (generation == 0 ||
            !PostQueuedCompletionStatus(
                port->iocp, 0, generation,
                &port->precise_timeout_overlapped)) {
            goto cleanup;
        }
    }
    if (!SetEvent(event_handle) ||
        WaitForSingleObject(context.done, 3000) != WAIT_OBJECT_0 ||
        context.result != 1 || context.event.data.u64 != data.u64 ||
        (context.event.events & EPOLLIN) == 0) {
        fprintf(stderr,
                "readiness: timer=%d result=%d error=%d data=%llu "
                "events=0x%08lx WSA=%lu\n",
                timer_state, context.result, context.error,
                (unsigned long long)context.event.data.u64,
                (unsigned long)context.event.events,
                (unsigned long)WSAGetLastError());
        fprintf(stderr,
                "readiness timer state: capability=%u create=%p clock=%p "
                "generation=%llu post-failures=%llu\n",
                (unsigned)port->precise_timeout_capability,
                (void *)(uintptr_t)port->create_waitable_timer_ex_w,
                (void *)(uintptr_t)port->query_unbiased_interrupt_time_precise,
                (unsigned long long)port->precise_timeout_generation,
                (unsigned long long)atomic_load_explicit(
                    &port->precise_timeout_post_failures,
                    memory_order_relaxed));
        goto cleanup;
    }
    result = 0;

cleanup:
    close_wait_context(&context, thread);
    if (registered &&
        ep_port_unregister(port, (SOCKET)event_handle) != 0) {
        fprintf(stderr, "readiness unregister: errno=%d WSA=%lu\n",
                errno, (unsigned long)WSAGetLastError());
        result = -1;
    }
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    if (event_handle != NULL) CloseHandle(event_handle);
    if (result == 0) {
        puts("readiness-wins: OK");
    } else if (result == TEST_SKIP) {
        puts("readiness-wins: SKIPPED (high-resolution timer unavailable)");
    }
    return result;
}

static int test_close(void)
{
    const struct timespec duration = { 10, 0 };
    wait_context_t context;
    ep_port_t *port = NULL;
    HANDLE thread = NULL;
    int result = -1;

    memset(&context, 0, sizeof(context));
    if (ep_port_create(0, 0, &port) != 0 ||
        start_wait(&context, &thread, port, &duration) != 0) {
        goto cleanup;
    }
    Sleep(20);
    ep_port_begin_close(port);
    if (WaitForSingleObject(context.done, 3000) != WAIT_OBJECT_0 ||
        context.result != -1 || context.error != EBADF ||
        atomic_load_explicit(&port->precise_timeout_armed,
                             memory_order_acquire) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    close_wait_context(&context, thread);
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    if (result == 0) puts("close: OK");
    return result;
}

static int test_fallback(void)
{
    const struct timespec duration = { 0, 2000000L };
    ep_wait_timeout_t timeout;
    epoll_event_ex event;
    ep_port_t *port = NULL;
    int result = -1;

    if (ep_port_create(0, 0, &port) != 0 ||
        ep_wait_timeout_from_timespec(&duration, &timeout) != 0) {
        goto cleanup;
    }
    port->precise_timeout_capability =
        EP_TIMEOUT_CAPABILITY_UNAVAILABLE;
    errno = 0;
    if (ep_port_wait_timeout(port, &event, 1, &timeout, NULL) != 0 ||
        port->precise_timeout_generation != 0 ||
        port->precise_timeout_timer != NULL || errno != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    if (result == 0) puts("fallback: OK");
    return result;
}

static int run_mode(const char *mode)
{
    if (strcmp(mode, "conversion") == 0) return test_conversion();
    if (strcmp(mode, "generation-deadline") == 0)
        return test_generation_deadline();
    if (strcmp(mode, "readiness-wins") == 0)
        return test_readiness_wins();
    if (strcmp(mode, "close") == 0) return test_close();
    if (strcmp(mode, "fallback") == 0) return test_fallback();
    return -2;
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        int result = run_mode(argv[1]);

        if (result != -2) {
            if (result == TEST_SKIP) return TEST_SKIP;
            return result == 0 ? 0 : 1;
        }
    }

    fprintf(stderr,
            "usage: %s [conversion|generation-deadline|readiness-wins|"
            "close|fallback]\n",
            argv[0]);
    return 1;
}
