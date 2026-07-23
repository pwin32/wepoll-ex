/*
 * test_windows_ipv6.c -- native Windows IPv6 readiness regressions.
 *
 * Exercises the AFD-backed path with an IPv6 loopback listener, a connected
 * stream in both directions, and a remote half-close.  Missing IPv6 protocol
 * or loopback-address support is the only condition treated as a skip.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex.h"

#ifdef _WIN32

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct ipv6_fixture {
    SOCKET listener;
    SOCKET client;
    SOCKET server;
    int epfd;
} ipv6_fixture_t;

enum setup_result {
    SETUP_FAILED = -1,
    SETUP_OK = 0,
    SETUP_IPV6_UNAVAILABLE = 1
};

static void fixture_init(ipv6_fixture_t *fixture)
{
    fixture->listener = INVALID_SOCKET;
    fixture->client = INVALID_SOCKET;
    fixture->server = INVALID_SOCKET;
    fixture->epfd = -1;
}

static void fixture_close(ipv6_fixture_t *fixture)
{
    if (fixture->epfd >= 0) {
        (void)wepoll_close(fixture->epfd);
        fixture->epfd = -1;
    }
    if (fixture->server != INVALID_SOCKET) {
        closesocket(fixture->server);
        fixture->server = INVALID_SOCKET;
    }
    if (fixture->client != INVALID_SOCKET) {
        closesocket(fixture->client);
        fixture->client = INVALID_SOCKET;
    }
    if (fixture->listener != INVALID_SOCKET) {
        closesocket(fixture->listener);
        fixture->listener = INVALID_SOCKET;
    }
}

static int ipv6_unavailable_error(int error)
{
    return error == WSAEAFNOSUPPORT ||
           error == WSAEPROTONOSUPPORT ||
           error == WSAEADDRNOTAVAIL;
}

static int set_nonblocking(SOCKET socket)
{
    u_long enabled = 1;

    return ioctlsocket(socket, FIONBIO, &enabled) == 0 ? 0 : -1;
}

static int add_socket(int epfd, SOCKET socket, uint32_t events,
                      uint64_t data)
{
    struct epoll_event event;

    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.u64 = data;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, socket, &event);
}

static int wait_for_mask(int epfd, uint64_t data, uint32_t mask,
                         const char *phase)
{
    struct epoll_event output;
    int count = epoll_wait(epfd, &output, 1, 2000);

    if (count != 1) {
        fprintf(stderr, "%s: expected one event, got %d "
                "(errno=%d WSA=%d)\n",
                phase, count, errno, WSAGetLastError());
        return -1;
    }
    if (output.data.u64 != data || (output.events & mask) != mask) {
        fprintf(stderr,
                "%s: expected data=0x%llx mask=0x%08lx, "
                "got data=0x%llx events=0x%08lx\n",
                phase, (unsigned long long)data, (unsigned long)mask,
                (unsigned long long)output.data.u64,
                (unsigned long)output.events);
        return -1;
    }
    return 0;
}

static int wait_for_rdhup(int epfd, uint64_t data)
{
    ULONGLONG deadline = GetTickCount64() + UINT64_C(2000);

    for (;;) {
        struct epoll_event output;
        ULONGLONG now = GetTickCount64();
        int remaining;
        int count;

        if (now >= deadline) {
            fprintf(stderr, "IPv6 half-close: timed out waiting for RDHUP\n");
            return -1;
        }
        remaining = (int)(deadline - now);
        count = epoll_wait(epfd, &output, 1, remaining);
        if (count != 1) {
            fprintf(stderr,
                    "IPv6 half-close: expected one event, got %d "
                    "(errno=%d WSA=%d)\n",
                    count, errno, WSAGetLastError());
            return -1;
        }
        if (output.data.u64 != data) {
            fprintf(stderr,
                    "IPv6 half-close: event data mismatch, got 0x%llx\n",
                    (unsigned long long)output.data.u64);
            return -1;
        }
        if ((output.events & EPOLLRDHUP) != 0) {
            return 0;
        }

        /* A level-triggered read may already have been re-queued before the
         * preceding byte was consumed.  Ignore only that stale readable
         * notification while waiting for the subsequent FIN completion. */
        if ((output.events & ~EPOLLIN) != 0 ||
            (output.events & EPOLLIN) == 0) {
            fprintf(stderr,
                    "IPv6 half-close: unexpected events 0x%08lx\n",
                    (unsigned long)output.events);
            return -1;
        }
    }
}

static int create_ipv6_listener(ipv6_fixture_t *fixture,
                                struct sockaddr_in6 *address)
{
    static const IN6_ADDR loopback = IN6ADDR_LOOPBACK_INIT;
    DWORD ipv6_only = 1;
    int address_length = (int)sizeof(*address);
    int error;

    fixture->listener = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (fixture->listener == INVALID_SOCKET) {
        error = WSAGetLastError();
        return ipv6_unavailable_error(error) ?
            SETUP_IPV6_UNAVAILABLE : SETUP_FAILED;
    }

    if (setsockopt(fixture->listener, IPPROTO_IPV6, IPV6_V6ONLY,
                   (const char *)&ipv6_only,
                   (int)sizeof(ipv6_only)) == SOCKET_ERROR) {
        return SETUP_FAILED;
    }

