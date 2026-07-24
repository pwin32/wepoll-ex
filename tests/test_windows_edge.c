#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32

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
    event.events = EPOLLIN;
    event.data.u64 = 3;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0)
        goto fail;

    event.events = EPOLLIN | EPOLLEXCLUSIVE;
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

static int test_exclusive_single_wake(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    struct epoll_event events[2];
    int epfd_a = -1;
    int epfd_b = -1;
    int got_a = 0;
    int got_b = 0;
    int n;
    int i;

    if (make_loopback_pair(&listener, &client, &accepted) != 0)
        return 1;

    epfd_a = epoll_create1(0);
    epfd_b = epoll_create1(0);
    if (epfd_a < 0 || epfd_b < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLEXCLUSIVE;
    event.data.u64 = 11;
    if (epoll_ctl(epfd_a, EPOLL_CTL_ADD, accepted, &event) != 0) {
        fprintf(stderr, "exclusive: ADD A failed errno=%d\n", errno);
        goto fail;
    }

    event.events = EPOLLIN | EPOLLEXCLUSIVE;
    event.data.u64 = 22;
    if (epoll_ctl(epfd_b, EPOLL_CTL_ADD, accepted, &event) != 0) {
        fprintf(stderr, "exclusive: ADD B failed errno=%d\n", errno);
        goto fail;
    }

    /* Arm both instances by entering wait briefly. */
    (void)epoll_wait(epfd_a, events, 1, 0);
    (void)epoll_wait(epfd_b, events, 1, 0);

    if (send(client, "x", 1, 0) != 1)
        goto fail;

    for (i = 0; i < 40; i++) {
        n = epoll_wait(epfd_a, events, 1, 25);
        if (n < 0)
            goto fail;
        if (n == 1 && (events[0].events & EPOLLIN) != 0)
            got_a++;
        n = epoll_wait(epfd_b, events, 1, 25);
        if (n < 0)
            goto fail;
        if (n == 1 && (events[0].events & EPOLLIN) != 0)
            got_b++;
        if ((got_a + got_b) > 0 && i > 8)
            break;
    }

    if (!((got_a == 1 && got_b == 0) || (got_a == 0 && got_b == 1))) {
        fprintf(stderr,
                "exclusive: expected one instance once, got A=%d B=%d\n",
                got_a, got_b);
        goto fail;
    }

    (void)wepoll_close(epfd_a);
    (void)wepoll_close(epfd_b);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("exclusive-wake: OK");
    return 0;

fail:
    if (epfd_a >= 0) (void)wepoll_close(epfd_a);
    if (epfd_b >= 0) (void)wepoll_close(epfd_b);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int run_mode(const char *mode)
{
    if (strcmp(mode, "readable") == 0)
        return test_edge_readable();
    if (strcmp(mode, "writable") == 0)
        return test_edge_writable_no_spin();
    if (strcmp(mode, "exclusive-mod") == 0)
        return test_exclusive_mod_rejected();
    if (strcmp(mode, "exclusive-wake") == 0)
        return test_exclusive_single_wake();
    fprintf(stderr, "unknown edge mode: %s\n", mode);
    return 2;
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    int failures = 0;
    const char *modes[] = {
        "readable", "writable", "exclusive-mod", "exclusive-wake"
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
