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
    struct sockaddr_storage sender_address;
    int sender_address_length;
    int family;
} udp_fixture_t;

#define TEST_UDP_READ_IOCP_KEY  ((ULONG_PTR)UINT32_C(0x55445052))
#define TEST_UDP_ERROR_IOCP_KEY ((ULONG_PTR)UINT32_C(0x55445045))

enum {
    TEST_UDP_OVERSIZED_LENGTH = 257
};

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

static int associate_socket_iocp(SOCKET socket_fd,
                                 HANDLE completion_port,
                                 ULONG_PTR completion_key,
                                 const char *name)
{
    HANDLE associated;

    associated = CreateIoCompletionPort((HANDLE)(uintptr_t)socket_fd,
                                        completion_port,
                                        completion_key, 0);
    if (associated != completion_port) {
        fprintf(stderr, "%s: CreateIoCompletionPort failed, error=%lu\n",
                name, (unsigned long)GetLastError());
        return -1;
    }
    return 0;
}

static int wait_iocp_empty(HANDLE completion_port, DWORD timeout_ms,
                           const char *name)
{
    DWORD transferred = 0;
    ULONG_PTR completion_key = 0;
    OVERLAPPED *overlapped = NULL;
    BOOL dequeued;
    DWORD error;

    SetLastError(ERROR_SUCCESS);
    dequeued = GetQueuedCompletionStatus(completion_port, &transferred,
                                         &completion_key, &overlapped,
                                         timeout_ms);
    error = GetLastError();
    /* A failed completion has dequeued == FALSE but a non-NULL OVERLAPPED.
     * Only the NULL/WAIT_TIMEOUT combination proves that the port is empty. */
    if (!dequeued && overlapped == NULL && error == WAIT_TIMEOUT) {
        return 0;
    }
    fprintf(stderr,
            "%s: unexpected application IOCP packet: dequeued=%d "
            "error=%lu bytes=%lu key=0x%llx overlapped=%p\n",
            name, dequeued != FALSE, (unsigned long)error,
            (unsigned long)transferred,
            (unsigned long long)completion_key, (void *)overlapped);
    return -1;
}

