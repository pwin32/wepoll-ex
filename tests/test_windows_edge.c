#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#define EXCLUSIVE_SCALE_REGISTRATIONS 129
#define MULTI_WAITERS 4
#define SAME_EPFD_ET_WAITERS 2
#define EXCLUSIVE_MIXED_ORDINARY 2
#define MULTI_WAIT_TIMEOUT_MS 1000

static int make_loopback_pair(SOCKET *listener_out, SOCKET *client_out,
                              SOCKET *accepted_out)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct sockaddr_in addr;
    int addrlen = (int)sizeof(addr);
    u_long nonblocking = 1;

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET || client == INVALID_SOCKET)
        goto fail;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR ||
        getsockname(listener, (struct sockaddr *)&addr, &addrlen) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR ||
        ioctlsocket(client, FIONBIO, &nonblocking) == SOCKET_ERROR ||
        connect(client, (const struct sockaddr *)&addr, addrlen) == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK)
            goto fail;
    }
    accepted = accept(listener, NULL, NULL);
    if (accepted == INVALID_SOCKET)
        goto fail;
    nonblocking = 1;
    if (ioctlsocket(accepted, FIONBIO, &nonblocking) == SOCKET_ERROR)
        goto fail;

    *listener_out = listener;
    *client_out = client;
    *accepted_out = accepted;
    return 0;

fail:
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return -1;
}

static int wait_one(int epfd, int timeout_ms, uint32_t *events_out,
                    uint32_t *flags_out)
{
    struct epoll_event_ex events[1];
    int n = epoll_wait_ex(epfd, events, 1, timeout_ms);
    if (n < 0)
        return -1;
    if (n == 0)
        return 0;
    if (events_out)
        *events_out = events[0].events;
    if (flags_out)
        *flags_out = events[0].flags;
    return 1;
}

static int drain_nonblocking_socket(SOCKET socket_fd)
{
    char buffer[64];
    int total = 0;

    for (;;) {
        int received = recv(socket_fd, buffer, (int)sizeof(buffer), 0);

        if (received > 0) {
            total += received;
            continue;
        }
        if (received == 0) {
            return total;
        }
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            return total;
        }
        return -1;
    }
}

typedef struct multi_wait_context {
    int epfd;
    HANDLE started;
    struct epoll_event event;
    int result;
    int error;
} multi_wait_context_t;

static DWORD WINAPI multi_wait_thread(void *opaque)
{
    multi_wait_context_t *context = (multi_wait_context_t *)opaque;

    (void)SetEvent(context->started);
    errno = 0;
    context->result = epoll_wait(context->epfd, &context->event, 1,
                                 MULTI_WAIT_TIMEOUT_MS);
    context->error = errno;
    return 0;
}

static int test_edge_readable(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    char buffer[16];
    uint32_t events = 0;
    uint32_t flags = 0;
    int epfd = -1;
    int n;

    if (make_loopback_pair(&listener, &client, &accepted) != 0) {
        fputs("edge: pair setup failed\n", stderr);
        return 1;
    }

    epfd = epoll_create1(0);
    if (epfd < 0) {
        fputs("edge: epoll_create1 failed\n", stderr);
        goto fail;
    }

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET;
    event.data.u64 = 1;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0) {
        fprintf(stderr, "edge: ADD EPOLLET failed errno=%d\n", errno);
        goto fail;
    }

    if (send(client, "abc", 3, 0) != 3) {
        fputs("edge: send failed\n", stderr);
        goto fail;
    }

    n = wait_one(epfd, 1000, &events, &flags);
    if (n != 1 || (events & EPOLLIN) == 0 ||
        (flags & WEPOLL_FLAG_ET_DELIVERED) == 0) {
        fprintf(stderr, "edge: first wait n=%d events=%u flags=%u errno=%d\n",
                n, events, flags, errno);
        goto fail;
    }

    /* Still unread: edge-triggered must not redeliver the same level. */
    n = wait_one(epfd, 50, &events, &flags);
    if (n != 0) {
        fprintf(stderr, "edge: unexpected redelivery n=%d events=%u\n",
                n, events);
        goto fail;
    }

    if (recv(accepted, buffer, sizeof(buffer), 0) != 3) {
        fputs("edge: recv failed\n", stderr);
        goto fail;
    }

    /* Observe the drained (EAGAIN) condition before the next write. */
    n = wait_one(epfd, 0, &events, &flags);
    if (n != 0) {
        fprintf(stderr, "edge: post-drain wait n=%d events=%u\n", n, events);
        goto fail;
    }

    if (send(client, "de", 2, 0) != 2) {
        fputs("edge: second send failed\n", stderr);
        goto fail;
    }

    n = wait_one(epfd, 1000, &events, &flags);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr, "edge: second edge missing n=%d events=%u\n",
                n, events);
        goto fail;
    }

    (void)wepoll_close(epfd);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("edge-readable: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_same_epfd_et_single_wake(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    const uint64_t data = UINT64_C(0x455453494e474c45);
    struct epoll_event event;
    multi_wait_context_t contexts[SAME_EPFD_ET_WAITERS];
    HANDLE started[SAME_EPFD_ET_WAITERS];
    HANDLE threads[SAME_EPFD_ET_WAITERS];
    int epfd = -1;
    int registered = 0;
    int all_threads_done = 0;
    int result = 1;
    int winners = 0;
    char byte;
    int i;

    memset(contexts, 0, sizeof(contexts));
    memset(started, 0, sizeof(started));
    memset(threads, 0, sizeof(threads));
    if (make_loopback_pair(&listener, &client, &accepted) != 0) {
        fputs("same-epfd-et: pair setup failed\n", stderr);
        goto done;
    }
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        fprintf(stderr, "same-epfd-et: epoll_create1 failed errno=%d\n",
                errno);
        goto done;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET;
    event.data.u64 = data;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0) {
        fprintf(stderr, "same-epfd-et: ADD failed errno=%d\n", errno);
        goto done;
    }
    registered = 1;

    for (i = 0; i < SAME_EPFD_ET_WAITERS; i++) {
        started[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
        contexts[i].epfd = epfd;
        contexts[i].started = started[i];
        contexts[i].result = -2;
        if (started[i] == NULL) {
            fprintf(stderr,
                    "same-epfd-et: start event %d failed error=%lu\n",
                    i, (unsigned long)GetLastError());
            goto done;
        }
        threads[i] = CreateThread(NULL, 0, multi_wait_thread,
                                  &contexts[i], 0, NULL);
        if (threads[i] == NULL) {
            fprintf(stderr,
                    "same-epfd-et: waiter %d failed error=%lu\n",
                    i, (unsigned long)GetLastError());
            goto done;
        }
    }
    if (WaitForMultipleObjects(SAME_EPFD_ET_WAITERS, started, TRUE,
                               2000) != WAIT_OBJECT_0) {
        fputs("same-epfd-et: waiters did not start\n", stderr);
        goto done;
    }
    Sleep(50);
    if (send(client, "e", 1, 0) != 1) {
        fprintf(stderr, "same-epfd-et: send failed WSA=%d\n",
                WSAGetLastError());
        goto done;
    }
    if (WaitForMultipleObjects(SAME_EPFD_ET_WAITERS, threads, TRUE,
                               MULTI_WAIT_TIMEOUT_MS + 2000) !=
        WAIT_OBJECT_0) {
        fputs("same-epfd-et: bounded waiters hung\n", stderr);
        goto done;
    }
    all_threads_done = 1;
    for (i = 0; i < SAME_EPFD_ET_WAITERS; i++) {
        if (contexts[i].result == 1) {
            if (contexts[i].event.events != EPOLLIN ||
                contexts[i].event.data.u64 != data) {
                fprintf(stderr,
                        "same-epfd-et: waiter %d event mismatch "
                        "events=%u data=%llu errno=%d\n",
                        i, contexts[i].event.events,
                        (unsigned long long)contexts[i].event.data.u64,
                        contexts[i].error);
                goto done;
            }
            winners++;
        } else if (contexts[i].result != 0) {
            fprintf(stderr,
                    "same-epfd-et: waiter %d result=%d errno=%d\n",
                    i, contexts[i].result, contexts[i].error);
            goto done;
        }
    }
    if (winners != 1 || recv(accepted, &byte, 1, 0) != 1 || byte != 'e') {
        fprintf(stderr,
                "same-epfd-et: expected one winner, observed %d\n",
                winners);
        goto done;
    }
    result = 0;

done:
    if (registered && epfd >= 0 &&
        epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL) == 0) {
        registered = 0;
    }
    if (!all_threads_done && epfd >= 0) {
        (void)wepoll_close(epfd);
        epfd = -1;
    }
    for (i = 0; i < SAME_EPFD_ET_WAITERS; i++) {
        if (threads[i] != NULL &&
            WaitForSingleObject(threads[i], 5000) != WAIT_OBJECT_0) {
            (void)TerminateThread(threads[i], 1);
        }
        if (threads[i] != NULL) CloseHandle(threads[i]);
        if (started[i] != NULL) CloseHandle(started[i]);
    }
    if (registered && epfd >= 0 &&
        epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL) != 0) {
        result = 1;
    }
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    if (result == 0) puts("same-epfd-et: exactly one waiter woke");
    return result;
}

