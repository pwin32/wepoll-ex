#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32

static int wait_one(int epfd, int timeout_ms, uint32_t *events_out)
{
    struct epoll_event events[1];
    int n = epoll_wait(epfd, events, 1, timeout_ms);
    if (n < 0)
        return -1;
    if (n == 0)
        return 0;
    if (events_out)
        *events_out = events[0].events;
    return 1;
}

typedef LONG (NTAPI *test_nt_query_semaphore_fn)(
    HANDLE semaphore,
    ULONG information_class,
    PVOID information,
    ULONG information_length,
    ULONG *return_length);

typedef struct test_semaphore_basic_information {
    LONG current_count;
    LONG maximum_count;
} test_semaphore_basic_information_t;

static int wait_for_semaphore_count(HANDLE semaphore, LONG expected,
                                    DWORD timeout_ms)
{
    test_nt_query_semaphore_fn query = NULL;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    FARPROC proc;
    uint64_t deadline;
    size_t copy_size;

    if (ntdll == NULL)
        return 0;
    proc = GetProcAddress(ntdll, "NtQuerySemaphore");
    if (proc == NULL)
        return 0;
    copy_size = sizeof(query) < sizeof(proc) ? sizeof(query) : sizeof(proc);
    memcpy(&query, &proc, copy_size);
    if (query == NULL)
        return 0;

    deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        test_semaphore_basic_information_t info;
        ULONG return_length = 0;
        LONG status;

        memset(&info, 0, sizeof(info));
        status = query(semaphore, 0, &info, (ULONG)sizeof(info),
                       &return_length);
        if (status < 0)
            return 0;
        if (info.current_count == expected)
            return 1;
        if (GetTickCount64() >= deadline)
            return 0;
        Sleep(1);
    }
}

static int test_event_handle(void)
{
    HANDLE event_handle = NULL;
    struct epoll_event event;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    event_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (event_handle == NULL) {
        fputs("waitable: CreateEvent failed\n", stderr);
        return 1;
    }

    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = 7;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)event_handle, &event) != 0) {
        fprintf(stderr, "waitable: ADD event failed errno=%d\n", errno);
        goto fail;
    }

    n = wait_one(epfd, 30, &events);
    if (n != 0) {
        fprintf(stderr, "waitable: unsignaled event produced n=%d events=%u\n",
                n, events);
        goto fail;
    }

    if (!SetEvent(event_handle))
        goto fail;

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr, "waitable: signaled event n=%d events=%u\n", n, events);
        goto fail;
    }

    /* Level-triggered: remains ready while still signaled. */
    n = wait_one(epfd, 50, &events);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr, "waitable: level redelivery missing n=%d\n", n);
        goto fail;
    }

    if (!ResetEvent(event_handle))
        goto fail;

    n = wait_one(epfd, 30, &events);
    if (n != 0) {
        fprintf(stderr, "waitable: reset event still ready n=%d\n", n);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(event_handle);
    puts("waitable-event: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (event_handle) CloseHandle(event_handle);
    return 1;
}

static int test_event_handle_et(void)
{
    HANDLE event_handle = NULL;
    struct epoll_event event;
    wepoll_ex_stats stats_before;
    wepoll_ex_stats stats_after;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    event_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (event_handle == NULL)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    memset(&stats_before, 0, sizeof(stats_before));
    memset(&stats_after, 0, sizeof(stats_after));
    event.events = EPOLLIN | EPOLLET;
    event.data.u64 = 8;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)event_handle, &event) != 0)
        goto fail;

    if (!SetEvent(event_handle))
        goto fail;

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0)
        goto fail;

    if (wepoll_ex_get_stats(epfd, &stats_before, sizeof(stats_before)) != 0)
        goto fail;
    n = wait_one(epfd, 30, &events);
    if (n != 0) {
        fprintf(stderr, "waitable-et: redelivery n=%d\n", n);
        goto fail;
    }
    if (wepoll_ex_get_stats(epfd, &stats_after, sizeof(stats_after)) != 0 ||
        stats_after.rearm_work_items - stats_before.rearm_work_items > 128) {
        fprintf(stderr,
                "waitable-et: excessive deferred rearm work before=%llu "
                "after=%llu\n",
                (unsigned long long)stats_before.rearm_work_items,
                (unsigned long long)stats_after.rearm_work_items);
        goto fail;
    }

    if (!ResetEvent(event_handle))
        goto fail;

    /* Let the backend observe the unsignaled gap before the next edge. */
    n = wait_one(epfd, 0, &events);
    if (n != 0) {
        fprintf(stderr, "waitable-et: post-reset wait n=%d\n", n);
        goto fail;
    }

    if (!SetEvent(event_handle))
        goto fail;

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr, "waitable-et: second edge missing n=%d\n", n);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(event_handle);
    puts("waitable-event-et: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (event_handle) CloseHandle(event_handle);
    return 1;
}