static int prove_socket_iocp_send(SOCKET socket_fd,
                                  HANDLE completion_port,
                                  ULONG_PTR expected_key,
                                  const struct sockaddr *destination,
                                  int destination_length,
                                  const char *name)
{
    char byte = 'c';
    WSABUF buffer;
    OVERLAPPED overlapped;
    OVERLAPPED *completed = NULL;
    HANDLE event;
    DWORD transferred = 0;
    ULONG_PTR completion_key = 0;
    BOOL dequeued;
    DWORD error;
    int send_result;
    int started = 0;
    int result = -1;

    event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (event == NULL) {
        fprintf(stderr, "%s: CreateEvent failed, error=%lu\n",
                name, (unsigned long)GetLastError());
        return -1;
    }
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = event;
    buffer.buf = &byte;
    buffer.len = 1;
    if (destination == NULL) {
        send_result = WSASend(socket_fd, &buffer, 1, &transferred, 0,
                              &overlapped, NULL);
    } else {
        send_result = WSASendTo(socket_fd, &buffer, 1, &transferred, 0,
                                destination, destination_length,
                                &overlapped, NULL);
    }
    if (send_result == 0) {
        started = 1;
    } else if (WSAGetLastError() == WSA_IO_PENDING) {
        started = 1;
    } else {
        fprintf(stderr, "%s: WSASend failed, WSA=%d\n",
                name, WSAGetLastError());
        goto cleanup;
    }

    SetLastError(ERROR_SUCCESS);
    dequeued = GetQueuedCompletionStatus(completion_port, &transferred,
                                         &completion_key, &completed, 2000);
    error = GetLastError();
    if (!dequeued || completed != &overlapped ||
        completion_key != expected_key || transferred != 1) {
        fprintf(stderr,
                "%s: control packet mismatch: dequeued=%d error=%lu "
                "bytes=%lu key=0x%llx overlapped=%p expected=%p\n",
                name, dequeued != FALSE, (unsigned long)error,
                (unsigned long)transferred,
                (unsigned long long)completion_key, (void *)completed,
                (void *)&overlapped);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (result != 0 && started && completed != &overlapped) {
        (void)CancelIoEx((HANDLE)(uintptr_t)socket_fd, &overlapped);
        (void)WaitForSingleObject(event, INFINITE);
    }
    if (!CloseHandle(event)) {
        result = -1;
    }
    return result;
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

static int enable_oob_inline(SOCKET socket_fd)
{
    int enabled = 1;

    return setsockopt(socket_fd, SOL_SOCKET, SO_OOBINLINE,
                      (const char *)&enabled,
                      (int)sizeof(enabled)) == SOCKET_ERROR ? -1 : 0;
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
    if ((expected != 0 &&
         wait_exact(epfd, 2000, data, expected, name) != 0) ||
        (expected == 0 && wait_empty(epfd, 100, name) != 0)) {
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

static int test_write_band_mod(void)
{
    tcp_pair_t pair;
    const uint64_t band_data = UINT64_C(0x4106);
    const uint64_t out_data = UINT64_C(0x4107);
    int epfd = -1;
    int registered = 0;
    int result = -1;

    if (make_tcp_pair(&pair) != 0) {
        return -1;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        ctl_socket(epfd, EPOLL_CTL_ADD, pair.client,
                   EPOLLWRBAND, band_data) != 0) {
        goto cleanup;
    }
    registered = 1;
    if (wait_empty(epfd, 100, "WRBAND MOD no readiness") != 0 ||
        ctl_socket(epfd, EPOLL_CTL_MOD, pair.client,
                   EPOLLOUT, out_data) != 0 ||
        wait_exact(epfd, 2000, out_data, EPOLLOUT,
                   "WRBAND MOD to OUT") != 0) {
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
        run_write_alias_case(EPOLLWRBAND, 0,
                             UINT64_C(0x4104), "WRBAND") != 0 ||
        run_write_alias_case(EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND,
                             EPOLLOUT | EPOLLWRNORM,
                             UINT64_C(0x4105), "write aliases") != 0 ||
        test_write_band_mod() != 0) {
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
        send_oob(pair.client, '!') != 0) {
        goto cleanup;
    }
    if ((expected != 0 &&
         (wait_exact(epfd, 2000, data, expected, name) != 0 ||
          wait_exact(epfd, 2000, data, expected, name) != 0)) ||
        (expected == 0 && wait_empty(epfd, 100, name) != 0) ||
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
        run_oob_lt_case(EPOLLRDBAND, 0, UINT64_C(0x4202),
                        "RDBAND LT") != 0 ||
        run_oob_lt_case(EPOLLPRI | EPOLLRDBAND, EPOLLPRI,
                        UINT64_C(0x4204), "PRI+RDBAND LT") != 0 ||
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
    const uint64_t band_data = UINT64_C(0x4500);
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
                   EPOLLRDBAND, band_data) != 0) {
        goto cleanup;
    }
    registered = 1;
    if (send_oob(pair.client, 'b') != 0 ||
        wait_empty(epfd, 100, "MOD RDBAND no readiness") != 0 ||
        ctl_socket(epfd, EPOLL_CTL_MOD, pair.server,
                   EPOLLPRI, pri_data) != 0 ||
        wait_exact(epfd, 2000, pri_data, EPOLLPRI,
                   "MOD RDBAND to PRI") != 0 ||
        recv_oob(pair.server, 'b') != 0 ||
        ctl_socket(epfd, EPOLL_CTL_MOD, pair.server,
                   EPOLLIN, in_data) != 0 ||
        send_normal(pair.client, 'm') != 0 ||
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

static int run_oob_inline_interest_case(uint32_t interest,
                                        uint32_t expected_events,
                                        uint64_t data,
                                        const char *name)
{
    tcp_pair_t pair;
    int epfd = -1;
    int registered = 0;
    int result = -1;

    if (make_tcp_pair(&pair) != 0) {
        return -1;
    }
    if (enable_oob_inline(pair.server) != 0) {
        goto cleanup;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        ctl_socket(epfd, EPOLL_CTL_ADD, pair.server,
                   interest, data) != 0) {
        goto cleanup;
    }
    registered = 1;
    if (send_oob(pair.client, 'q') != 0) {
        goto cleanup;
    }
    if (expected_events != 0) {
        if (wait_exact(epfd, 2000, data, expected_events, name) != 0) {
            goto cleanup;
        }
    } else if (wait_empty(epfd, 100, name) != 0) {
        goto cleanup;
    }
    if (recv_normal(pair.server, 'q') != 0 ||
        wait_empty(epfd, 100, "OOBINLINE interest drained") != 0) {
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

static int test_oob_inline_lt(void)
{
    tcp_pair_t pair;
    const uint64_t data = UINT64_C(0x4551);
    int epfd = -1;
    int registered = 0;
    int result = -1;

    if (run_oob_inline_interest_case(
            EPOLLIN | EPOLLPRI, EPOLLIN, UINT64_C(0x4550),
            "OOBINLINE combined interest") != 0 ||
        run_oob_inline_interest_case(
            EPOLLPRI, 0, UINT64_C(0x4553),
            "OOBINLINE PRI-only interest") != 0 ||
        make_tcp_pair(&pair) != 0) {
        return -1;
    }
    if (enable_oob_inline(pair.server) != 0) {
        goto cleanup;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        ctl_socket(epfd, EPOLL_CTL_ADD, pair.server,
                   EPOLLIN, data) != 0) {
        goto cleanup;
    }
    registered = 1;
    if (send_oob(pair.client, 'i') != 0 ||
        wait_exact(epfd, 2000, data, EPOLLIN,
                   "OOBINLINE LT first") != 0 ||
        wait_exact(epfd, 2000, data, EPOLLIN,
                   "OOBINLINE LT persistent") != 0 ||
        recv_normal(pair.server, 'i') != 0 ||
        wait_empty(epfd, 100, "OOBINLINE LT drained") != 0) {
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
        puts("oob-inline-lt: OK");
    }
    return result;
}

static int test_oob_inline_et(void)
{
    tcp_pair_t pair;
    const uint64_t data = UINT64_C(0x4552);
    int epfd = -1;
    int registered = 0;
    int result = -1;

    if (make_tcp_pair(&pair) != 0) {
        return -1;
    }
    if (enable_oob_inline(pair.server) != 0) {
        goto cleanup;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        ctl_socket(epfd, EPOLL_CTL_ADD, pair.server,
                   EPOLLIN | EPOLLET, data) != 0) {
        goto cleanup;
    }
    registered = 1;
    if (send_oob(pair.client, 'a') != 0 ||
        wait_exact(epfd, 2000, data, EPOLLIN,
                   "OOBINLINE ET first") != 0 ||
        wait_empty(epfd, 100, "OOBINLINE ET duplicate") != 0 ||
        recv_normal(pair.server, 'a') != 0 ||
        wait_empty(epfd, 100, "OOBINLINE ET gap") != 0 ||
        send_oob(pair.client, 'b') != 0 ||
        wait_exact(epfd, 2000, data, EPOLLIN,
                   "OOBINLINE ET re-edge") != 0 ||
        recv_normal(pair.server, 'b') != 0 ||
        wait_empty(epfd, 100, "OOBINLINE ET drained") != 0) {
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
        puts("oob-inline-et: OK");
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
           error == WSAEADDRNOTAVAIL || error == WSAENETUNREACH ||
           error == WSAEINVAL;
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
    memcpy(&fixture->sender_address, &fixture->address,
           sizeof(fixture->sender_address));
    if (family == AF_INET) {
        ((struct sockaddr_in *)&fixture->sender_address)->sin_port =
            htons(0);
        address_length = (int)sizeof(struct sockaddr_in);
    } else {
        ((struct sockaddr_in6 *)&fixture->sender_address)->sin6_port =
            htons(0);
        address_length = (int)sizeof(struct sockaddr_in6);
    }
    if (bind(fixture->sender,
             (const struct sockaddr *)&fixture->sender_address,
             address_length) == SOCKET_ERROR ||
        getsockname(fixture->sender,
                    (struct sockaddr *)&fixture->sender_address,
                    &address_length) == SOCKET_ERROR) {
        int error = WSAGetLastError();

        udp_fixture_close(fixture);
        return family == AF_INET6 && ipv6_unavailable_error(error)
            ? TEST_SKIPPED : TEST_FAILED;
    }
    fixture->sender_address_length = address_length;
    return TEST_OK;
}

static int queue_udp_datagram(const udp_fixture_t *fixture,
                              const char *payload,
                              int payload_length,
                              const char *name)
{
    int sent;

    sent = sendto(fixture->sender, payload, payload_length, 0,
                  (const struct sockaddr *)&fixture->address,
                  fixture->address_length);
    if (sent != payload_length) {
        fprintf(stderr, "%s: sendto=%d expected=%d WSA=%d\n",
                name, sent, payload_length, WSAGetLastError());
        return -1;
    }
    return 0;
}

static int expect_udp_datagram(const udp_fixture_t *fixture,
                               int epfd,
                               uint64_t data,
                               const char *payload,
                               int payload_length,
                               HANDLE application_iocp,
                               const char *name)
{
    char received[TEST_UDP_OVERSIZED_LENGTH + 1];
    int received_length;

    if (payload_length < 0 ||
        payload_length > (int)sizeof(received)) {
        fprintf(stderr, "%s: invalid payload length %d\n",
                name, payload_length);
        return -1;
    }
    if (wait_exact(epfd, 2000, data, EPOLLIN, name) != 0 ||
        wait_exact(epfd, 2000, data, EPOLLIN, name) != 0 ||
        (application_iocp != NULL &&
         wait_iocp_empty(application_iocp, 250, name) != 0)) {
        return -1;
    }
    memset(received, 0, sizeof(received));
    received_length = recv(fixture->receiver, received,
                           (int)sizeof(received), 0);
    if (received_length != payload_length ||
        memcmp(received, payload, (size_t)payload_length) != 0) {
        fprintf(stderr, "%s: recv=%d expected=%d WSA=%d\n",
                name, received_length, payload_length, WSAGetLastError());
        return -1;
    }
    return wait_empty(epfd, 100, name);
}

static int exercise_udp_datagram(const udp_fixture_t *fixture,
                                 int epfd,
                                 uint64_t data,
                                 const char *payload,
                                 int payload_length,
                                 HANDLE application_iocp,
                                 const char *name)
{
    if (queue_udp_datagram(fixture, payload, payload_length, name) != 0) {
        return -1;
    }
    return expect_udp_datagram(fixture, epfd, data, payload,
                               payload_length, application_iocp, name);
}

static int test_udp_readiness(int family, HANDLE application_iocp)
{
    udp_fixture_t fixture;
    const uint64_t data = family == AF_INET
        ? UINT64_C(0x4604) : UINT64_C(0x4606);
    const char normal_payload[] = {
        'u', 'd', 'p', family == AF_INET ? '4' : '6'
    };
    char zero_payload = 0;
    char oversized_payload[TEST_UDP_OVERSIZED_LENGTH];
    int index;
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
    if (application_iocp != NULL &&
        associate_socket_iocp(fixture.receiver, application_iocp,
                              TEST_UDP_READ_IOCP_KEY,
                              "UDP receiver IOCP association") != 0) {
        goto cleanup;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        ctl_socket(epfd, EPOLL_CTL_ADD, fixture.receiver,
                   EPOLLIN, data) != 0) {
        goto cleanup;
    }
    registered = 1;
    if (queue_udp_datagram(&fixture, normal_payload,
                           (int)sizeof(normal_payload),
                           "UDP normal datagram") != 0) {
        goto cleanup;
    }
    if (application_iocp != NULL &&
        prove_socket_iocp_send(
            fixture.receiver, application_iocp,
            TEST_UDP_READ_IOCP_KEY,
            (const struct sockaddr *)&fixture.sender_address,
            fixture.sender_address_length,
            "UDP application IOCP control") != 0) {
        goto cleanup;
    }
    if (expect_udp_datagram(&fixture, epfd, data, normal_payload,
                            (int)sizeof(normal_payload), application_iocp,
                            "UDP normal datagram") != 0 ||
        exercise_udp_datagram(&fixture, epfd, data, &zero_payload, 0,
                              application_iocp,
                              "UDP zero-length datagram") != 0) {
        goto cleanup;
    }
    for (index = 0; index < (int)sizeof(oversized_payload); index++) {
        oversized_payload[index] = (char)(index ^ 0x5a);
    }
    if (exercise_udp_datagram(
            &fixture, epfd, data, oversized_payload,
            (int)sizeof(oversized_payload), application_iocp,
            "UDP oversized datagram") != 0) {
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

static int test_udp_error(int family, HANDLE application_iocp,
                          uint32_t modifiers, const char *mode)
{
    struct sockaddr_storage address;
    SOCKET reserved = INVALID_SOCKET;
    SOCKET sender = INVALID_SOCKET;
    struct epoll_event output;
    const uint64_t data = family == AF_INET
        ? UINT64_C(0x46455234) : UINT64_C(0x46455236);
    const uint64_t rearm_data = data + UINT64_C(1);
    const uint64_t drained_data = data + UINT64_C(2);
    BOOL enabled = TRUE;
    DWORD bytes_returned = 0;
    u_long nonblocking = 1;
    char byte;
    int address_length;
    int epfd = -1;
    int error;
    int registered = 0;
    int result = TEST_FAILED;
    int recv_result;
    int wait_count;

    memset(&address, 0, sizeof(address));
    if (family == AF_INET) {
        struct sockaddr_in *address_v4 = (struct sockaddr_in *)&address;

        address_v4->sin_family = AF_INET;
        address_v4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address_v4->sin_port = htons(0);
        address_length = (int)sizeof(*address_v4);
    } else {
        struct sockaddr_in6 *address_v6 = (struct sockaddr_in6 *)&address;

        address_v6->sin6_family = AF_INET6;
        address_v6->sin6_addr = in6addr_loopback;
        address_v6->sin6_port = htons(0);
        address_length = (int)sizeof(*address_v6);
    }
    reserved = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (reserved == INVALID_SOCKET ||
        bind(reserved, (const struct sockaddr *)&address,
             address_length) == SOCKET_ERROR ||
        getsockname(reserved, (struct sockaddr *)&address,
                    &address_length) == SOCKET_ERROR) {
        error = WSAGetLastError();
        if (family == AF_INET6 && ipv6_unavailable_error(error)) {
            printf("UDP IPv6 error readiness: SKIP "
                   "(address family unavailable, WSA=%d)\n", error);
            result = TEST_SKIPPED;
        }
        goto cleanup;
    }
    closesocket(reserved);
    reserved = INVALID_SOCKET;

    sender = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (sender == INVALID_SOCKET ||
        ioctlsocket(sender, FIONBIO, &nonblocking) == SOCKET_ERROR ||
        WSAIoctl(sender, SIO_UDP_CONNRESET,
                 &enabled, (DWORD)sizeof(enabled), NULL, 0,
                 &bytes_returned, NULL, NULL) == SOCKET_ERROR) {
        error = WSAGetLastError();

        if (sender != INVALID_SOCKET && udp_connreset_unsupported(error)) {
            printf("UDP IPv%d error readiness: SKIP "
                   "(SIO_UDP_CONNRESET WSA=%d)\n",
                   family == AF_INET ? 4 : 6, error);
            result = TEST_SKIPPED;
        } else if (family == AF_INET6 && ipv6_unavailable_error(error)) {
            printf("UDP IPv6 error readiness: SKIP "
                   "(address family unavailable, WSA=%d)\n", error);
            result = TEST_SKIPPED;
        }
        goto cleanup;
    }
    if (connect(sender, (const struct sockaddr *)&address,
                address_length) == SOCKET_ERROR) {
        error = WSAGetLastError();
        if (family == AF_INET6 && ipv6_unavailable_error(error)) {
            printf("UDP IPv6 error readiness: SKIP "
                   "(address family unavailable, WSA=%d)\n", error);
            result = TEST_SKIPPED;
        }
        goto cleanup;
    }
    if (application_iocp != NULL &&
        associate_socket_iocp(sender, application_iocp,
                              TEST_UDP_ERROR_IOCP_KEY,
                              "UDP error socket IOCP association") != 0) {
        goto cleanup;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        ctl_socket(epfd, EPOLL_CTL_ADD, sender,
                   EPOLLIN | modifiers, data) != 0) {
        goto cleanup;
    }
    registered = 1;
    memset(&output, 0, sizeof(output));
    if (epoll_wait(epfd, &output, 1, 0) != 0) {
        goto cleanup;
    }
    if (application_iocp != NULL) {
        if (prove_socket_iocp_send(sender, application_iocp,
                                   TEST_UDP_ERROR_IOCP_KEY,
                                   NULL, 0,
                                   "UDP error application IOCP control") !=
            0) {
            goto cleanup;
        }
    } else if (send(sender, "e", 1, 0) != 1) {
        goto cleanup;
    }

    memset(&output, 0, sizeof(output));
    wait_count = epoll_wait(epfd, &output, 1, 2000);
    if (wait_count == 0) {
        recv_result = recv(sender, &byte, 1, MSG_PEEK);
        error = recv_result == SOCKET_ERROR ? WSAGetLastError() : 0;
        if (recv_result == SOCKET_ERROR && error == WSAEWOULDBLOCK) {
            printf("UDP IPv%d error readiness: SKIP "
                   "(ICMP error suppressed)\n",
                   family == AF_INET ? 4 : 6);
            result = TEST_SKIPPED;
            goto cleanup;
        }
        if (recv_result != SOCKET_ERROR || error != WSAECONNRESET) {
            fprintf(stderr,
                    "UDP IPv%d error timeout probe mismatch: recv=%d "
                    "WSA=%d\n", family == AF_INET ? 4 : 6,
                    recv_result, error);
            goto cleanup;
        }
        memset(&output, 0, sizeof(output));
        wait_count = epoll_wait(epfd, &output, 1, 250);
        if (wait_count == 0) {
            fprintf(stderr,
                    "UDP IPv%d error readiness missed queued "
                    "WSAECONNRESET after grace wait\n",
                    family == AF_INET ? 4 : 6);
            goto cleanup;
        }
    }
    if (wait_count != 1 || output.data.u64 != data ||
        output.events != EPOLLERR) {
        fprintf(stderr,
                "UDP IPv%d error mismatch: count=%d errno=%d WSA=%d "
                "data=0x%llx events=0x%08lx\n",
                family == AF_INET ? 4 : 6,
                wait_count, errno, WSAGetLastError(),
                (unsigned long long)output.data.u64,
                (unsigned long)output.events);
        goto cleanup;
    }
    if (modifiers == 0) {
        if (wait_exact(epfd, 2000, data, EPOLLERR,
                       "UDP error LT repeat") != 0) {
            goto cleanup;
        }
    } else if (modifiers == EPOLLET) {
        if (wait_empty(epfd, 100, "UDP error ET duplicate") != 0) {
            goto cleanup;
        }
    } else if (modifiers == EPOLLONESHOT) {
        if (wait_empty(epfd, 100, "UDP error ONESHOT disabled") != 0 ||
            ctl_socket(epfd, EPOLL_CTL_MOD, sender,
                       EPOLLIN | EPOLLONESHOT, rearm_data) != 0 ||
            wait_exact(epfd, 2000, rearm_data, EPOLLERR,
                       "UDP error ONESHOT MOD rearm") != 0 ||
            wait_empty(epfd, 100,
                       "UDP error ONESHOT re-disabled") != 0) {
            goto cleanup;
        }
    } else {
        fprintf(stderr, "%s: unsupported modifiers 0x%08lx\n",
                mode, (unsigned long)modifiers);
        goto cleanup;
    }
    if (application_iocp != NULL &&
        wait_iocp_empty(application_iocp, 250,
                        "UDP error qualifier isolation") != 0) {
        goto cleanup;
    }
    recv_result = recv(sender, &byte, 1, 0);
    error = recv_result == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (recv_result != SOCKET_ERROR || error != WSAECONNRESET) {
        fprintf(stderr,
                "UDP IPv%d error did not surface as WSAECONNRESET: "
                "recv=%d WSA=%d\n",
                family == AF_INET ? 4 : 6, recv_result, error);
        goto cleanup;
    }
    if (modifiers == EPOLLET) {
        if (wait_empty(epfd, 100, "UDP error ET gap") != 0 ||
            send(sender, "r", 1, 0) != 1 ||
            wait_exact(epfd, 2000, data, EPOLLERR,
                       "UDP error ET re-edge") != 0 ||
            wait_empty(epfd, 100,
                       "UDP error ET re-edge duplicate") != 0) {
            goto cleanup;
        }
        recv_result = recv(sender, &byte, 1, 0);
        error = recv_result == SOCKET_ERROR ? WSAGetLastError() : 0;
        if (recv_result != SOCKET_ERROR || error != WSAECONNRESET) {
            fprintf(stderr,
                    "UDP IPv%d ET re-edge did not preserve "
                    "WSAECONNRESET: recv=%d WSA=%d\n",
                    family == AF_INET ? 4 : 6, recv_result, error);
            goto cleanup;
        }
    }
    if (modifiers == EPOLLONESHOT &&
        ctl_socket(epfd, EPOLL_CTL_MOD, sender,
                   EPOLLIN | EPOLLONESHOT, drained_data) != 0) {
        goto cleanup;
    }
    if (wait_empty(epfd, 100, "UDP error drained") != 0) {
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
        printf("%s: OK\n", mode);
    }
    return result;
}

static int test_udp_iocp_isolation(void)
{
    HANDLE completion_port;
    int error_result;
    int result;

    completion_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL,
                                             0, 1);
    if (completion_port == NULL) {
        fprintf(stderr, "UDP IOCP isolation: create failed, error=%lu\n",
                (unsigned long)GetLastError());
        return TEST_FAILED;
    }
    result = test_udp_readiness(AF_INET, completion_port);
    if (result == TEST_OK) {
        error_result = test_udp_error(AF_INET6, completion_port, 0,
                                      "udp-error-v6");
        if (error_result == TEST_FAILED) {
            result = TEST_FAILED;
        } else if (error_result == TEST_SKIPPED) {
            puts("UDP IOCP error qualifier: SKIP "
                 "(ICMP error behavior unavailable)");
        }
    }
    if (result == TEST_OK &&
        wait_iocp_empty(completion_port, 100,
                        "UDP qualifier final isolation") != 0) {
        result = TEST_FAILED;
    }
    if (!CloseHandle(completion_port)) {
        result = TEST_FAILED;
    }
    if (result == TEST_OK) {
        puts("udp-iocp: OK");
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
    if (strcmp(mode, "oob-inline-lt") == 0) {
        return test_oob_inline_lt();
    }
    if (strcmp(mode, "oob-inline-et") == 0) {
        return test_oob_inline_et();
    }
    if (strcmp(mode, "udp-v4") == 0) {
        return test_udp_readiness(AF_INET, NULL);
    }
    if (strcmp(mode, "udp-v6") == 0) {
        return test_udp_readiness(AF_INET6, NULL);
    }
    if (strcmp(mode, "udp-error") == 0) {
        return test_udp_error(AF_INET, NULL, 0, mode);
    }
    if (strcmp(mode, "udp-error-v6") == 0) {
        return test_udp_error(AF_INET6, NULL, 0, mode);
    }
    if (strcmp(mode, "udp-error-v6-et") == 0) {
        return test_udp_error(AF_INET6, NULL, EPOLLET, mode);
    }
    if (strcmp(mode, "udp-error-v6-oneshot") == 0) {
        return test_udp_error(AF_INET6, NULL, EPOLLONESHOT, mode);
    }
    if (strcmp(mode, "udp-iocp") == 0) {
        return test_udp_iocp_isolation();
    }
    fprintf(stderr,
            "usage: test_windows_socket_events "
            "[aliases|oob-lt|oob-et|oob-oneshot|oob-mod|"
            "oob-inline-lt|oob-inline-et|udp-v4|udp-v6|udp-error|"
            "udp-error-v6|udp-error-v6-et|udp-error-v6-oneshot|"
            "udp-iocp]\n");
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