static int test_edge_writable_no_spin(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    uint32_t events = 0;
    int epfd = -1;
    int n;
    int i;

    if (make_loopback_pair(&listener, &client, &accepted) != 0) {
        fputs("edge-out: pair setup failed\n", stderr);
        return 1;
    }

    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLOUT | EPOLLET;
    event.data.u64 = 2;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0) {
        fprintf(stderr, "edge-out: ADD failed errno=%d\n", errno);
        goto fail;
    }

    n = wait_one(epfd, 1000, &events, NULL);
    if (n != 1 || (events & EPOLLOUT) == 0) {
        fprintf(stderr, "edge-out: first OUT missing n=%d events=%u\n",
                n, events);
        goto fail;
    }

    for (i = 0; i < 8; i++) {
        n = wait_one(epfd, 20, &events, NULL);
        if (n != 0) {
            fprintf(stderr, "edge-out: spin redelivery i=%d n=%d events=%u\n",
                    i, n, events);
            goto fail;
        }
    }

    (void)wepoll_close(epfd);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("edge-writable: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_explicit_rearm_directional(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    uint32_t events = 0;
    int epfd = -1;
    int registered = 0;
    int n;

    if (make_loopback_pair(&listener, &client, &accepted) != 0) {
        fputs("explicit-rearm: pair setup failed\n", stderr);
        return 1;
    }
    epfd = epoll_create_ex(0, WEPOLL_EX_CREATE_EXPLICIT_REARM);
    if (epfd < 0) {
        fprintf(stderr, "explicit-rearm: create failed errno=%d\n", errno);
        goto fail;
    }

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLOUT | EPOLLET;
    event.data.u64 = UINT64_C(0x4558504c49434954);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0) {
        fprintf(stderr, "explicit-rearm: ADD failed errno=%d\n", errno);
        goto fail;
    }
    registered = 1;

    n = wait_one(epfd, 1000, &events, NULL);
    if (n != 1 || (events & EPOLLOUT) == 0) {
        fprintf(stderr,
                "explicit-rearm: initial writable n=%d events=0x%08lx\n",
                n, (unsigned long)events);
        goto fail;
    }
    if (wait_one(epfd, 50, &events, NULL) != 0) {
        fputs("explicit-rearm: writable class redelivered\n", stderr);
        goto fail;
    }

    if (send(client, "abc", 3, 0) != 3) {
        fputs("explicit-rearm: first send failed\n", stderr);
        goto fail;
    }
    n = wait_one(epfd, 1000, &events, NULL);
    if (n != 1 || (events & EPOLLIN) == 0 ||
        (events & EPOLLOUT) != 0) {
        fprintf(stderr,
                "explicit-rearm: read class n=%d events=0x%08lx\n",
                n, (unsigned long)events);
        goto fail;
    }

    if (send(client, "d", 1, 0) != 1 ||
        wait_one(epfd, 50, &events, NULL) != 0) {
        fputs("explicit-rearm: unread level redelivered before ack\n", stderr);
        goto fail;
    }
    if (epoll_rearm_classes(epfd, accepted, WEPOLL_EX_REARM_READ) != 0) {
        fprintf(stderr, "explicit-rearm: read ack failed errno=%d\n", errno);
        goto fail;
    }
    n = wait_one(epfd, 1000, &events, NULL);
    if (n != 1 || (events & EPOLLIN) == 0 ||
        (events & EPOLLOUT) != 0) {
        fprintf(stderr,
                "explicit-rearm: incomplete drain n=%d events=0x%08lx\n",
                n, (unsigned long)events);
        goto fail;
    }
    if (drain_nonblocking_socket(accepted) != 4 ||
        epoll_rearm_classes(epfd, accepted,
                            WEPOLL_EX_REARM_READ) != 0 ||
        wait_one(epfd, 50, &events, NULL) != 0) {
        fputs("explicit-rearm: drained read did not stay pending\n", stderr);
        goto fail;
    }

    if (send(client, "e", 1, 0) != 1) {
        fputs("explicit-rearm: second send failed\n", stderr);
        goto fail;
    }
    n = wait_one(epfd, 1000, &events, NULL);
    if (n != 1 || (events & EPOLLIN) == 0 ||
        (events & EPOLLOUT) != 0 ||
        drain_nonblocking_socket(accepted) != 1) {
        fprintf(stderr,
                "explicit-rearm: second read n=%d events=0x%08lx\n",
                n, (unsigned long)events);
        goto fail;
    }

    if (epoll_rearm_classes(epfd, accepted, WEPOLL_EX_REARM_READ) != 0 ||
        epoll_rearm_classes(epfd, accepted, WEPOLL_EX_REARM_WRITE) != 0) {
        fprintf(stderr,
                "explicit-rearm: duplex ack failed errno=%d\n", errno);
        goto fail;
    }
    n = wait_one(epfd, 1000, &events, NULL);
    if (n != 1 || (events & EPOLLOUT) == 0 ||
        (events & EPOLLIN) != 0 ||
        wait_one(epfd, 50, &events, NULL) != 0) {
        fprintf(stderr,
                "explicit-rearm: writable ack n=%d events=0x%08lx\n",
                n, (unsigned long)events);
        goto fail;
    }

    event.data.u64++;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, accepted, &event) != 0) {
        fprintf(stderr, "explicit-rearm: MOD failed errno=%d\n", errno);
        goto fail;
    }
    n = wait_one(epfd, 1000, &events, NULL);
    if (n != 1 || (events & EPOLLOUT) == 0 ||
        wait_one(epfd, 50, &events, NULL) != 0) {
        fprintf(stderr,
                "explicit-rearm: MOD did not reset disarms n=%d events=0x%08lx\n",
                n, (unsigned long)events);
        goto fail;
    }
    if (epoll_rearm(epfd, accepted) != 0) {
        fprintf(stderr, "explicit-rearm: all-class ack failed errno=%d\n",
                errno);
        goto fail;
    }
    n = wait_one(epfd, 1000, &events, NULL);
    if (n != 1 || (events & EPOLLOUT) == 0 ||
        wait_one(epfd, 50, &events, NULL) != 0) {
        fprintf(stderr,
                "explicit-rearm: all-class redelivery n=%d events=0x%08lx\n",
                n, (unsigned long)events);
        goto fail;
    }

    if (epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL) != 0) {
        fprintf(stderr, "explicit-rearm: DEL failed errno=%d\n", errno);
        goto fail;
    }
    registered = 0;
    (void)wepoll_close(epfd);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("explicit-rearm: directional, incomplete-drain, MOD, and DEL OK");
    return 0;

fail:
    if (registered && epfd >= 0)
        (void)epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL);
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_explicit_rearm_terminal(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    struct linger reset = { 1, 0 };
    uint32_t events = 0;
    int epfd = -1;
    int registered = 0;
    int n;

    if (make_loopback_pair(&listener, &client, &accepted) != 0) {
        return 1;
    }
    epfd = epoll_create_ex(0, WEPOLL_EX_CREATE_EXPLICIT_REARM);
    if (epfd < 0) goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    event.data.u64 = UINT64_C(0x4558505445524d);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0) goto fail;
    registered = 1;
    if (wait_one(epfd, 1000, &events, NULL) != 1 ||
        (events & EPOLLOUT) == 0 ||
        setsockopt(client, SOL_SOCKET, SO_LINGER,
                   (const char *)&reset, (int)sizeof(reset)) == SOCKET_ERROR) {
        goto fail;
    }
    closesocket(client);
    client = INVALID_SOCKET;

    n = wait_one(epfd, 2000, &events, NULL);
    if (n != 1 ||
        (events & (EPOLLERR | EPOLLHUP)) != (EPOLLERR | EPOLLHUP)) {
        fprintf(stderr,
                "explicit-terminal: reset n=%d events=0x%08lx errno=%d\n",
                n, (unsigned long)events, errno);
        goto fail;
    }
    if (wait_one(epfd, 100, &events, NULL) != 0) {
        fputs("explicit-terminal: persistent reset redelivered\n", stderr);
        goto fail;
    }
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL) != 0) goto fail;
    registered = 0;

    (void)wepoll_close(epfd);
    closesocket(accepted);
    closesocket(listener);
    puts("explicit-terminal: terminal delivery idled until DEL");
    return 0;

fail:
    if (registered && epfd >= 0)
        (void)epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL);
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_explicit_rearm_fin(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    char byte;
    uint32_t events = 0;
    int epfd = -1;
    int registered = 0;
    int n;

    if (make_loopback_pair(&listener, &client, &accepted) != 0) {
        return 1;
    }
    epfd = epoll_create_ex(0, WEPOLL_EX_CREATE_EXPLICIT_REARM);
    if (epfd < 0) goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    event.data.u64 = UINT64_C(0x45585046494e0001);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0) goto fail;
    registered = 1;
    if (shutdown(client, SD_SEND) == SOCKET_ERROR) goto fail;

    n = wait_one(epfd, 2000, &events, NULL);
    if (n != 1 || (events & (EPOLLIN | EPOLLRDHUP)) !=
                      (EPOLLIN | EPOLLRDHUP) ||
        (events & (EPOLLERR | EPOLLHUP)) != 0 ||
        recv(accepted, &byte, 1, 0) != 0 ||
        wait_one(epfd, 100, &events, NULL) != 0) {
        fprintf(stderr,
                "explicit-fin: first EOF n=%d events=0x%08lx errno=%d\n",
                n, (unsigned long)events, errno);
        goto fail;
    }

    if (epoll_rearm_classes(epfd, accepted, WEPOLL_EX_REARM_READ) != 0) {
        fprintf(stderr, "explicit-fin: read ack failed errno=%d\n", errno);
        goto fail;
    }
    n = wait_one(epfd, 2000, &events, NULL);
    if (n != 1 || (events & (EPOLLIN | EPOLLRDHUP)) !=
                      (EPOLLIN | EPOLLRDHUP) ||
        (events & (EPOLLERR | EPOLLHUP)) != 0 ||
        wait_one(epfd, 100, &events, NULL) != 0) {
        fprintf(stderr,
                "explicit-fin: rearmed EOF n=%d events=0x%08lx errno=%d\n",
                n, (unsigned long)events, errno);
        goto fail;
    }

    if (epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL) != 0) goto fail;
    registered = 0;
    (void)wepoll_close(epfd);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("explicit-fin: graceful EOF follows read-class acknowledgement");
    return 0;