static int test_auto_reset_event(void)
{
    HANDLE event_handle = NULL;
    struct epoll_event event;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    event_handle = CreateEventW(NULL, FALSE, TRUE, NULL);
    if (event_handle == NULL)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)event_handle, &event) != 0)
        goto fail;

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr,
                "waitable-auto-reset: initial signal n=%d events=%u\n",
                n, events);
        goto fail;
    }
    if (wait_one(epfd, 30, &events) != 0)
        goto fail;
    if (!SetEvent(event_handle))
        goto fail;
    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr,
                "waitable-auto-reset: second signal n=%d events=%u\n",
                n, events);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(event_handle);
    puts("waitable-auto-reset: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (event_handle) CloseHandle(event_handle);
    return 1;
}

static int test_auto_reset_event_et(void)
{
    HANDLE event_handle = NULL;
    struct epoll_event event;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    event_handle = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (event_handle == NULL)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)event_handle, &event) != 0)
        goto fail;

    if (!SetEvent(event_handle))
        goto fail;
    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0)
        goto fail;
    if (wait_one(epfd, 30, &events) != 0)
        goto fail;

    if (!SetEvent(event_handle))
        goto fail;
    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr,
                "waitable-auto-reset-et: second signal n=%d events=%u\n",
                n, events);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(event_handle);
    puts("waitable-auto-reset-et: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (event_handle) CloseHandle(event_handle);
    return 1;
}

static int test_semaphore_handle(void)
{
    HANDLE semaphore = NULL;
    struct epoll_event event;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    semaphore = CreateSemaphoreW(NULL, 1, 4, NULL);
    if (semaphore == NULL)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)semaphore, &event) != 0)
        goto fail;

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0)
        goto fail;
    if (wait_one(epfd, 30, &events) != 0)
        goto fail;
    if (!ReleaseSemaphore(semaphore, 2, NULL))
        goto fail;
    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0)
        goto fail;
    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr,
                "waitable-semaphore: second count n=%d events=%u\n",
                n, events);
        goto fail;
    }
    if (wait_one(epfd, 30, &events) != 0)
        goto fail;

    (void)wepoll_close(epfd);
    CloseHandle(semaphore);
    puts("waitable-semaphore: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (semaphore) CloseHandle(semaphore);
    return 1;
}

