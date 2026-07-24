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
    event.events = EPOLLIN | EPOLLET;
    event.data.u64 = 8;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)event_handle, &event) != 0)
        goto fail;

    if (!SetEvent(event_handle))
        goto fail;

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0)
        goto fail;

    n = wait_one(epfd, 30, &events);
    if (n != 0) {
        fprintf(stderr, "waitable-et: redelivery n=%d\n", n);
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

static int run_mode(const char *mode)
{
    if (strcmp(mode, "event") == 0)
        return test_event_handle();
    if (strcmp(mode, "event-et") == 0)
        return test_event_handle_et();
    if (strcmp(mode, "exclusive") == 0)
        return test_waitable_exclusive_rejected();
    fprintf(stderr, "unknown waitable mode: %s\n", mode);
    return 2;
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    int failures = 0;
    const char *modes[] = { "event", "event-et", "exclusive" };
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