fail:
    if (registered && epfd >= 0)
        (void)epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL);
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_explicit_rearm_oneshot(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    uint32_t events = 0;
    uint32_t flags = 0;
    int epfd = -1;
    int registered = 0;
    int n;

    if (make_loopback_pair(&listener, &client, &accepted) != 0) {
        return 1;
    }
    epfd = epoll_create_ex(0, WEPOLL_EX_CREATE_EXPLICIT_REARM);
    if (epfd < 0) goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT;
    event.data.u64 = UINT64_C(0x4558504f4e455348);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0) goto fail;
    registered = 1;

    n = wait_one(epfd, 1000, &events, &flags);
    if (n != 1 || (events & EPOLLOUT) == 0 ||
        (flags & (WEPOLL_FLAG_ET_DELIVERED |
                  WEPOLL_FLAG_ONESHOT_FIRED)) !=
            (WEPOLL_FLAG_ET_DELIVERED | WEPOLL_FLAG_ONESHOT_FIRED)) {
        fprintf(stderr,
                "explicit-oneshot: initial n=%d events=0x%08lx flags=0x%08lx\n",
                n, (unsigned long)events, (unsigned long)flags);
        goto fail;
    }

    if (send(client, "a", 1, 0) != 1 ||
        epoll_rearm_classes(epfd, accepted,
                            WEPOLL_EX_REARM_READ) != 0 ||
        wait_one(epfd, 50, &events, &flags) != 0) {
        fputs("explicit-oneshot: unrelated acknowledgement rearmed it\n",
              stderr);
        goto fail;
    }
    if (epoll_rearm_classes(epfd, accepted,
                            WEPOLL_EX_REARM_WRITE) != 0) {
        fprintf(stderr,
                "explicit-oneshot: final write acknowledgement failed errno=%d\n",
                errno);
        goto fail;
    }
    n = wait_one(epfd, 1000, &events, &flags);
    if (n != 1 || (events & (EPOLLIN | EPOLLOUT)) !=
                      (EPOLLIN | EPOLLOUT) ||
        (flags & WEPOLL_FLAG_ONESHOT_FIRED) == 0 ||
        drain_nonblocking_socket(accepted) != 1) {
        fprintf(stderr,
                "explicit-oneshot: second n=%d events=0x%08lx flags=0x%08lx\n",
                n, (unsigned long)events, (unsigned long)flags);
        goto fail;
    }

    if (epoll_rearm_classes(epfd, accepted,
                            WEPOLL_EX_REARM_READ) != 0 ||
        wait_one(epfd, 50, &events, &flags) != 0 ||
        epoll_rearm_classes(epfd, accepted,
                            WEPOLL_EX_REARM_WRITE) != 0) {
        fputs("explicit-oneshot: partial delivered-class contract failed\n",
              stderr);
        goto fail;
    }
    n = wait_one(epfd, 1000, &events, &flags);
    if (n != 1 || (events & EPOLLOUT) == 0 ||
        (flags & WEPOLL_FLAG_ONESHOT_FIRED) == 0) {
        fprintf(stderr,
                "explicit-oneshot: partial-final n=%d events=0x%08lx flags=0x%08lx\n",
                n, (unsigned long)events, (unsigned long)flags);
        goto fail;
    }

    event.data.u64++;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, accepted, &event) != 0) {
        fprintf(stderr, "explicit-oneshot: MOD failed errno=%d\n", errno);
        goto fail;
    }
    n = wait_one(epfd, 1000, &events, &flags);
    if (n != 1 || (events & EPOLLOUT) == 0 ||
        (flags & WEPOLL_FLAG_ONESHOT_FIRED) == 0) {
        fprintf(stderr,
                "explicit-oneshot: MOD rearm n=%d events=0x%08lx flags=0x%08lx\n",
                n, (unsigned long)events, (unsigned long)flags);
        goto fail;
    }

    if (epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL) != 0) goto fail;
    registered = 0;
    (void)wepoll_close(epfd);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("explicit-oneshot: class acknowledgements and MOD rearm OK");
    return 0;

fail:
    if (registered && epfd >= 0)
        (void)epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL);
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

typedef enum local_shutdown_mode {
    LOCAL_SHUTDOWN_LT = 0,
    LOCAL_SHUTDOWN_ET,
    LOCAL_SHUTDOWN_ONESHOT,
    LOCAL_SHUTDOWN_EXPLICIT
} local_shutdown_mode_t;

static int test_local_shutdown(local_shutdown_mode_t mode)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    uint32_t events = 0;
    uint32_t flags = 0;
    uint32_t expected_read;
    uint32_t expected_full;
    int epfd = -1;
    int registered = 0;
    int n;
    char byte;

    if (make_loopback_pair(&listener, &client, &accepted) != 0) {
        return 1;
    }
    epfd = mode == LOCAL_SHUTDOWN_EXPLICIT
        ? epoll_create_ex(0, WEPOLL_EX_CREATE_EXPLICIT_REARM)
        : epoll_create1(0);
    if (epfd < 0) goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
    if (mode != LOCAL_SHUTDOWN_LT) event.events |= EPOLLET;
    if (mode == LOCAL_SHUTDOWN_ONESHOT) event.events |= EPOLLONESHOT;
    event.data.u64 = UINT64_C(0x53485554444f574e) + (uint64_t)mode;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0) goto fail;
    registered = 1;

    n = wait_one(epfd, 1000, &events, &flags);
    if (n != 1 || events != EPOLLOUT ||
        (mode != LOCAL_SHUTDOWN_LT &&
         (flags & WEPOLL_FLAG_ET_DELIVERED) == 0) ||
        (mode == LOCAL_SHUTDOWN_ONESHOT &&
         (flags & WEPOLL_FLAG_ONESHOT_FIRED) == 0)) {
        fprintf(stderr,
                "local-shutdown: initial mode=%d n=%d events=0x%08lx flags=0x%08lx\n",
                (int)mode, n, (unsigned long)events, (unsigned long)flags);
        goto fail;
    }

    if (wepoll_ex_shutdown_socket(epfd, accepted, SD_RECEIVE) != 0) {
        fprintf(stderr,
                "local-shutdown: SD_RECEIVE mode=%d errno=%d WSA=%d\n",
                (int)mode, errno, WSAGetLastError());
        goto fail;
    }
    if (mode == LOCAL_SHUTDOWN_ONESHOT) {
        if (wait_one(epfd, 50, &events, &flags) != 0 ||
            epoll_ctl(epfd, EPOLL_CTL_MOD, accepted, &event) != 0) {
            fputs("local-shutdown: fired oneshot was not kept disabled\n",
                  stderr);
            goto fail;
        }
    }

    expected_read = (mode == LOCAL_SHUTDOWN_LT ||
                     mode == LOCAL_SHUTDOWN_ONESHOT)
        ? EPOLLIN | EPOLLOUT | EPOLLRDHUP
        : EPOLLIN | EPOLLRDHUP;
    n = wait_one(epfd, 1000, &events, &flags);
    if (n != 1 || events != expected_read ||
        (mode != LOCAL_SHUTDOWN_LT &&
         (flags & WEPOLL_FLAG_ET_DELIVERED) == 0) ||
        (mode == LOCAL_SHUTDOWN_ONESHOT &&
         (flags & WEPOLL_FLAG_ONESHOT_FIRED) == 0)) {
        fprintf(stderr,
                "local-shutdown: read mode=%d n=%d events=0x%08lx flags=0x%08lx WSA=%d\n",
                (int)mode, n, (unsigned long)events, (unsigned long)flags,
                WSAGetLastError());
        goto fail;
    }
    if (recv(accepted, &byte, 1, 0) != SOCKET_ERROR ||
        WSAGetLastError() != WSAESHUTDOWN) {
        fputs("local-shutdown: Winsock recv boundary changed\n", stderr);
        goto fail;
    }

    if (mode == LOCAL_SHUTDOWN_LT) {
        n = wait_one(epfd, 1000, &events, &flags);
        if (n != 1 || events != expected_read) {
            fputs("local-shutdown: LT level did not persist\n", stderr);
            goto fail;
        }
    } else if (mode == LOCAL_SHUTDOWN_EXPLICIT) {
        if (wait_one(epfd, 50, &events, &flags) != 0 ||
            epoll_rearm_classes(epfd, accepted,
                                WEPOLL_EX_REARM_READ) != 0) {
            fputs("local-shutdown: explicit read acknowledgement failed\n",
                  stderr);
            goto fail;
        }
        n = wait_one(epfd, 1000, &events, &flags);
        if (n != 1 || events != (EPOLLIN | EPOLLRDHUP)) {
            fputs("local-shutdown: explicit local EOF did not redeliver\n",
                  stderr);
            goto fail;
        }
    } else if (wait_one(epfd, 50, &events, &flags) != 0) {
        fputs("local-shutdown: ET/oneshot local EOF redelivered\n", stderr);
        goto fail;
    }

    if (wepoll_ex_shutdown_socket(epfd, accepted, SD_SEND) != 0) {
        fprintf(stderr, "local-shutdown: SD_SEND mode=%d errno=%d WSA=%d\n",
                (int)mode, errno, WSAGetLastError());
        goto fail;
    }
    if (mode == LOCAL_SHUTDOWN_ONESHOT) {
        if (wait_one(epfd, 50, &events, &flags) != 0 ||
            epoll_ctl(epfd, EPOLL_CTL_MOD, accepted, &event) != 0) {
            fputs("local-shutdown: full shutdown escaped fired oneshot\n",
                  stderr);
            goto fail;
        }
    }

    expected_full = mode == LOCAL_SHUTDOWN_LT ||
                    mode == LOCAL_SHUTDOWN_ONESHOT
        ? EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLHUP
        : EPOLLHUP;
    n = wait_one(epfd, 1000, &events, &flags);
    if (n != 1 || events != expected_full) {
        fprintf(stderr,
                "local-shutdown: full mode=%d n=%d events=0x%08lx expected=0x%08lx flags=0x%08lx\n",
                (int)mode, n, (unsigned long)events,
                (unsigned long)expected_full, (unsigned long)flags);
        goto fail;
    }

    if (epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL) != 0) goto fail;
    registered = 0;
    (void)wepoll_close(epfd);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    printf("local-shutdown-%d: Linux-like local shutdown readiness OK\n",
           (int)mode);
    return 0;