static int test_semaphore_handle_et(void)
{
    HANDLE semaphore = NULL;
    struct epoll_event event;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    semaphore = CreateSemaphoreW(NULL, 2, 2, NULL);
    if (semaphore == NULL)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)semaphore, &event) != 0)
        goto fail;

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0)
        goto fail;
    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr,
                "waitable-semaphore-et: second count n=%d events=%u\n",
                n, events);
        goto fail;
    }
    if (wait_one(epfd, 30, &events) != 0) {
        fputs("waitable-semaphore-et: count was delivered more than twice\n",
              stderr);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(semaphore);
    puts("waitable-semaphore-et: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (semaphore) CloseHandle(semaphore);
    return 1;
}

static int test_waitable_pending_mod(void)
{
    HANDLE semaphore = NULL;
    struct epoll_event event;
    struct epoll_event output;
    LONG previous = -1;
    int epfd = -1;
    int n;

    semaphore = CreateSemaphoreW(NULL, 0, 1, NULL);
    if (semaphore == NULL)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = 10;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)semaphore, &event) != 0 ||
        epoll_wait(epfd, &output, 1, 0) != 0)
        goto fail;
    if (!ReleaseSemaphore(semaphore, 1, &previous) || previous != 0 ||
        !wait_for_semaphore_count(semaphore, 0, 1000)) {
        fputs("waitable-pending-mod: callback did not consume count\n",
              stderr);
        goto fail;
    }

    event.events = EPOLLIN | EPOLLET;
    event.data.u64 = 11;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD,
                  (epoll_fd_t)semaphore, &event) != 0)
        goto fail;
    n = epoll_wait(epfd, &output, 1, 1000);
    if (n != 1 || (output.events & EPOLLIN) == 0 ||
        output.data.u64 != 11) {
        fprintf(stderr,
                "waitable-pending-mod: n=%d events=%u data=%llu errno=%d\n",
                n, n > 0 ? output.events : 0,
                (unsigned long long)(n > 0 ? output.data.u64 : 0), errno);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(semaphore);
    puts("waitable-pending-mod: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (semaphore) CloseHandle(semaphore);
    return 1;
}

static int test_waitable_ready_mod(void)
{
    HANDLE semaphores[2] = { NULL, NULL };
    struct epoll_event event;
    struct epoll_event output;
    wepoll_ex_stats stats = {0};
    int epfd = -1;
    int other;
    int n;

    semaphores[0] = CreateSemaphoreW(NULL, 1, 1, NULL);
    semaphores[1] = CreateSemaphoreW(NULL, 1, 1, NULL);
    if (semaphores[0] == NULL || semaphores[1] == NULL)
        goto fail;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = 1;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)semaphores[0], &event) != 0)
        goto fail;
    event.data.u64 = 2;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)semaphores[1], &event) != 0)
        goto fail;

    n = epoll_wait(epfd, &output, 1, 1000);
    if (n != 1 || (output.data.u64 != 1 && output.data.u64 != 2))
        goto fail;
    if (wepoll_ex_get_stats(epfd, &stats, sizeof(stats)) != 0 ||
        stats.ready_queue_depth != 1) {
        fprintf(stderr,
                "waitable-ready-mod: expected one queued peer depth=%zu\n",
                stats.ready_queue_depth);
        goto fail;
    }
    other = output.data.u64 == 1 ? 1 : 0;

    event.events = EPOLLIN | EPOLLET;
    event.data.u64 = 3;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD,
                  (epoll_fd_t)semaphores[other], &event) != 0)
        goto fail;
    n = epoll_wait(epfd, &output, 1, 1000);
    if (n != 1 || (output.events & EPOLLIN) == 0 || output.data.u64 != 3) {
        fprintf(stderr,
                "waitable-ready-mod: n=%d events=%u data=%llu errno=%d\n",
                n, n > 0 ? output.events : 0,
                (unsigned long long)(n > 0 ? output.data.u64 : 0), errno);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(semaphores[0]);
    CloseHandle(semaphores[1]);
    puts("waitable-ready-mod: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (semaphores[0]) CloseHandle(semaphores[0]);
    if (semaphores[1]) CloseHandle(semaphores[1]);
    return 1;
}

static int test_timer_ready_mod(void)
{
    HANDLE timers[2] = { NULL, NULL };
    LARGE_INTEGER due;
    struct epoll_event event;
    struct epoll_event output;
    wepoll_ex_stats stats = {0};
    int epfd = -1;
    int other;
    int n;

    timers[0] = CreateWaitableTimerW(NULL, FALSE, NULL);
    timers[1] = CreateWaitableTimerW(NULL, FALSE, NULL);
    if (timers[0] == NULL || timers[1] == NULL)
        goto fail;
    due.QuadPart = -10000;
    if (!SetWaitableTimer(timers[0], &due, 0, NULL, NULL, FALSE) ||
        !SetWaitableTimer(timers[1], &due, 0, NULL, NULL, FALSE))
        goto fail;
    Sleep(10);

    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = 1;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)timers[0], &event) != 0)
        goto fail;
    event.data.u64 = 2;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)timers[1], &event) != 0)
        goto fail;

    n = epoll_wait(epfd, &output, 1, 1000);
    if (n != 1 || (output.data.u64 != 1 && output.data.u64 != 2))
        goto fail;
    if (wepoll_ex_get_stats(epfd, &stats, sizeof(stats)) != 0 ||
        stats.ready_queue_depth != 1) {
        fprintf(stderr,
                "waitable-timer-ready-mod: expected queued peer depth=%zu\n",
                stats.ready_queue_depth);
        goto fail;
    }
    other = output.data.u64 == 1 ? 1 : 0;

    event.data.u64 = 3;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD,
                  (epoll_fd_t)timers[other], &event) != 0)
        goto fail;
    n = epoll_wait(epfd, &output, 1, 1000);
    if (n != 1 || (output.events & EPOLLIN) == 0 || output.data.u64 != 3) {
        fprintf(stderr,
                "waitable-timer-ready-mod: n=%d events=%u data=%llu errno=%d\n",
                n, n > 0 ? output.events : 0,
                (unsigned long long)(n > 0 ? output.data.u64 : 0), errno);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(timers[0]);
    CloseHandle(timers[1]);
    puts("waitable-timer-ready-mod: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (timers[0]) CloseHandle(timers[0]);
    if (timers[1]) CloseHandle(timers[1]);
    return 1;
}

static int test_timer_et_rejected(void)
{
    HANDLE timer = NULL;
    LARGE_INTEGER due;
    struct epoll_event event;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    if (timer == NULL)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)timer, &event) != -1 ||
        errno != EINVAL) {
        fprintf(stderr, "waitable-timer-et: ADD errno=%d\n", errno);
        goto fail;
    }

    event.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)timer, &event) != 0)
        goto fail;
    event.events = EPOLLIN | EPOLLET;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, (epoll_fd_t)timer, &event) != -1 ||
        errno != EINVAL) {
        fprintf(stderr, "waitable-timer-et: MOD errno=%d\n", errno);
        goto fail;
    }

    due.QuadPart = -10000;
    if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        goto fail;
    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr,
                "waitable-timer-et: failed MOD changed LT registration n=%d\n",
                n);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(timer);
    puts("waitable-timer-et: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (timer) CloseHandle(timer);
    return 1;
}