    memset(address, 0, sizeof(*address));
    address->sin6_family = AF_INET6;
    address->sin6_addr = loopback;
    address->sin6_port = htons(0);
    if (bind(fixture->listener, (const struct sockaddr *)address,
             (int)sizeof(*address)) == SOCKET_ERROR) {
        error = WSAGetLastError();
        return ipv6_unavailable_error(error) ?
            SETUP_IPV6_UNAVAILABLE : SETUP_FAILED;
    }
    if (listen(fixture->listener, 1) == SOCKET_ERROR ||
        getsockname(fixture->listener, (struct sockaddr *)address,
                    &address_length) == SOCKET_ERROR ||
        set_nonblocking(fixture->listener) != 0) {
        return SETUP_FAILED;
    }
    return SETUP_OK;
}

static int test_ipv6_readiness(void)
{
    enum {
        LISTENER_DATA = 0x6001,
        CLIENT_DATA = 0x6002,
        SERVER_DATA = 0x6003
    };
    ipv6_fixture_t fixture;
    struct sockaddr_in6 address;
    struct sockaddr_in6 peer_address;
    int peer_address_length = (int)sizeof(peer_address);
    const char sent_byte = '6';
    char received_byte = 0;
    int setup_result;
    int result = -1;

    fixture_init(&fixture);
    setup_result = create_ipv6_listener(&fixture, &address);
    if (setup_result != SETUP_OK) {
        int error = WSAGetLastError();

        fixture_close(&fixture);
        if (setup_result == SETUP_IPV6_UNAVAILABLE) {
            printf("IPv6 readiness: SKIP (IPv6 loopback unavailable, WSA=%d)\n",
                   error);
            return 1;
        }
        fprintf(stderr, "IPv6 listener setup failed, WSA=%d\n", error);
        return -1;
    }

    fixture.epfd = epoll_create1(0);
    if (fixture.epfd < 0 ||
        add_socket(fixture.epfd, fixture.listener, EPOLLIN,
                   LISTENER_DATA) != 0) {
        fprintf(stderr, "IPv6 listener registration failed, errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }

    fixture.client = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (fixture.client == INVALID_SOCKET ||
        connect(fixture.client, (const struct sockaddr *)&address,
                (int)sizeof(address)) == SOCKET_ERROR ||
        wait_for_mask(fixture.epfd, LISTENER_DATA, EPOLLIN,
                      "IPv6 accept readiness") != 0) {
        fprintf(stderr, "IPv6 loopback connect/accept readiness failed, "
                "errno=%d WSA=%d\n", errno, WSAGetLastError());
        goto cleanup;
    }

    memset(&peer_address, 0, sizeof(peer_address));
    fixture.server = accept(fixture.listener,
                            (struct sockaddr *)&peer_address,
                            &peer_address_length);
    if (fixture.server == INVALID_SOCKET ||
        peer_address.sin6_family != AF_INET6 ||
        set_nonblocking(fixture.client) != 0 ||
        set_nonblocking(fixture.server) != 0 ||
        epoll_ctl(fixture.epfd, EPOLL_CTL_DEL,
                  fixture.listener, NULL) != 0) {
        fprintf(stderr, "IPv6 accept failed, errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }
    closesocket(fixture.listener);
    fixture.listener = INVALID_SOCKET;

    if (add_socket(fixture.epfd, fixture.client, EPOLLOUT,
                   CLIENT_DATA) != 0 ||
        wait_for_mask(fixture.epfd, CLIENT_DATA, EPOLLOUT,
                      "IPv6 connected write readiness") != 0 ||
        epoll_ctl(fixture.epfd, EPOLL_CTL_DEL,
                  fixture.client, NULL) != 0) {
        fprintf(stderr, "IPv6 write readiness failed, errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }

    if (add_socket(fixture.epfd, fixture.server,
                   EPOLLIN | EPOLLRDHUP, SERVER_DATA) != 0 ||
        send(fixture.client, &sent_byte, 1, 0) != 1 ||
        wait_for_mask(fixture.epfd, SERVER_DATA, EPOLLIN,
                      "IPv6 connected read readiness") != 0 ||
        recv(fixture.server, &received_byte, 1, 0) != 1 ||
        received_byte != sent_byte) {
        fprintf(stderr, "IPv6 read readiness failed, errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }

    if (shutdown(fixture.client, SD_SEND) == SOCKET_ERROR ||
        wait_for_rdhup(fixture.epfd, SERVER_DATA) != 0 ||
        recv(fixture.server, &received_byte, 1, 0) != 0 ||
        epoll_ctl(fixture.epfd, EPOLL_CTL_DEL,
                  fixture.server, NULL) != 0) {
        fprintf(stderr, "IPv6 half-close handling failed, errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }

    result = 0;

cleanup:
    fixture_close(&fixture);
    if (result == 0) {
        puts("IPv6 accept/read/write/RDHUP: OK");
    }
    return result;
}

int main(void)
{
    WSADATA wsa_data;
    int result;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 2;
    }
    result = test_ipv6_readiness();
    WSACleanup();
    if (result == 1) return 77;
    return result == 0 ? 0 : 1;
}

#else

int main(void)
{
    return 0;
}

#endif