fail:
    if (registered && epfd >= 0)
        (void)epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL);
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_local_shutdown_mod_wake(void)
{
    const uint64_t data = UINT64_C(0x534855544d4f4457);
    multi_wait_context_t context;
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    HANDLE thread = NULL;
    uint32_t events = 0;
    uint32_t flags = 0;
    int epfd = -1;
    int registered = 0;
    int result = 1;

    memset(&context, 0, sizeof(context));
    if (make_loopback_pair(&listener, &client, &accepted) != 0) goto cleanup;
    epfd = epoll_create1(0);
    context.epfd = epfd;
    context.started = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (epfd < 0 || context.started == NULL) goto cleanup;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP |
                   EPOLLET | EPOLLONESHOT;
    event.data.u64 = data;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0) goto cleanup;
    registered = 1;
    if (wait_one(epfd, 1000, &events, &flags) != 1 ||
        events != EPOLLOUT ||
        (flags & WEPOLL_FLAG_ONESHOT_FIRED) == 0 ||
        wepoll_ex_shutdown_socket(epfd, accepted, SD_RECEIVE) != 0 ||
        wait_one(epfd, 50, &events, &flags) != 0) {
        goto cleanup;
    }

    thread = CreateThread(NULL, 0, multi_wait_thread, &context, 0, NULL);
    if (thread == NULL ||
        WaitForSingleObject(context.started, 2000) != WAIT_OBJECT_0) {
        goto cleanup;
    }
    Sleep(100);
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, accepted, &event) != 0 ||
        WaitForSingleObject(thread, 2000) != WAIT_OBJECT_0 ||
        context.result != 1 || context.error != 0 ||
        context.event.events != (EPOLLIN | EPOLLOUT | EPOLLRDHUP) ||
        context.event.data.u64 != data) {
        goto cleanup;
    }
    CloseHandle(thread);
    thread = NULL;
    result = 0;

cleanup:
    if (thread != NULL &&
        WaitForSingleObject(thread, 0) != WAIT_OBJECT_0) {
        if (epfd >= 0) (void)wepoll_ex_wake(epfd);
        if (WaitForSingleObject(thread, 2000) != WAIT_OBJECT_0) {
            (void)TerminateThread(thread, 1);
            result = 1;
        }
    }
    if (thread != NULL) CloseHandle(thread);
    if (registered && epfd >= 0)
        (void)epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL);
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (context.started != NULL) CloseHandle(context.started);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    if (result == 0)
        puts("local-shutdown-mod-wake: blocked wait observed fresh MOD");
    return result;
}

static int test_local_shutdown_rearm_wake(void)
{
    const uint64_t data = UINT64_C(0x5348555452454152);
    multi_wait_context_t context;
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    HANDLE thread = NULL;
    uint32_t events = 0;
    uint32_t flags = 0;
    int epfd = -1;
    int registered = 0;
    int result = 1;

    memset(&context, 0, sizeof(context));
    if (make_loopback_pair(&listener, &client, &accepted) != 0) goto cleanup;
    epfd = epoll_create_ex(0, WEPOLL_EX_CREATE_EXPLICIT_REARM);
    context.epfd = epfd;
    context.started = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (epfd < 0 || context.started == NULL) goto cleanup;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    event.data.u64 = data;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0 ||
        wepoll_ex_shutdown_socket(epfd, accepted, SD_RECEIVE) != 0 ||
        wait_one(epfd, 1000, &events, &flags) != 1 ||
        events != (EPOLLIN | EPOLLRDHUP) ||
        (flags & WEPOLL_FLAG_ET_DELIVERED) == 0) {
        goto cleanup;
    }
    registered = 1;

    thread = CreateThread(NULL, 0, multi_wait_thread, &context, 0, NULL);
    if (thread == NULL ||
        WaitForSingleObject(context.started, 2000) != WAIT_OBJECT_0) {
        goto cleanup;
    }
    Sleep(100);
    if (epoll_rearm_classes(epfd, accepted,
                            WEPOLL_EX_REARM_READ) != 0 ||
        WaitForSingleObject(thread, 2000) != WAIT_OBJECT_0 ||
        context.result != 1 || context.error != 0 ||
        context.event.events != (EPOLLIN | EPOLLRDHUP) ||
        context.event.data.u64 != data) {
        goto cleanup;
    }
    CloseHandle(thread);
    thread = NULL;
    result = 0;

cleanup:
    if (thread != NULL &&
        WaitForSingleObject(thread, 0) != WAIT_OBJECT_0) {
        if (epfd >= 0) (void)wepoll_ex_wake(epfd);
        if (WaitForSingleObject(thread, 2000) != WAIT_OBJECT_0) {
            (void)TerminateThread(thread, 1);
            result = 1;
        }
    }
    if (thread != NULL) CloseHandle(thread);
    if (registered && epfd >= 0)
        (void)epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL);
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (context.started != NULL) CloseHandle(context.started);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    if (result == 0)
        puts("local-shutdown-rearm-wake: blocked wait observed rearm");
    return result;
}

static int test_explicit_rearm_contract(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    HANDLE event_handle = NULL;
    struct epoll_event event;
    int epfd = -1;
    int ordinary_epfd = -1;
    int registered = 0;

    errno = 0;
    if (epoll_create1(WEPOLL_EX_CREATE_EXPLICIT_REARM) != -1 ||
        errno != EINVAL ||
        make_loopback_pair(&listener, &client, &accepted) != 0) {
        goto fail;
    }
    epfd = epoll_create_ex(0, WEPOLL_EX_CREATE_EXPLICIT_REARM);
    ordinary_epfd = epoll_create1(0);
    event_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (epfd < 0 || ordinary_epfd < 0 || event_handle == NULL) goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET | EPOLLEXCLUSIVE;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != -1 ||
        errno != EINVAL) {
        goto fail;
    }
    event.events = EPOLLIN | EPOLLET;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)event_handle,
                  &event) != -1 || errno != EOPNOTSUPP) {
        goto fail;
    }

    event.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0) goto fail;
    registered = 1;
    errno = 0;
    if (epoll_rearm_classes(epfd, accepted, WEPOLL_EX_REARM_READ) != -1 ||
        errno != EOPNOTSUPP ||
        epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL) != 0) {
        goto fail;
    }
    registered = 0;

    event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(ordinary_epfd, EPOLL_CTL_ADD, accepted, &event) != 0) {
        goto fail;
    }
    registered = 2;
    errno = 0;
    if (epoll_rearm_classes(ordinary_epfd, accepted,
                            WEPOLL_EX_REARM_READ) != -1 ||
        errno != EOPNOTSUPP) {
        goto fail;
    }
    errno = 0;
    if (epoll_rearm_classes(ordinary_epfd, accepted, 0) != -1 ||
        errno != EINVAL) {
        goto fail;
    }
    if (epoll_ctl(ordinary_epfd, EPOLL_CTL_DEL, accepted, NULL) != 0) {
        goto fail;
    }
    registered = 0;

    (void)wepoll_close(ordinary_epfd);
    (void)wepoll_close(epfd);
    CloseHandle(event_handle);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("explicit-contract: create and registration boundaries OK");
    return 0;

fail:
    if (registered == 1 && epfd >= 0)
        (void)epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL);
    if (registered == 2 && ordinary_epfd >= 0)
        (void)epoll_ctl(ordinary_epfd, EPOLL_CTL_DEL, accepted, NULL);
    if (ordinary_epfd >= 0) (void)wepoll_close(ordinary_epfd);
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (event_handle != NULL) CloseHandle(event_handle);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_exclusive_mod_rejected(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    int epfd = -1;

    if (make_loopback_pair(&listener, &client, &accepted) != 0)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLEXCLUSIVE;
    event.data.u64 = 3;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0)
        goto fail;

    /* Linux rejects every MOD of an exclusive registration, even when the
     * MOD mask does not repeat EPOLLEXCLUSIVE. */
    event.events = EPOLLIN;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, accepted, &event) != -1 ||
        errno != EINVAL) {
        fprintf(stderr, "exclusive-mod: expected EINVAL, got errno=%d\n",
                errno);
        goto fail;
    }

    (void)wepoll_close(epfd);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("exclusive-mod: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int wait_for_exclusive_recipient(int epfd_a, int epfd_b,
                                        int *winner_out, const char *label)
{
    struct epoll_event events[1];
    int winner = 0;
    int loser_checks = 0;
    int i;

    for (i = 0; i < 40 && loser_checks < 6; i++) {
        int n;

        if (winner != 1) {
            n = epoll_wait(epfd_a, events, 1, 25);
            if (n < 0)
                return -1;
            if (n == 1 && (events[0].events & EPOLLIN) != 0) {
                if (winner == 2) {
                    fprintf(stderr, "%s: both instances woke\n", label);
                    return -1;
                }
                winner = 1;
            }
        }
        if (winner != 2) {
            n = epoll_wait(epfd_b, events, 1, 25);
            if (n < 0)
                return -1;
            if (n == 1 && (events[0].events & EPOLLIN) != 0) {
                if (winner == 1) {
                    fprintf(stderr, "%s: both instances woke\n", label);
                    return -1;
                }
                winner = 2;
            }
        }
        if (winner != 0)
            loser_checks++;
    }
    if (winner == 0) {
        fprintf(stderr, "%s: neither instance woke\n", label);
        return -1;
    }
    *winner_out = winner;
    return 0;
}

static int arm_exclusive_after_drain(int epfd, const char *label)
{
    struct epoll_event events[1];
    int n = epoll_wait(epfd, events, 1, 0);

    /* A queued peer completion from the prior level must be re-sampled and
     * discarded after the application has synchronously drained the socket. */
    if (n < 0) {
        fprintf(stderr, "%s: rearm failed errno=%d\n", label, errno);
        return -1;
    }
    if (n != 0) {
        fprintf(stderr, "%s: stale post-drain events=%u\n",
                label, events[0].events);
        return -1;
    }
    return 0;
}