static int test_job_rejected(void)
{
    HANDLE job = NULL;
    struct epoll_event event;
    int epfd = -1;

    job = CreateJobObjectW(NULL, NULL);
    if (job == NULL)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)job, &event) != -1 ||
        errno != EPERM) {
        fprintf(stderr, "waitable-job: expected EPERM errno=%d\n", errno);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(job);
    puts("waitable-job: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (job) CloseHandle(job);
    return 1;
}

static int test_waitable_access_rejected(void)
{
    HANDLE source = NULL;
    HANDLE modify_only = NULL;
    struct epoll_event event;
    int epfd = -1;
    int ctl_result;
    int error;
    int count;

    source = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (source == NULL ||
        !DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                         &modify_only, EVENT_MODIFY_STATE, FALSE, 0)) {
        goto fail;
    }
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    errno = 0;
    ctl_result = epoll_ctl(epfd, EPOLL_CTL_ADD,
                           (epoll_fd_t)modify_only, &event);
    error = errno;
    count = epoll_fd_count(epfd);
    if (ctl_result != -1 || error != EACCES || count != 0) {
        fprintf(stderr,
                "waitable-access: expected EACCES/no registration errno=%d "
                "count=%d\n",
                error, count);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(modify_only);
    CloseHandle(source);
    puts("waitable-access: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (modify_only != NULL) CloseHandle(modify_only);
    if (source != NULL) CloseHandle(source);
    return 1;
}

static DWORD WINAPI exit_thread(PVOID parameter)
{
    (void)parameter;
    return 0;
}

static int test_process_and_thread_handles(void)
{
    HANDLE process = NULL;
    HANDLE thread = NULL;
    struct epoll_event event;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                          FALSE, GetCurrentProcessId());
    thread = CreateThread(NULL, 0, exit_thread, NULL, CREATE_SUSPENDED, NULL);
    if (process == NULL || thread == NULL)
        goto fail;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)process, &event) != 0 ||
        wait_one(epfd, 0, &events) != 0 ||
        epoll_ctl(epfd, EPOLL_CTL_DEL,
                  (epoll_fd_t)process, NULL) != 0 ||
        epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)thread, &event) != 0 ||
        wait_one(epfd, 20, &events) != 0 ||
        ResumeThread(thread) == (DWORD)-1) {
        goto fail;
    }

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0 ||
        wait_one(epfd, 50, &events) != 1) {
        fprintf(stderr, "waitable-process-thread: completion n=%d events=%u\n",
                n, events);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(thread);
    CloseHandle(process);
    puts("waitable-process-thread: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (thread != NULL) {
        (void)ResumeThread(thread);
        (void)WaitForSingleObject(thread, 1000);
        CloseHandle(thread);
    }
    if (process != NULL) CloseHandle(process);
    return 1;
}

