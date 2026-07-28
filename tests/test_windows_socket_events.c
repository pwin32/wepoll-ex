/*
 * Public Windows socket-event regressions.  These tests intentionally use
 * only the installed API surface so the same modes run against static and
 * shared builds.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex.h"

#ifdef _WIN32

#include <errno.h>
#include <mswsock.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum test_result {
    TEST_FAILED = -1,
    TEST_OK = 0,
    TEST_SKIPPED = 1
};

typedef struct tcp_pair {
    SOCKET client;
    SOCKET server;
} tcp_pair_t;

typedef struct udp_fixture {
    SOCKET receiver;
    SOCKET sender;
    struct sockaddr_storage address;
    int address_length;
    int family;
} udp_fixture_t;

static void tcp_pair_init(tcp_pair_t *pair)
{
    pair->client = INVALID_SOCKET;
    pair->server = INVALID_SOCKET;
}

static void tcp_pair_close(tcp_pair_t *pair)
{
    if (pair->server != INVALID_SOCKET) {
        closesocket(pair->server);
        pair->server = INVALID_SOCKET;
    }
    if (pair->client != INVALID_SOCKET) {
        closesocket(pair->client);
        pair->client = INVALID_SOCKET;
    }
}

static int make_tcp_pair(tcp_pair_t *pair)
{
    SOCKET listener = INVALID_SOCKET;
    struct sockaddr_in address;
    int address_length = (int)sizeof(address);
    u_long nonblocking = 1;

    tcp_pair_init(pair);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (bind(listener, (const struct sockaddr *)&address,
             (int)sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR ||
        getsockname(listener, (struct sockaddr *)&address,
                    &address_length) == SOCKET_ERROR) {
        closesocket(listener);
        return -1;
    }

    pair->client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pair->client == INVALID_SOCKET ||
        connect(pair->client, (const struct sockaddr *)&address,
                (int)sizeof(address)) == SOCKET_ERROR) {
        closesocket(listener);
        tcp_pair_close(pair);
        return -1;
    }
    pair->server = accept(listener, NULL, NULL);
    closesocket(listener);
    if (pair->server == INVALID_SOCKET ||
        ioctlsocket(pair->server, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        tcp_pair_close(pair);
        return -1;
    }
    return 0;
}

static int ctl_socket(int epfd, int operation, SOCKET socket_fd,
                      uint32_t events, uint64_t data)
{
    struct epoll_event event;

    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.u64 = data;
    return epoll_ctl(epfd, operation, socket_fd,
                     operation == EPOLL_CTL_DEL ? NULL : &event);
}

static int wait_exact(int epfd, int timeout_ms, uint64_t expected_data,
                      uint32_t expected_events, const char *name)
{
    struct epoll_event output;
    int count;

    memset(&output, 0, sizeof(output));
    count = epoll_wait(epfd, &output, 1, timeout_ms);
    if (count != 1 || output.data.u64 != expected_data ||
        output.events != expected_events) {
        fprintf(stderr,
                "%s: count=%d errno=%d WSA=%d data=0x%llx "
                "events=0x%08lx expected_data=0x%llx "
                "expected_events=0x%08lx\n",
                name, count, errno, WSAGetLastError(),
                (unsigned long long)output.data.u64,
                (unsigned long)output.events,
                (unsigned long long)expected_data,
                (unsigned long)expected_events);
        return -1;
    }
    return 0;
}

static int wait_empty(int epfd, int timeout_ms, const char *name)
{
    struct epoll_event output;
    int count;

    memset(&output, 0, sizeof(output));
    count = epoll_wait(epfd, &output, 1, timeout_ms);
    if (count != 0) {
        fprintf(stderr, "%s: expected no event, count=%d errno=%d "
                "events=0x%08lx\n", name, count, errno,
                (unsigned long)output.events);
        return -1;
    }
    return 0;
}

static int send_normal(SOCKET socket_fd, char byte)
{
    return send(socket_fd, &byte, 1, 0) == 1 ? 0 : -1;
}

static int recv_normal(SOCKET socket_fd, char expected)
{
    char byte = 0;

    return recv(socket_fd, &byte, 1, 0) == 1 && byte == expected ? 0 : -1;
}

static int send_oob(SOCKET socket_fd, char byte)
{
    return send(socket_fd, &byte, 1, MSG_OOB) == 1 ? 0 : -1;
}

static int recv_oob(SOCKET socket_fd, char expected)
{
    char byte = 0;

    return recv(socket_fd, &byte, 1, MSG_OOB) == 1 && byte == expected
        ? 0 : -1;
}

static int run_read_alias_case(uint32_t interest, uint32_t expected,
                               uint64_t data, const char *name)
{
    tcp_pair_t pair;
    int epfd = -1;
    int registered = 0;
    int result = -1;

    if (make_tcp_pair(&pair) != 0) {
        return -1;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        ctl_socket(epfd, EPOLL_CTL_ADD, pair.server, interest, data) != 0) {
        goto cleanup;
    }
    registered = 1;
    if (send_normal(pair.client, 'r') != 0 ||
        wait_exact(epfd, 2000, data, expected, name) != 0 ||
        recv_normal(pair.server, 'r') != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (registered &&
        ctl_socket(epfd, EPOLL_CTL_DEL, pair.server, 0, 0) != 0) {
        result = -1;
    }
    if (epfd >= 0) {
        (void)wepoll_close(epfd);
    }
    tcp_pair_close(&pair);
    return result;
}

static int run_write_alias_case(uint32_t interest, uint32_t expected,
                                uint64_t data, const char *name)
{
    tcp_pair_t pair;
    int epfd = -1;
    int registered = 0;
    int result = -1;

    if (make_tcp_pair(&pair) != 0) {
        return -1;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        ctl_socket(epfd, EPOLL_CTL_ADD, pair.client, interest, data) != 0) {
        goto cleanup;
    }
    registered = 1;
    if (wait_exact(epfd, 2000, data, expected, name) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (registered &&
        ctl_socket(epfd, EPOLL_CTL_DEL, pair.client, 0, 0) != 0) {
        result = -1;
    }
    if (epfd >= 0) {
        (void)wepoll_close(epfd);
    }
    tcp_pair_close(&pair);
    return result;
}

static int test_aliases(void)
{
    if (run_read_alias_case(EPOLLRDNORM, EPOLLRDNORM,
                            UINT64_C(0x4101), "RDNORM") != 0 ||
        run_read_alias_case(EPOLLIN | EPOLLRDNORM,
                            EPOLLIN | EPOLLRDNORM,
                            UINT64_C(0x4102), "IN+RDNORM") != 0 ||
        run_write_alias_case(EPOLLWRNORM, EPOLLWRNORM,
                             UINT64_C(0x4103), "WRNORM") != 0 ||
        run_write_alias_case(EPOLLWRBAND, EPOLLWRBAND,
                             UINT64_C(0x4104), "WRBAND") != 0 ||
        run_write_alias_case(EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND,
                             EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND,
                             UINT64_C(0x4105), "write aliases") != 0) {
        return -1;
    }
    puts("aliases: OK");
    return 0;
}

static int run_oob_lt_case(uint32_t interest, uint32_t expected,
                           uint64_t data, const char *name)
{
    tcp_pair_t pair;
    int epfd = -1;
    int registered = 0;
    int result = -1;

    if (make_tcp_pair(&pair) != 0) {
        return -1;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        ctl_socket(epfd, EPOLL_CTL_ADD, pair.server, interest, data) != 0) {
        goto cleanup;
    }
    registered = 1;
    if (send_normal(pair.client, 'n') != 0 ||
        send_oob(pair.client, '!') != 0 ||
        wait_exact(epfd, 2000, data, expected, name) != 0 ||
        wait_exact(epfd, 2000, data, expected, name) != 0 ||
        recv_oob(pair.server, '!') != 0) {
        goto cleanup;
    }
    if ((interest & (EPOLLIN | EPOLLRDNORM)) == 0 &&
        wait_empty(epfd, 100, name) != 0) {
        goto cleanup;
    }
    if (recv_normal(pair.server, 'n') != 0 ||
        wait_empty(epfd, 100, name) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (registered &&
        ctl_socket(epfd, EPOLL_CTL_DEL, pair.server, 0, 0) != 0) {
        result = -1;
    }
    if (epfd >= 0) {
        (void)wepoll_close(epfd);
    }
    tcp_pair_close(&pair);
    return result;
}

static int test_oob_lt(void)
{
    if (run_oob_lt_case(EPOLLPRI, EPOLLPRI, UINT64_C(0x4201),
                        "PRI LT") != 0 ||
        run_oob_lt_case(EPOLLRDBAND, EPOLLRDBAND, UINT64_C(0x4202),
                        "RDBAND LT") != 0 ||
        run_oob_lt_case(EPOLLIN | EPOLLPRI, EPOLLIN | EPOLLPRI,
                        UINT64_C(0x4203), "IN+PRI LT") != 0) {
        return -1;
    }
    puts("oob-lt: OK");
    return 0;
}

static int test_oob_et(void)
{
    tcp_pair_t pair;
    const uint64_t data = UINT64_C(0x4301);
    int epfd = -1;
    int registered = 0;
    int result = -1;

    if (make_tcp_pair(&pair) != 0) {
        return -1;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        ctl_socket(epfd, EPOLL_CTL_ADD, pair.server,
                   EPOLLPRI | EPOLLET, data) != 0) {
        goto cleanup;
    }
    registered = 1;
    if (send_oob(pair.client, 'a') != 0 ||
        wait_exact(epfd, 2000, data, EPOLLPRI, "PRI ET first") != 0 ||
        wait_empty(epfd, 100, "PRI ET duplicate") != 0 ||
        recv_oob(pair.server, 'a') != 0 ||
        wait_empty(epfd, 100, "PRI ET reset") != 0 ||
        send_oob(pair.client, 'b') != 0 ||
        wait_exact(epfd, 2000, data, EPOLLPRI, "PRI ET re-edge") != 0 ||
        recv_oob(pair.server, 'b') != 0 ||
        wait_empty(epfd, 100, "PRI ET drained") != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (registered &&
        ctl_socket(epfd, EPOLL_CTL_DEL, pair.server, 0, 0) != 0) {
        result = -1;
    }
    if (epfd >= 0) {
        (void)wepoll_close(epfd);
    }
    tcp_pair_close(&pair);
    if (result == 0) {
        puts("oob-et: OK");
    }
    return result;
}

static int test_oob_oneshot(void)
{
    tcp_pair_t pair;
    const uint64_t first_data = UINT64_C(0x4401);
    const uint64_t rearm_data = UINT64_C(0x4402);
    int epfd = -1;
    int registered = 0;
    int result = -1;

    if (make_tcp_pair(&pair) != 0) {
        return -1;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        ctl_socket(epfd, EPOLL_CTL_ADD, pair.server,
                   EPOLLPRI | EPOLLONESHOT, first_data) != 0) {
        goto cleanup;
    }
    registered = 1;
    if (send_oob(pair.client, 'o') != 0 ||
        wait_exact(epfd, 2000, first_data, EPOLLPRI,
                   "PRI ONESHOT first") != 0 ||
        wait_empty(epfd, 100, "PRI ONESHOT disabled") != 0 ||
        ctl_socket(epfd, EPOLL_CTL_MOD, pair.server,
                   EPOLLPRI | EPOLLONESHOT, rearm_data) != 0 ||
        wait_exact(epfd, 2000, rearm_data, EPOLLPRI,
                   "PRI ONESHOT rearm") != 0 ||
        recv_oob(pair.server, 'o') != 0 ||
        wait_empty(epfd, 100, "PRI ONESHOT drained") != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (registered &&
        ctl_socket(epfd, EPOLL_CTL_DEL, pair.server, 0, 0) != 0) {
        result = -1;
    }
    if (epfd >= 0) {
        (void)wepoll_close(epfd);
    }
    tcp_pair_close(&pair);
    if (result == 0) {
        puts("oob-oneshot: OK");
    }
    return result;
}

static int test_oob_mod(void)
{
    tcp_pair_t pair;
    const uint64_t in_data = UINT64_C(0x4501);
    const uint64_t pri_data = UINT64_C(0x4502);
    const uint64_t final_data = UINT64_C(0x4503);
    int epfd = -1;
    int registered = 0;
    int result = -1;

    if (make_tcp_pair(&pair) != 0) {
        return -1;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        ctl_socket(epfd, EPOLL_CTL_ADD, pair.server,
                   EPOLLIN, in_data) != 0) {
        goto cleanup;
    }
    registered = 1;
    if (send_normal(pair.client, 'm') != 0 ||
        send_oob(pair.client, 'p') != 0 ||
        wait_exact(epfd, 2000, in_data, EPOLLIN, "MOD IN") != 0 ||
        ctl_socket(epfd, EPOLL_CTL_MOD, pair.server,
                   EPOLLPRI, pri_data) != 0 ||
        wait_exact(epfd, 2000, pri_data, EPOLLPRI, "MOD PRI") != 0 ||
        recv_oob(pair.server, 'p') != 0 ||
        ctl_socket(epfd, EPOLL_CTL_MOD, pair.server,
                   EPOLLIN, final_data) != 0 ||
        wait_exact(epfd, 2000, final_data, EPOLLIN, "MOD IN restore") != 0 ||
        recv_normal(pair.server, 'm') != 0 ||
        wait_empty(epfd, 100, "MOD drained") != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (registered &&
        ctl_socket(epfd, EPOLL_CTL_DEL, pair.server, 0, 0) != 0) {
        result = -1;
    }
    if (epfd >= 0) {
        (void)wepoll_close(epfd);
    }
    tcp_pair_close(&pair);
    if (result == 0) {
        puts("oob-mod: OK");
    }
    return result;
}

static void udp_fixture_init(udp_fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->receiver = INVALID_SOCKET;
    fixture->sender = INVALID_SOCKET;
    fixture->family = AF_UNSPEC;
}

static void udp_fixture_close(udp_fixture_t *fixture)
{
    if (fixture->sender != INVALID_SOCKET) {
        closesocket(fixture->sender);
        fixture->sender = INVALID_SOCKET;
    }
    if (fixture->receiver != INVALID_SOCKET) {
        closesocket(fixture->receiver);
        fixture->receiver = INVALID_SOCKET;
    }
}

static int ipv6_unavailable_error(int error)
{
    return error == WSAEAFNOSUPPORT || error == WSAEPROTONOSUPPORT ||
           error == WSAEADDRNOTAVAIL || error == WSAEINVAL;
}

static int make_udp_fixture(udp_fixture_t *fixture, int family)
{
    int address_length;
    u_long nonblocking = 1;

    udp_fixture_init(fixture);
    fixture->family = family;
    fixture->receiver = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fixture->receiver == INVALID_SOCKET) {
        return family == AF_INET6 && ipv6_unavailable_error(WSAGetLastError())
            ? TEST_SKIPPED : TEST_FAILED;
    }

    if (family == AF_INET) {
        struct sockaddr_in *address =
            (struct sockaddr_in *)&fixture->address;

        memset(address, 0, sizeof(*address));
        address->sin_family = AF_INET;
        address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address->sin_port = htons(0);
        address_length = (int)sizeof(*address);
    } else {
        struct sockaddr_in6 *address =
            (struct sockaddr_in6 *)&fixture->address;

        memset(address, 0, sizeof(*address));
        address->sin6_family = AF_INET6;
        address->sin6_addr = in6addr_loopback;
        address->sin6_port = htons(0);
        address_length = (int)sizeof(*address);
    }
    if (bind(fixture->receiver,
             (const struct sockaddr *)&fixture->address,
             address_length) == SOCKET_ERROR ||
        getsockname(fixture->receiver,
                    (struct sockaddr *)&fixture->address,
                    &address_length) == SOCKET_ERROR ||
        ioctlsocket(fixture->receiver, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        int error = WSAGetLastError();

        udp_fixture_close(fixture);
        return family == AF_INET6 && ipv6_unavailable_error(error)
            ? TEST_SKIPPED : TEST_FAILED;
    }
    fixture->address_length = address_length;
    fixture->sender = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fixture->sender == INVALID_SOCKET) {
        int error = WSAGetLastError();

        udp_fixture_close(fixture);
        return family == AF_INET6 && ipv6_unavailable_error(error)
            ? TEST_SKIPPED : TEST_FAILED;
    }
    return TEST_OK;
}

static int test_udp_readiness(int family)
{
    udp_fixture_t fixture;
    const uint64_t data = family == AF_INET
        ? UINT64_C(0x4604) : UINT64_C(0x4606);
    const char byte = family == AF_INET ? '4' : '6';
    char received = 0;
    int epfd = -1;
    int registered = 0;
    int setup;
    int result = -1;

    setup = make_udp_fixture(&fixture, family);
    if (setup == TEST_SKIPPED) {
        printf("UDP IPv%d readiness: SKIP (address family unavailable)\n",
               family == AF_INET ? 4 : 6);
        return TEST_SKIPPED;
    }
    if (setup != TEST_OK) {
        return TEST_FAILED;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        ctl_socket(epfd, EPOLL_CTL_ADD, fixture.receiver,
                   EPOLLIN, data) != 0) {
        goto cleanup;
    }
    registered = 1;
    if (sendto(fixture.sender, &byte, 1, 0,
               (const struct sockaddr *)&fixture.address,
               fixture.address_length) != 1 ||
        wait_exact(epfd, 2000, data, EPOLLIN, "UDP readiness") != 0) {
        goto cleanup;
    }
    if (recv(fixture.receiver, &received, 1, 0) != 1 || received != byte) {
        goto cleanup;
    }
    result = TEST_OK;

cleanup:
    if (registered &&
        ctl_socket(epfd, EPOLL_CTL_DEL, fixture.receiver, 0, 0) != 0) {
        result = TEST_FAILED;
    }
    if (epfd >= 0) {
        (void)wepoll_close(epfd);
    }
    udp_fixture_close(&fixture);
    if (result == TEST_OK) {
        printf("udp-v%d: OK\n", family == AF_INET ? 4 : 6);
    }
    return result;
}

static int udp_connreset_unsupported(int error)
{
    return error == WSAEINVAL || error == WSAEOPNOTSUPP ||
           error == WSAENOPROTOOPT;
}

static int test_udp_error(void)
{
    struct sockaddr_in address;
    SOCKET reserved = INVALID_SOCKET;
    SOCKET sender = INVALID_SOCKET;
    struct epoll_event output;
    const uint64_t data = UINT64_C(0x46455252);
    BOOL enabled = TRUE;
    DWORD bytes_returned = 0;
    char byte;
    int address_length = (int)sizeof(address);
    int epfd = -1;
    int registered = 0;
    int result = TEST_FAILED;
    int wait_count;

    reserved = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (reserved == INVALID_SOCKET ||
        bind(reserved, (const struct sockaddr *)&address,
             (int)sizeof(address)) == SOCKET_ERROR ||
        getsockname(reserved, (struct sockaddr *)&address,
                    &address_length) == SOCKET_ERROR) {
        goto cleanup;
    }
    closesocket(reserved);
    reserved = INVALID_SOCKET;

    sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sender == INVALID_SOCKET ||
        WSAIoctl(sender, SIO_UDP_CONNRESET,
                 &enabled, (DWORD)sizeof(enabled), NULL, 0,
                 &bytes_returned, NULL, NULL) == SOCKET_ERROR) {
        int error = WSAGetLastError();

        if (sender != INVALID_SOCKET && udp_connreset_unsupported(error)) {
            printf("UDP error readiness: SKIP (SIO_UDP_CONNRESET WSA=%d)\n",
                   error);
            result = TEST_SKIPPED;
        }
        goto cleanup;
    }
    if (connect(sender, (const struct sockaddr *)&address,
                address_length) == SOCKET_ERROR) {
        goto cleanup;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        ctl_socket(epfd, EPOLL_CTL_ADD, sender, EPOLLIN, data) != 0) {
        goto cleanup;
    }
    registered = 1;
    memset(&output, 0, sizeof(output));
    if (epoll_wait(epfd, &output, 1, 0) != 0 ||
        send(sender, "e", 1, 0) != 1) {
        goto cleanup;
    }

    memset(&output, 0, sizeof(output));
    wait_count = epoll_wait(epfd, &output, 1, 2000);
    if (wait_count == 0) {
        puts("UDP error readiness: SKIP (ICMP error suppressed)");
        result = TEST_SKIPPED;
        goto cleanup;
    }
    if (wait_count != 1 || output.data.u64 != data ||
        (output.events & EPOLLERR) == 0) {
        fprintf(stderr,
                "UDP error mismatch: count=%d errno=%d WSA=%d "
                "data=0x%llx events=0x%08lx\n",
                wait_count, errno, WSAGetLastError(),
                (unsigned long long)output.data.u64,
                (unsigned long)output.events);
        goto cleanup;
    }
    if (recv(sender, &byte, 1, 0) != SOCKET_ERROR ||
        WSAGetLastError() != WSAECONNRESET) {
        fprintf(stderr, "UDP error did not surface as WSAECONNRESET, WSA=%d\n",
                WSAGetLastError());
        goto cleanup;
    }
    result = TEST_OK;

cleanup:
    if (registered &&
        ctl_socket(epfd, EPOLL_CTL_DEL, sender, 0, 0) != 0) {
        result = TEST_FAILED;
    }
    if (epfd >= 0) {
        (void)wepoll_close(epfd);
    }
    if (sender != INVALID_SOCKET) {
        closesocket(sender);
    }
    if (reserved != INVALID_SOCKET) {
        closesocket(reserved);
    }
    if (result == TEST_OK) {
        puts("udp-error: OK");
    }
    return result;
}

static int run_mode(const char *mode)
{
    if (strcmp(mode, "aliases") == 0) {
        return test_aliases();
    }
    if (strcmp(mode, "oob-lt") == 0) {
        return test_oob_lt();
    }
    if (strcmp(mode, "oob-et") == 0) {
        return test_oob_et();
    }
    if (strcmp(mode, "oob-oneshot") == 0) {
        return test_oob_oneshot();
    }
    if (strcmp(mode, "oob-mod") == 0) {
        return test_oob_mod();
    }
    if (strcmp(mode, "udp-v4") == 0) {
        return test_udp_readiness(AF_INET);
    }
    if (strcmp(mode, "udp-v6") == 0) {
        return test_udp_readiness(AF_INET6);
    }
    if (strcmp(mode, "udp-error") == 0) {
        return test_udp_error();
    }
    fprintf(stderr,
            "usage: test_windows_socket_events "
            "[aliases|oob-lt|oob-et|oob-oneshot|oob-mod|"
            "udp-v4|udp-v6|udp-error]\n");
    return TEST_FAILED;
}

int main(int argc, char **argv)
{
    WSADATA wsa_data;
    int result;

    if (argc != 2) {
        return 2;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return 2;
    }
    result = run_mode(argv[1]);
    WSACleanup();
    if (result == TEST_SKIPPED) {
        return 77;
    }
    return result == TEST_OK ? 0 : 1;
}

#else

int main(void)
{
    return 0;
}

#endif