static int test_exclusive_two_cycles(int edge_triggered)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    int epfd_a = -1;
    int epfd_b = -1;
    int first_winner = 0;
    int second_winner = 0;
    char byte;
    uint32_t flags = EPOLLIN | EPOLLEXCLUSIVE;

    if (edge_triggered)
        flags |= EPOLLET;

    if (make_loopback_pair(&listener, &client, &accepted) != 0)
        return 1;

    epfd_a = epoll_create1(0);
    epfd_b = epoll_create1(0);
    if (epfd_a < 0 || epfd_b < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = flags;
    event.data.u64 = 11;
    if (epoll_ctl(epfd_a, EPOLL_CTL_ADD, accepted, &event) != 0) {
        fprintf(stderr, "exclusive: ADD A failed errno=%d\n", errno);
        goto fail;
    }

    event.events = flags;
    event.data.u64 = 22;
    if (epoll_ctl(epfd_b, EPOLL_CTL_ADD, accepted, &event) != 0) {
        fprintf(stderr, "exclusive: ADD B failed errno=%d\n", errno);
        goto fail;
    }

    if (send(client, "x", 1, 0) != 1)
        goto fail;

    if (wait_for_exclusive_recipient(epfd_a, epfd_b, &first_winner,
                                     edge_triggered ? "exclusive-et first" :
                                                      "exclusive first") != 0) {
        goto fail;
    }
    if (recv(accepted, &byte, 1, 0) != 1) {
        fprintf(stderr, "exclusive: first recv failed WSA=%d\n",
                WSAGetLastError());
        goto fail;
    }

    /* A pending re-submit after the application drains the byte is the
     * quiescent transition that releases the process-wide exclusive claim. */
    if (arm_exclusive_after_drain(epfd_a, "exclusive rearm A") != 0 ||
        arm_exclusive_after_drain(epfd_b, "exclusive rearm B") != 0) {
        goto fail;
    }

    if (send(client, "y", 1, 0) != 1)
        goto fail;
    if (wait_for_exclusive_recipient(epfd_a, epfd_b, &second_winner,
                                     edge_triggered ? "exclusive-et second" :
                                                      "exclusive second") != 0) {
        goto fail;
    }
    if (recv(accepted, &byte, 1, 0) != 1) {
        fprintf(stderr, "exclusive: second recv failed WSA=%d\n",
                WSAGetLastError());
        goto fail;
    }

    (void)wepoll_close(epfd_a);
    (void)wepoll_close(epfd_b);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    printf("%s: first=%c second=%c OK\n",
           edge_triggered ? "exclusive-et-wake" : "exclusive-wake",
           first_winner == 1 ? 'A' : 'B',
           second_winner == 1 ? 'A' : 'B');
    return 0;

fail:
    if (epfd_a >= 0) (void)wepoll_close(epfd_a);
    if (epfd_b >= 0) (void)wepoll_close(epfd_b);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static void multi_wait_close_ports(int epfds[MULTI_WAITERS])
{
    int i;

    for (i = 0; i < MULTI_WAITERS; i++) {
        if (epfds[i] >= 0) {
            (void)wepoll_close(epfds[i]);
            epfds[i] = -1;
        }
    }
}

static int multi_wait_cycle(
    int epfds[MULTI_WAITERS], const int *waiter_order, SOCKET client,
    SOCKET accepted, const char *suite_label, const char *order_label,
    const char *trigger_label, int cycle, int ordinary_count,
    uint64_t token_base)
{
    multi_wait_context_t contexts[MULTI_WAITERS];
    HANDLE started[MULTI_WAITERS];
    HANDLE threads[MULTI_WAITERS];
    int created_threads = 0;
    int all_threads_done = 0;
    int exclusive_wakes = 0;
    int result = 1;
    int i;
    char byte;

    memset(contexts, 0, sizeof(contexts));
    memset(started, 0, sizeof(started));
    memset(threads, 0, sizeof(threads));
    for (i = 0; i < MULTI_WAITERS; i++) {
        started[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
        contexts[i].epfd = epfds[i];
        contexts[i].started = started[i];
        contexts[i].result = -2;
        if (started[i] == NULL) {
            fprintf(stderr,
                    "%s %s %s cycle %d: start event %d "
                    "failed error=%lu\n",
                    suite_label, order_label, trigger_label, cycle + 1, i,
                    (unsigned long)GetLastError());
            goto done;
        }
    }
    for (i = 0; i < MULTI_WAITERS; i++) {
        int index = waiter_order[i];

        threads[index] = CreateThread(NULL, 0, multi_wait_thread,
                                      &contexts[index], 0, NULL);
        if (threads[index] == NULL) {
            fprintf(stderr,
                    "%s %s %s cycle %d: waiter %d failed "
                    "error=%lu\n",
                    suite_label, order_label, trigger_label, cycle + 1,
                    index,
                    (unsigned long)GetLastError());
            goto done;
        }
        created_threads++;
    }
    if (WaitForMultipleObjects(MULTI_WAITERS, started, TRUE,
                               2000) != WAIT_OBJECT_0) {
        fprintf(stderr,
                "%s %s %s cycle %d: waiters did not start\n",
                suite_label, order_label, trigger_label, cycle + 1);
        goto done;
    }

    /* Keep the socket unread until every waiter returns.  This makes a late
     * scheduled ordinary waiter observe the same persistent level instead of
     * turning scheduler timing into a missed-wakeup result. */
    Sleep(50);
    if (send(client, "x", 1, 0) != 1) {
        fprintf(stderr,
                "%s %s %s cycle %d: send failed WSA=%d\n",
                suite_label, order_label, trigger_label, cycle + 1,
                WSAGetLastError());
        goto done;
    }
    if (WaitForMultipleObjects(MULTI_WAITERS, threads, TRUE,
                               MULTI_WAIT_TIMEOUT_MS + 2000) !=
        WAIT_OBJECT_0) {
        fprintf(stderr,
                "%s %s %s cycle %d: bounded waiters hung\n",
                suite_label, order_label, trigger_label, cycle + 1);
        goto done;
    }
    all_threads_done = 1;

    for (i = 0; i < MULTI_WAITERS; i++) {
        uint64_t expected = token_base + (uint64_t)i;

        if (i < ordinary_count) {
            if (contexts[i].result != 1 ||
                (contexts[i].event.events & EPOLLIN) == 0 ||
                contexts[i].event.data.u64 != expected) {
                fprintf(stderr,
                        "%s %s %s cycle %d: ordinary %d "
                        "result=%d events=%u data=%llu errno=%d\n",
                        suite_label, order_label, trigger_label,
                        cycle + 1, i,
                        contexts[i].result, contexts[i].event.events,
                        (unsigned long long)contexts[i].event.data.u64,
                        contexts[i].error);
                goto done;
            }
        } else if (contexts[i].result == 1) {
            if ((contexts[i].event.events & EPOLLIN) == 0 ||
                contexts[i].event.data.u64 != expected) {
                fprintf(stderr,
                        "%s %s %s cycle %d: exclusive %d "
                        "events=%u data=%llu errno=%d\n",
                        suite_label, order_label, trigger_label,
                        cycle + 1, i - ordinary_count,
                        contexts[i].event.events,
                        (unsigned long long)contexts[i].event.data.u64,
                        contexts[i].error);
                goto done;
            }
            exclusive_wakes++;
        } else if (contexts[i].result != 0) {
            fprintf(stderr,
                    "%s %s %s cycle %d: exclusive %d "
                    "result=%d errno=%d\n",
                    suite_label, order_label, trigger_label, cycle + 1,
                    i - ordinary_count,
                    contexts[i].result, contexts[i].error);
            goto done;
        }
    }
    if (ordinary_count < MULTI_WAITERS && exclusive_wakes == 0) {
        fprintf(stderr,
                "%s %s %s cycle %d: no exclusive "
                "registration woke\n",
                suite_label, order_label, trigger_label, cycle + 1);
        goto done;
    }
    if (recv(accepted, &byte, 1, 0) != 1) {
        fprintf(stderr,
                "%s %s %s cycle %d: recv failed WSA=%d\n",
                suite_label, order_label, trigger_label, cycle + 1,
                WSAGetLastError());
        goto done;
    }
    result = 0;

done:
    if (!all_threads_done && created_threads != 0) {
        /* Closing the ports is the bounded public wakeup path.  Termination
         * below is only a last resort if a broken wait ignores that close. */
        multi_wait_close_ports(epfds);
    }
    for (i = 0; i < MULTI_WAITERS; i++) {
        if (threads[i] != NULL &&
            WaitForSingleObject(threads[i], 5000) != WAIT_OBJECT_0) {
            (void)TerminateThread(threads[i], 1);
        }
    }
    for (i = 0; i < MULTI_WAITERS; i++) {
        if (threads[i] != NULL) {
            CloseHandle(threads[i]);
        }
        if (started[i] != NULL) {
            CloseHandle(started[i]);
        }
    }
    return result;
}

static int test_exclusive_mixed_order(int exclusive_first,
                                      int edge_triggered)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    struct epoll_event output;
    int epfds[MULTI_WAITERS];
    int registration_order[MULTI_WAITERS];
    const char *order_label = exclusive_first ? "exclusive-first" :
                                                "ordinary-first";
    const char *trigger_label = edge_triggered ? "ET" : "LT";
    int result = 1;
    int cycle;
    int i;

    for (i = 0; i < MULTI_WAITERS; i++) {
        epfds[i] = -1;
        registration_order[i] = exclusive_first
            ? (i + EXCLUSIVE_MIXED_ORDINARY) % MULTI_WAITERS
            : i;
    }

    if (make_loopback_pair(&listener, &client, &accepted) != 0) {
        fprintf(stderr,
                "exclusive-mixed %s %s: pair setup failed WSA=%d\n",
                order_label, trigger_label, WSAGetLastError());
        goto done;
    }
    for (i = 0; i < MULTI_WAITERS; i++) {
        epfds[i] = epoll_create1(0);
        if (epfds[i] < 0) {
            fprintf(stderr,
                    "exclusive-mixed %s %s: epoll_create1 %d failed "
                    "errno=%d\n",
                    order_label, trigger_label, i, errno);
            goto done;
        }
    }

    memset(&event, 0, sizeof(event));
    for (i = 0; i < MULTI_WAITERS; i++) {
        int index = registration_order[i];

        event.events = EPOLLIN | (edge_triggered ? EPOLLET : 0);
        if (index >= EXCLUSIVE_MIXED_ORDINARY) {
            event.events |= EPOLLEXCLUSIVE;
        }
        event.data.u64 = UINT64_C(100) + (uint64_t)index;
        if (epoll_ctl(epfds[index], EPOLL_CTL_ADD, accepted, &event) != 0) {
            fprintf(stderr,
                    "exclusive-mixed %s %s: ADD %d failed errno=%d\n",
                    order_label, trigger_label, index, errno);
            goto done;
        }
    }

    /* ADD synchronously submits each AFD request, so the registration loop
     * above also defines the ordinary-first/exclusive-first native order. */

    for (cycle = 0; cycle < 2; cycle++) {
        if (cycle != 0) {
            /* The prior byte is drained.  Every registration must observe
             * quiescence and reopen its LT/ET readiness path before cycle 2. */
            for (i = 0; i < MULTI_WAITERS; i++) {
                int index = registration_order[i];
                int n = epoll_wait(epfds[index], &output, 1, 0);

                if (n != 0) {
                    fprintf(stderr,
                            "exclusive-mixed %s %s: rearm %d returned %d "
                            "events=%u errno=%d\n",
                            order_label, trigger_label, index, n,
                            n > 0 ? output.events : 0U, errno);
                    goto done;
                }
            }
        }
        if (multi_wait_cycle(
                epfds, registration_order, client, accepted,
                "exclusive-mixed", order_label, trigger_label, cycle,
                EXCLUSIVE_MIXED_ORDINARY, UINT64_C(100)) != 0) {
            goto done;
        }
        for (i = 0; i < MULTI_WAITERS; i++) {
            if (epfds[i] < 0) {
                fprintf(stderr,
                        "exclusive-mixed %s %s: cycle %d closed port %d\n",
                        order_label, trigger_label, cycle + 1, i);
                goto done;
            }
        }
    }
    result = 0;

done:
    multi_wait_close_ports(epfds);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return result;
}

static int test_exclusive_mixed_waiters(void)
{
    int edge_triggered;
    int exclusive_first;

    for (edge_triggered = 0; edge_triggered <= 1; edge_triggered++) {
        for (exclusive_first = 0; exclusive_first <= 1; exclusive_first++) {
            if (test_exclusive_mixed_order(exclusive_first,
                                           edge_triggered) != 0) {
                return 1;
            }
        }
    }
    puts("exclusive-mixed: LT/ET ordinary and exclusive wake cycles OK");
    return 0;
}

static int test_ordinary_multi_variant(int edge_triggered,
                                       int readiness_before_wait)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    struct epoll_event output;
    int epfds[MULTI_WAITERS];
    int waiter_order[MULTI_WAITERS];
    const char *suite_label = readiness_before_wait
        ? "ordinary-prearmed"
        : "ordinary-multi";
    const char *trigger_label = edge_triggered ? "ET" : "LT";
    uint64_t token_base = readiness_before_wait
        ? UINT64_C(300)
        : UINT64_C(200);
    int result = 1;
    int cycle;
    int i;

    for (i = 0; i < MULTI_WAITERS; i++) {
        epfds[i] = -1;
        waiter_order[i] = i;
    }
    if (make_loopback_pair(&listener, &client, &accepted) != 0) {
        fprintf(stderr, "%s %s: pair setup failed WSA=%d\n",
                suite_label, trigger_label, WSAGetLastError());
        goto done;
    }
    for (i = 0; i < MULTI_WAITERS; i++) {
        epfds[i] = epoll_create1(0);
        if (epfds[i] < 0) {
            fprintf(stderr,
                    "%s %s: epoll_create1 %d failed errno=%d\n",
                    suite_label, trigger_label, i, errno);
            goto done;
        }
        memset(&event, 0, sizeof(event));
        event.events = EPOLLIN | (edge_triggered ? EPOLLET : 0);
        event.data.u64 = token_base + (uint64_t)i;
        if (epoll_ctl(epfds[i], EPOLL_CTL_ADD, accepted, &event) != 0) {
            fprintf(stderr, "%s %s: ADD %d failed errno=%d\n",
                    suite_label, trigger_label, i, errno);
            goto done;
        }
    }

    /* The first cycle deliberately relies only on requests armed by the four
     * idle ADD calls above. */
    for (cycle = 0; cycle < 2; cycle++) {
        if (cycle != 0) {
            /* After the prior byte is drained, every registration must
             * observe quiescence and rearm before the second cycle. */
            for (i = 0; i < MULTI_WAITERS; i++) {
                int n = epoll_wait(epfds[i], &output, 1, 0);

                if (n != 0) {
                    fprintf(stderr,
                            "%s %s cycle %d: rearm %d returned %d "
                            "events=%u errno=%d\n",
                            suite_label, trigger_label, cycle + 1, i, n,
                            n > 0 ? output.events : 0U, errno);
                    goto done;
                }
            }
        }

        if (!readiness_before_wait) {
            if (multi_wait_cycle(
                    epfds, waiter_order, client, accepted, suite_label,
                    "concurrent", trigger_label, cycle, MULTI_WAITERS,
                    token_base) != 0) {
                goto done;
            }
        } else {
            char byte;

            if (send(client, "p", 1, 0) != 1) {
                fprintf(stderr,
                        "%s %s cycle %d: send failed WSA=%d\n",
                        suite_label, trigger_label, cycle + 1,
                        WSAGetLastError());
                goto done;
            }
            /* Readiness exists before any sequential wait.  Waiting in the
             * reverse of arm order makes a lone first-arm winner visible. */
            Sleep(50);
            for (i = 0; i < MULTI_WAITERS; i++) {
                int index = MULTI_WAITERS - i - 1;
                uint64_t expected = token_base + (uint64_t)index;
                int n;

                memset(&output, 0, sizeof(output));
                errno = 0;
                n = epoll_wait(epfds[index], &output, 1,
                               MULTI_WAIT_TIMEOUT_MS);
                if (n != 1 || (output.events & EPOLLIN) == 0 ||
                    output.data.u64 != expected) {
                    fprintf(stderr,
                            "%s %s cycle %d: wait %d returned %d "
                            "events=%u data=%llu errno=%d\n",
                            suite_label, trigger_label, cycle + 1,
                            index, n, n > 0 ? output.events : 0U,
                            (unsigned long long)(n > 0
                                ? output.data.u64
                                : UINT64_C(0)),
                            errno);
                    goto done;
                }
            }
            if (recv(accepted, &byte, 1, 0) != 1) {
                fprintf(stderr,
                        "%s %s cycle %d: recv failed WSA=%d\n",
                        suite_label, trigger_label, cycle + 1,
                        WSAGetLastError());
                goto done;
            }
        }
        for (i = 0; i < MULTI_WAITERS; i++) {
            if (epfds[i] < 0) {
                fprintf(stderr,
                        "%s %s cycle %d: port %d closed unexpectedly\n",
                        suite_label, trigger_label, cycle + 1, i);
                goto done;
            }
        }
    }
    result = 0;

done:
    multi_wait_close_ports(epfds);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return result;
}