static int test_thread_handle_et_terminal(void)
{
    HANDLE thread = NULL;
    struct epoll_event event;
    wepoll_ex_stats stats_before = {0};
    wepoll_ex_stats stats_after = {0};
    uint32_t events = 0;
    int epfd = -1;
    int n;

    thread = CreateThread(NULL, 0, exit_thread, NULL, CREATE_SUSPENDED, NULL);
    if (thread == NULL)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)thread, &event) != 0 ||
        wait_one(epfd, 0, &events) != 0 ||
        ResumeThread(thread) == (DWORD)-1)
        goto fail;

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0 ||
        wepoll_ex_get_stats(epfd, &stats_before, sizeof(stats_before)) != 0)
        goto fail;
    n = wait_one(epfd, 50, &events);
    if (n != 0 ||
        wepoll_ex_get_stats(epfd, &stats_after, sizeof(stats_after)) != 0 ||
        stats_after.rearm_work_items != stats_before.rearm_work_items ||
        stats_after.rearm_queue_depth != 0) {
        fprintf(stderr,
                "waitable-thread-et: n=%d work_before=%llu work_after=%llu "
                "rearm=%zu\n",
                n,
                (unsigned long long)stats_before.rearm_work_items,
                (unsigned long long)stats_after.rearm_work_items,
                stats_after.rearm_queue_depth);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(thread);
    puts("waitable-thread-et: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (thread != NULL) {
        (void)ResumeThread(thread);
        (void)WaitForSingleObject(thread, 1000);
        CloseHandle(thread);
    }
    return 1;
}

static int test_waitable_exclusive_rejected(void)
{
    HANDLE event_handle = NULL;
    struct epoll_event event;
    int epfd = -1;

    event_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (event_handle == NULL)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLEXCLUSIVE;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)event_handle, &event) != -1 ||
        errno != EINVAL) {
        fprintf(stderr, "waitable exclusive: expected EINVAL errno=%d\n",
                errno);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(event_handle);
    puts("waitable-exclusive: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (event_handle) CloseHandle(event_handle);
    return 1;
}

static int test_mutex_rejected(void)
{
    HANDLE mutex = NULL;
    struct epoll_event event;
    int epfd = -1;

    mutex = CreateMutexW(NULL, FALSE, NULL);
    if (mutex == NULL)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)mutex, &event) != -1 || errno != EPERM) {
        fprintf(stderr, "waitable-mutex: expected EPERM errno=%d\n", errno);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(mutex);
    puts("waitable-mutex: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (mutex) CloseHandle(mutex);
    return 1;
}

static int test_waitable_cleanup(void)
{
    HANDLE event_handle = NULL;
    struct epoll_event event;
    DWORD before = 0;
    DWORD after = 0;
    uint32_t events = 0;
    int epfd = -1;
    int i;
    int n;

    event_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (event_handle == NULL)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)event_handle, &event) != 0)
        goto fail;

    /* Warm the process thread pool and leave one unsignaled registration
     * outstanding on both sides of the handle-count comparison. */
    if (wait_one(epfd, 0, &events) != 0 || !SetEvent(event_handle))
        goto fail;
    if (wait_one(epfd, 1000, &events) != 1 || !ResetEvent(event_handle))
        goto fail;
    if (wait_one(epfd, 0, &events) != 0)
        goto fail;
    if (!GetProcessHandleCount(GetCurrentProcess(), &before))
        goto fail;

    for (i = 0; i < 200; i++) {
        if (!SetEvent(event_handle))
            goto fail;
        n = wait_one(epfd, 1000, &events);
        if (n != 1 || (events & EPOLLIN) == 0) {
            fprintf(stderr,
                    "waitable-cleanup: delivery %d n=%d events=%u\n",
                    i, n, events);
            goto fail;
        }
        if (!ResetEvent(event_handle) || wait_one(epfd, 0, &events) != 0)
            goto fail;
    }

    if (!GetProcessHandleCount(GetCurrentProcess(), &after))
        goto fail;
    if (after > before + 4U) {
        fprintf(stderr,
                "waitable-cleanup: handle growth before=%lu after=%lu\n",
                (unsigned long)before, (unsigned long)after);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(event_handle);
    puts("waitable-cleanup: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (event_handle) CloseHandle(event_handle);
    return 1;
}

static int run_waitable_cancel_iteration(int signal_before_del)
{
    HANDLE event_handle = NULL;
    struct epoll_event event;
    uint32_t events = 0;
    int epfd = -1;
    int result = 1;

    event_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (event_handle == NULL)
        goto done;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto done;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)event_handle, &event) != 0)
        goto done;
    if (wait_one(epfd, 0, &events) != 0)
        goto done;
    if (signal_before_del) {
        if (!SetEvent(event_handle))
            goto done;
        Sleep(0);
    }
    if (epoll_ctl(epfd, EPOLL_CTL_DEL,
                  (epoll_fd_t)event_handle, NULL) != 0)
        goto done;
    result = 0;