static int test_ordinary_multi_waiters(void)
{
    int edge_triggered;

    for (edge_triggered = 0; edge_triggered <= 1; edge_triggered++) {
        if (test_ordinary_multi_variant(edge_triggered, 0) != 0)
            return 1;
    }
    puts("ordinary-multi: four LT/ET ports woke across two cycles");
    return 0;
}

static int test_ordinary_prearmed_waiters(void)
{
    int edge_triggered;

    for (edge_triggered = 0; edge_triggered <= 1; edge_triggered++) {
        if (test_ordinary_multi_variant(edge_triggered, 1) != 0)
            return 1;
    }
    puts("ordinary-prearmed: all idle LT/ET ports retained readiness");
    return 0;
}

static int test_exclusive_disjoint_classes(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    struct epoll_event output;
    int epfd_read = -1;
    int epfd_read_second = -1;
    int epfd_write = -1;
    char byte;

    if (make_loopback_pair(&listener, &client, &accepted) != 0)
        return 1;

    epfd_read = epoll_create1(0);
    epfd_write = epoll_create1(0);
    if (epfd_read < 0 || epfd_write < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLOUT | EPOLLEXCLUSIVE;
    event.data.u64 = 32;
    if (epoll_ctl(epfd_write, EPOLL_CTL_ADD, accepted, &event) != 0 ||
        epoll_wait(epfd_write, &output, 1, 1000) != 1 ||
        (output.events & EPOLLOUT) == 0 || output.data.u64 != 32) {
        fprintf(stderr,
                "exclusive-disjoint: initial OUT missing events=%u errno=%d\n",
                output.events, errno);
        goto fail;
    }

    event.events = EPOLLIN | EPOLLOUT | EPOLLEXCLUSIVE;
    event.data.u64 = 31;
    if (epoll_ctl(epfd_read, EPOLL_CTL_ADD, accepted, &event) != 0 ||
        epoll_wait(epfd_read, &output, 1, 0) != 0) {
        fprintf(stderr, "exclusive-disjoint: mixed arm failed errno=%d\n",
                errno);
        goto fail;
    }

    /* The writable registration now owns a continuously active OUT class.
     * The mixed registration must filter that class without suppressing its
     * independent IN class on the same base socket. */
    if (send(client, "z", 1, 0) != 1 ||
        epoll_wait(epfd_read, &output, 1, 1000) != 1 ||
        (output.events & EPOLLIN) == 0 || (output.events & EPOLLOUT) != 0 ||
        output.data.u64 != 31 ||
        recv(accepted, &byte, 1, 0) != 1) {
        fprintf(stderr,
                "exclusive-disjoint: IN blocked by OUT owner events=%u "
                "errno=%d WSA=%d\n",
                output.events, errno, WSAGetLastError());
        goto fail;
    }

    /* OUT remains true, so the mixed registration's next AFD request cannot
     * become wholly pending.  Its read-side select sample must still release
     * the now-inactive READ claim, allowing another IN-only owner to win. */
    if (epoll_wait(epfd_read, &output, 1, 20) != 0) {
        fprintf(stderr,
                "exclusive-disjoint: mixed post-drain wait returned events=%u\n",
                output.events);
        goto fail;
    }
    epfd_read_second = epoll_create1(0);
    event.events = EPOLLIN | EPOLLEXCLUSIVE;
    event.data.u64 = 33;
    if (epfd_read_second < 0 ||
        epoll_ctl(epfd_read_second, EPOLL_CTL_ADD, accepted, &event) != 0 ||
        epoll_wait(epfd_read_second, &output, 1, 0) != 0 ||
        send(client, "q", 1, 0) != 1 ||
        epoll_wait(epfd_read_second, &output, 1, 1000) != 1 ||
        (output.events & EPOLLIN) == 0 || output.data.u64 != 33 ||
        recv(accepted, &byte, 1, 0) != 1) {
        fprintf(stderr,
                "exclusive-disjoint: inactive READ claim was retained "
                "events=%u errno=%d WSA=%d\n",
                output.events, errno, WSAGetLastError());
        goto fail;
    }

    (void)wepoll_close(epfd_read);
    (void)wepoll_close(epfd_read_second);
    (void)wepoll_close(epfd_write);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("exclusive-disjoint: OK");
    return 0;

fail:
    if (epfd_read >= 0) (void)wepoll_close(epfd_read);
    if (epfd_read_second >= 0) (void)wepoll_close(epfd_read_second);
    if (epfd_write >= 0) (void)wepoll_close(epfd_write);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_exclusive_scale(void)
{
    SOCKET *listeners = NULL;
    SOCKET *clients = NULL;
    SOCKET *accepted = NULL;
    struct epoll_event *events = NULL;
    unsigned char *seen = NULL;
    struct epoll_event event;
    int epfd_owner = -1;
    int epfd_peer = -1;
    ULONGLONG bulk_deadline = 0;
    size_t pair_count = 0;
    size_t released_index = EXCLUSIVE_SCALE_REGISTRATIONS / 2;
    size_t seen_count = 0;
    size_t i;
    int result = 1;

    listeners = (SOCKET *)malloc(
        EXCLUSIVE_SCALE_REGISTRATIONS * sizeof(*listeners));
    clients = (SOCKET *)malloc(
        EXCLUSIVE_SCALE_REGISTRATIONS * sizeof(*clients));
    accepted = (SOCKET *)malloc(
        EXCLUSIVE_SCALE_REGISTRATIONS * sizeof(*accepted));
    events = (struct epoll_event *)calloc(
        EXCLUSIVE_SCALE_REGISTRATIONS, sizeof(*events));
    seen = (unsigned char *)calloc(EXCLUSIVE_SCALE_REGISTRATIONS,
                                   sizeof(*seen));
    if (listeners == NULL || clients == NULL || accepted == NULL ||
        events == NULL || seen == NULL) {
        fputs("exclusive-scale: allocation failed\n", stderr);
        goto done;
    }
    for (i = 0; i < EXCLUSIVE_SCALE_REGISTRATIONS; i++) {
        listeners[i] = INVALID_SOCKET;
        clients[i] = INVALID_SOCKET;
        accepted[i] = INVALID_SOCKET;
    }

    epfd_owner = epoll_create1(0);
    if (epfd_owner < 0) {
        fprintf(stderr, "exclusive-scale: owner create failed errno=%d\n",
                errno);
        goto done;
    }

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET | EPOLLEXCLUSIVE;
    for (i = 0; i < EXCLUSIVE_SCALE_REGISTRATIONS; i++) {
        if (make_loopback_pair(&listeners[i], &clients[i], &accepted[i]) !=
            0) {
            fprintf(stderr, "exclusive-scale: pair %u setup failed WSA=%d\n",
                    (unsigned)i, WSAGetLastError());
            goto done;
        }
        pair_count = i + 1;
        event.data.u64 = (uint64_t)i + 1;
        if (epoll_ctl(epfd_owner, EPOLL_CTL_ADD, accepted[i], &event) != 0) {
            fprintf(stderr, "exclusive-scale: owner ADD %u failed errno=%d\n",
                    (unsigned)i, errno);
            goto done;
        }
        if (send(clients[i], "x", 1, 0) != 1) {
            fprintf(stderr, "exclusive-scale: send %u failed WSA=%d\n",
                    (unsigned)i, WSAGetLastError());
            goto done;
        }
    }

    while (seen_count < EXCLUSIVE_SCALE_REGISTRATIONS) {
        int n = epoll_wait(epfd_owner, events,
                           EXCLUSIVE_SCALE_REGISTRATIONS, 2000);
        int j;

        if (n <= 0) {
            fprintf(stderr,
                    "exclusive-scale: owner drain stopped at %u n=%d "
                    "errno=%d\n",
                    (unsigned)seen_count, n, errno);
            goto done;
        }
        for (j = 0; j < n; j++) {
            uint64_t token = events[j].data.u64;
            size_t index;

            if (token == 0 || token > EXCLUSIVE_SCALE_REGISTRATIONS ||
                (events[j].events & EPOLLIN) == 0) {
                fprintf(stderr,
                        "exclusive-scale: invalid owner event token=%llu "
                        "events=%u\n",
                        (unsigned long long)token, events[j].events);
                goto done;
            }
            index = (size_t)(token - 1);
            if (seen[index]) {
                fprintf(stderr,
                        "exclusive-scale: duplicate owner edge token=%llu\n",
                        (unsigned long long)token);
                goto done;
            }
            seen[index] = 1;
            seen_count++;
        }
    }

    /* Every unread socket now has an independently held read claim.  The old
     * fixed 128-slot table failed open for at least one of these 129 bases,
     * allowing a second port to report the same exclusive readiness. */
    epfd_peer = epoll_create1(0);
    if (epfd_peer < 0) {
        fprintf(stderr, "exclusive-scale: peer create failed errno=%d\n",
                errno);
        goto done;
    }
    event.events = EPOLLIN | EPOLLEXCLUSIVE;
    for (i = 0; i < EXCLUSIVE_SCALE_REGISTRATIONS; i++) {
        event.data.u64 = (uint64_t)i + 1;
        if (epoll_ctl(epfd_peer, EPOLL_CTL_ADD, accepted[i], &event) != 0) {
            fprintf(stderr, "exclusive-scale: peer ADD %u failed errno=%d\n",
                    (unsigned)i, errno);
            goto done;
        }
    }
    {
        int n = epoll_wait(epfd_peer, events,
                           EXCLUSIVE_SCALE_REGISTRATIONS, 500);

        if (n != 0) {
            fprintf(stderr,
                    "exclusive-scale: peer observed duplicate readiness "
                    "n=%d token=%llu events=%u errno=%d\n",
                    n,
                    n > 0 ? (unsigned long long)events[0].data.u64 : 0ULL,
                    n > 0 ? events[0].events : 0U, errno);
            goto done;
        }
    }

    /* Releasing one owner must make exactly that still-readable base
     * available to the peer.  Removing the peer registration afterward keeps
     * the bulk-release check below free of level-triggered redelivery. */
    if (epoll_ctl(epfd_owner, EPOLL_CTL_DEL, accepted[released_index], NULL) !=
        0) {
        fprintf(stderr, "exclusive-scale: owner DEL failed errno=%d\n",
                errno);
        goto done;
    }
    {
        int n = epoll_wait(epfd_peer, events,
                           EXCLUSIVE_SCALE_REGISTRATIONS, 1000);
        uint64_t expected = (uint64_t)released_index + 1;

        if (n != 1 || events[0].data.u64 != expected ||
            (events[0].events & EPOLLIN) == 0) {
            fprintf(stderr,
                    "exclusive-scale: single release n=%d token=%llu "
                    "expected=%llu events=%u errno=%d\n",
                    n, n > 0 ? (unsigned long long)events[0].data.u64 : 0ULL,
                    (unsigned long long)expected,
                    n > 0 ? events[0].events : 0U, errno);
            goto done;
        }
    }
    if (epoll_ctl(epfd_peer, EPOLL_CTL_DEL, accepted[released_index], NULL) !=
        0) {
        fprintf(stderr, "exclusive-scale: peer DEL failed errno=%d\n",
                errno);
        goto done;
    }

    if (wepoll_close(epfd_owner) != 0) {
        fprintf(stderr, "exclusive-scale: owner close failed errno=%d\n",
                errno);
        goto done;
    }
    epfd_owner = -1;
    memset(seen, 0, EXCLUSIVE_SCALE_REGISTRATIONS * sizeof(*seen));
    seen_count = 0;
    bulk_deadline = GetTickCount64() + 5000;
    while (seen_count + 1 < EXCLUSIVE_SCALE_REGISTRATIONS) {
        ULONGLONG now = GetTickCount64();
        int timeout_ms;

        if (now >= bulk_deadline) {
            break;
        }
        timeout_ms = (int)(bulk_deadline - now);
        int n = epoll_wait(epfd_peer, events,
                           EXCLUSIVE_SCALE_REGISTRATIONS, timeout_ms);
        int j;

        if (n <= 0) {
            fprintf(stderr,
                    "exclusive-scale: bulk release stopped at %u n=%d "
                    "errno=%d\n",
                    (unsigned)seen_count, n, errno);
            goto done;
        }
        for (j = 0; j < n; j++) {
            uint64_t token = events[j].data.u64;
            size_t index;

            if (token == 0 || token > EXCLUSIVE_SCALE_REGISTRATIONS ||
                (events[j].events & EPOLLIN) == 0) {
                fprintf(stderr,
                        "exclusive-scale: invalid bulk event token=%llu "
                        "events=%u\n",
                        (unsigned long long)token, events[j].events);
                goto done;
            }
            index = (size_t)(token - 1);
            if (index == released_index) {
                fprintf(stderr,
                        "exclusive-scale: deleted peer token redelivered\n");
                goto done;
            }
            if (!seen[index]) {
                if (epoll_ctl(epfd_peer, EPOLL_CTL_DEL, accepted[index],
                              NULL) != 0) {
                    fprintf(stderr,
                            "exclusive-scale: bulk peer DEL token=%llu "
                            "failed errno=%d\n",
                            (unsigned long long)token, errno);
                    goto done;
                }
                seen[index] = 1;
                seen_count++;
            }
        }
    }
    if (seen_count + 1 != EXCLUSIVE_SCALE_REGISTRATIONS) {
        fprintf(stderr,
                "exclusive-scale: bulk release delivered only %u bases\n",
                (unsigned)seen_count);
        goto done;
    }

    puts("exclusive-scale: 129 concurrent claims and release lifecycle OK");
    result = 0;

done:
    if (epfd_peer >= 0) (void)wepoll_close(epfd_peer);
    if (epfd_owner >= 0) (void)wepoll_close(epfd_owner);
    if (accepted != NULL) {
        for (i = 0; i < pair_count; i++) {
            if (accepted[i] != INVALID_SOCKET) closesocket(accepted[i]);
        }
    }
    if (clients != NULL) {
        for (i = 0; i < pair_count; i++) {
            if (clients[i] != INVALID_SOCKET) closesocket(clients[i]);
        }
    }
    if (listeners != NULL) {
        for (i = 0; i < pair_count; i++) {
            if (listeners[i] != INVALID_SOCKET) closesocket(listeners[i]);
        }
    }
    free(seen);
    free(events);
    free(accepted);
    free(clients);
    free(listeners);
    return result;
}

static int test_exclusive_invalid_combos(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    int epfd = -1;

    if (make_loopback_pair(&listener, &client, &accepted) != 0)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLEXCLUSIVE | EPOLLONESHOT;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != -1 ||
        errno != EINVAL) {
        fprintf(stderr, "exclusive+oneshot: expected EINVAL errno=%d\n",
                errno);
        goto fail;
    }

    event.events = EPOLLIN | EPOLLRDHUP | EPOLLEXCLUSIVE;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != -1 ||
        errno != EINVAL) {
        fprintf(stderr, "exclusive+rdhup: expected EINVAL errno=%d\n", errno);
        goto fail;
    }

    event.events = EPOLLPRI | EPOLLEXCLUSIVE;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != -1 ||
        errno != EINVAL) {
        fprintf(stderr, "exclusive+pri: expected EINVAL errno=%d\n", errno);
        goto fail;
    }

    event.events = EPOLLIN | EPOLLEXCLUSIVE | EPOLLET;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0) {
        fprintf(stderr, "exclusive+et: ADD failed errno=%d\n", errno);
        goto fail;
    }
    /* DEL must retire the request submitted by ADD even though no wait has
     * run yet. */
    if (epoll_fd_count(epfd) != 1 ||
        epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL) != 0 ||
        epoll_fd_count(epfd) != 0) {
        fputs("exclusive+et: registration lifecycle failed\n", stderr);
        goto fail;
    }

    if (wepoll_close(epfd) != 0) {
        fprintf(stderr, "exclusive+et: close after DEL failed errno=%d\n",
                errno);
        goto fail;
    }
    epfd = -1;
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("exclusive-invalid: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_pwait_nonnull_sigmask(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    struct epoll_event events[1];
    struct epoll_event_ex events_ex[1];
    struct timespec zero;
    int dummy_mask = 0x5a5a5a5a;
    int epfd = -1;
    int n;

    if (make_loopback_pair(&listener, &client, &accepted) != 0)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = 99;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0)
        goto fail;
    if (send(client, "z", 1, 0) != 1)
        goto fail;

    /* Opaque non-null mask must be accepted and ignored on Windows. */
    n = epoll_pwait(epfd, events, 1, 1000,
                    (const wepoll_sigset_t *)&dummy_mask);
    if (n != 1 || (events[0].events & EPOLLIN) == 0) {
        fprintf(stderr, "pwait non-null mask failed n=%d errno=%d\n",
                n, errno);
        goto fail;
    }

    if (send(client, "y", 1, 0) != 1)
        goto fail;
    zero.tv_sec = 0;
    zero.tv_nsec = 0;
    n = epoll_pwait2_ex(epfd, events_ex, 1, &zero,
                        (const wepoll_sigset_t *)&dummy_mask);
    if (n != 1 || (events_ex[0].events & EPOLLIN) == 0) {
        fprintf(stderr, "pwait2_ex non-null mask failed n=%d errno=%d\n",
                n, errno);
        goto fail;
    }

    (void)wepoll_close(epfd);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("pwait-sigmask: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_wakeup_flag_accepted(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    int epfd = -1;

    if (make_loopback_pair(&listener, &client, &accepted) != 0)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    /* EPOLLWAKEUP has no freezable-task equivalent on Windows; accept and
     * ignore it so portable event masks do not fail registration. */
    event.events = EPOLLIN | EPOLLWAKEUP;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0 ||
        epoll_fd_count(epfd) != 1) {
        fprintf(stderr, "EPOLLWAKEUP ADD failed errno=%d\n", errno);
        goto fail;
    }

    /* Closing the epoll instance before its first wait must cancel and drain
     * the AFD request synchronously submitted by ADD. */
    if (wepoll_close(epfd) != 0) {
        fprintf(stderr, "EPOLLWAKEUP close-before-wait failed errno=%d\n",
                errno);
        goto fail;
    }
    epfd = -1;
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("wakeup-flag: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int run_mode(const char *mode)
{
    if (strcmp(mode, "readable") == 0)
        return test_edge_readable();
    if (strcmp(mode, "same-epfd-et") == 0)
        return test_same_epfd_et_single_wake();
    if (strcmp(mode, "writable") == 0)
        return test_edge_writable_no_spin();
    if (strcmp(mode, "explicit-rearm") == 0)
        return test_explicit_rearm_directional();
    if (strcmp(mode, "explicit-terminal") == 0)
        return test_explicit_rearm_terminal();
    if (strcmp(mode, "explicit-fin") == 0)
        return test_explicit_rearm_fin();
    if (strcmp(mode, "explicit-oneshot") == 0)
        return test_explicit_rearm_oneshot();
    if (strcmp(mode, "local-shutdown-lt") == 0)
        return test_local_shutdown(LOCAL_SHUTDOWN_LT);
    if (strcmp(mode, "local-shutdown-et") == 0)
        return test_local_shutdown(LOCAL_SHUTDOWN_ET);
    if (strcmp(mode, "local-shutdown-oneshot") == 0)
        return test_local_shutdown(LOCAL_SHUTDOWN_ONESHOT);
    if (strcmp(mode, "local-shutdown-explicit") == 0)
        return test_local_shutdown(LOCAL_SHUTDOWN_EXPLICIT);
    if (strcmp(mode, "local-shutdown-mod-wake") == 0)
        return test_local_shutdown_mod_wake();
    if (strcmp(mode, "local-shutdown-rearm-wake") == 0)
        return test_local_shutdown_rearm_wake();
    if (strcmp(mode, "explicit-contract") == 0)
        return test_explicit_rearm_contract();
    if (strcmp(mode, "exclusive-mod") == 0)
        return test_exclusive_mod_rejected();
    if (strcmp(mode, "exclusive-wake") == 0)
        return test_exclusive_two_cycles(0);
    if (strcmp(mode, "exclusive-et") == 0)
        return test_exclusive_two_cycles(1);
    if (strcmp(mode, "exclusive-mixed") == 0)
        return test_exclusive_mixed_waiters();
    if (strcmp(mode, "ordinary-multi") == 0)
        return test_ordinary_multi_waiters();
    if (strcmp(mode, "ordinary-prearmed") == 0)
        return test_ordinary_prearmed_waiters();
    if (strcmp(mode, "exclusive-disjoint") == 0)
        return test_exclusive_disjoint_classes();
    if (strcmp(mode, "exclusive-scale") == 0)
        return test_exclusive_scale();
    if (strcmp(mode, "exclusive-invalid") == 0)
        return test_exclusive_invalid_combos();
    if (strcmp(mode, "pwait-sigmask") == 0)
        return test_pwait_nonnull_sigmask();
    if (strcmp(mode, "wakeup-flag") == 0)
        return test_wakeup_flag_accepted();
    fprintf(stderr, "unknown edge mode: %s\n", mode);
    return 2;
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    int failures = 0;
    const char *modes[] = {
        "readable", "same-epfd-et", "writable", "explicit-rearm",
        "explicit-terminal", "explicit-fin", "explicit-oneshot",
        "local-shutdown-lt", "local-shutdown-et",
        "local-shutdown-oneshot", "local-shutdown-explicit",
        "local-shutdown-mod-wake", "local-shutdown-rearm-wake",
        "explicit-contract",
        "exclusive-mod",
        "exclusive-wake",
        "exclusive-et", "exclusive-mixed", "ordinary-multi",
        "ordinary-prearmed", "exclusive-disjoint", "exclusive-scale",
        "exclusive-invalid", "pwait-sigmask", "wakeup-flag"
    };
    size_t i;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fputs("WSAStartup failed\n", stderr);
        return 2;
    }

    if (argc > 1) {
        int rc = run_mode(argv[1]);
        WSACleanup();
        return rc;
    }

    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        int rc = run_mode(modes[i]);
        if (rc != 0)
            failures++;
    }
    WSACleanup();
    return failures == 0 ? 0 : 1;
}

#else
int main(void)
{
    return 0;
}
#endif