done:
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = 1;
    if (event_handle) CloseHandle(event_handle);
    return result;
}

static int test_waitable_cancel_lifecycle(void)
{
    DWORD before = 0;
    DWORD after = 0;
    int i;

    for (i = 0; i < 8; i++) {
        if (run_waitable_cancel_iteration(i & 1) != 0)
            return 1;
    }
    if (!GetProcessHandleCount(GetCurrentProcess(), &before))
        return 1;
    for (i = 0; i < 300; i++) {
        if (run_waitable_cancel_iteration(i & 1) != 0) {
            fprintf(stderr,
                    "waitable-cancel: iteration %d failed errno=%d\n",
                    i, errno);
            return 1;
        }
    }
    Sleep(20);
    if (!GetProcessHandleCount(GetCurrentProcess(), &after))
        return 1;
    if (after > before + 16U) {
        fprintf(stderr,
                "waitable-cancel: handle growth before=%lu after=%lu\n",
                (unsigned long)before, (unsigned long)after);
        return 1;
    }
    puts("waitable-cancel: OK");
    return 0;
}

static int run_mode(const char *mode)
{
    if (strcmp(mode, "event") == 0)
        return test_event_handle();
    if (strcmp(mode, "event-et") == 0)
        return test_event_handle_et();
    if (strcmp(mode, "auto-reset") == 0)
        return test_auto_reset_event();
    if (strcmp(mode, "auto-reset-et") == 0)
        return test_auto_reset_event_et();
    if (strcmp(mode, "semaphore") == 0)
        return test_semaphore_handle();
    if (strcmp(mode, "semaphore-et") == 0)
        return test_semaphore_handle_et();
    if (strcmp(mode, "pending-mod") == 0)
        return test_waitable_pending_mod();
    if (strcmp(mode, "ready-mod") == 0)
        return test_waitable_ready_mod();
    if (strcmp(mode, "timer-ready-mod") == 0)
        return test_timer_ready_mod();
    if (strcmp(mode, "timer-et") == 0)
        return test_timer_et_rejected();
    if (strcmp(mode, "job") == 0)
        return test_job_rejected();
    if (strcmp(mode, "access") == 0)
        return test_waitable_access_rejected();
    if (strcmp(mode, "process-thread") == 0)
        return test_process_and_thread_handles();
    if (strcmp(mode, "thread-et") == 0)
        return test_thread_handle_et_terminal();
    if (strcmp(mode, "exclusive") == 0)
        return test_waitable_exclusive_rejected();
    if (strcmp(mode, "mutex") == 0)
        return test_mutex_rejected();
    if (strcmp(mode, "cleanup") == 0)
        return test_waitable_cleanup();
    if (strcmp(mode, "cancel") == 0)
        return test_waitable_cancel_lifecycle();
    fprintf(stderr, "unknown waitable mode: %s\n", mode);
    return 2;
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    int failures = 0;
    const char *modes[] = {
        "event", "event-et", "auto-reset", "auto-reset-et", "semaphore",
        "semaphore-et", "pending-mod", "ready-mod", "timer-ready-mod",
        "timer-et", "job", "access", "process-thread", "thread-et",
        "exclusive", "mutex", "cleanup", "cancel"
    };
    size_t i;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 2;

    if (argc > 1) {
        int rc = run_mode(argv[1]);
        WSACleanup();
        return rc;
    }

    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (run_mode(modes[i]) != 0)
            failures++;
    }
    WSACleanup();
    return failures == 0 ? 0 : 1;
}

#else
int main(void) { return 0; }
#endif
