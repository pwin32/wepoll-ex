/*
 * test_windows_api.c -- runtime coverage for the native Windows API.
 *
 * The Windows implementation has no socketpair(2), so the tests use a
 * loopback TCP connection for ADD/WAIT/DEL and context/oneshot coverage.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct tcp_pair {
    SOCKET listener;
    SOCKET client;
    SOCKET server;
} tcp_pair_t;

static int tests_passed;
static int tests_failed;

#define TEST(name) do { \
    printf("  [test] %s ", name); \
    fflush(stdout); \
} while (0)

#define PASS() do { \
    puts("OK"); \
    tests_passed++; \
} while (0)

#define FAIL(reason) do { \
    printf("FAIL: %s (errno=%d)\n", reason, errno); \
    tests_failed++; \
} while (0)

static void tcp_pair_init(tcp_pair_t *pair)
{
    pair->listener = INVALID_SOCKET;
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
    if (pair->listener != INVALID_SOCKET) {
        closesocket(pair->listener);
        pair->listener = INVALID_SOCKET;
    }
}

static int make_tcp_pair(tcp_pair_t *pair)
{
    struct sockaddr_in address;
    int address_length = (int)sizeof(address);
    u_long nonblocking = 1;

    tcp_pair_init(pair);
    pair->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pair->listener == INVALID_SOCKET) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (bind(pair->listener, (const struct sockaddr *)&address,
             (int)sizeof(address)) == SOCKET_ERROR ||
        listen(pair->listener, 1) == SOCKET_ERROR ||
        getsockname(pair->listener, (struct sockaddr *)&address,
                    &address_length) == SOCKET_ERROR) {
        tcp_pair_close(pair);
        return -1;
    }

    pair->client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pair->client == INVALID_SOCKET ||
        ioctlsocket(pair->client, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        tcp_pair_close(pair);
        return -1;
    }
    if (connect(pair->client, (const struct sockaddr *)&address,
                (int)sizeof(address)) == SOCKET_ERROR) {
        int connect_error = WSAGetLastError();

        /* A nonblocking connect commonly reports WSAEWOULDBLOCK.  The
         * local listener still accepts it, so only hard errors fail setup. */
        if (connect_error != WSAEWOULDBLOCK &&
            connect_error != WSAEINPROGRESS &&
            connect_error != WSAEALREADY) {
            tcp_pair_close(pair);
            return -1;
        }
    }

    pair->server = accept(pair->listener, NULL, NULL);
    if (pair->server == INVALID_SOCKET ||
        ioctlsocket(pair->server, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        tcp_pair_close(pair);
        return -1;
    }
    return 0;
}

static int send_byte(SOCKET client)
{
    const char byte = 'x';
    return send(client, &byte, 1, 0) == 1 ? 0 : -1;
}

static int recv_byte(SOCKET server)
{
    char byte;
    return recv(server, &byte, 1, 0) == 1 ? 0 : -1;
}

static void test_create_close(void)
{
    int epfd;

    TEST("create and close epoll instances");
    epfd = epoll_create1(0);
    if (epfd < 0 || wepoll_close(epfd) != 0) {
        FAIL("create/close");
        return;
    }
    errno = 0;
    if (wepoll_close(epfd) != -1 || errno != EBADF) {
        FAIL("double close should be EBADF");
        return;
    }
    PASS();
}

static void test_operational_stats(void)
{
    wepoll_ex_global_stats global_stats;
    wepoll_ex_stats stats;
    uint32_t prefix[2] = { 0, 0 };
    int policy;
    int epfd;

    TEST("versioned operational statistics and lifetime policy");
    epfd = epoll_create_ex(8, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        return;
    }
    policy = wepoll_ex_get_socket_lifetime_policy();
    if (policy < WEPOLL_EX_SOCKET_LIFETIME_BEST_EFFORT ||
        policy > WEPOLL_EX_SOCKET_LIFETIME_SYNCHRONIZED ||
        wepoll_ex_get_stats(epfd, &stats, sizeof(stats)) != 0 ||
        stats.version != WEPOLL_EX_STATS_VERSION ||
        stats.struct_size != sizeof(stats) ||
        stats.socket_lifetime_policy != (uint32_t)policy ||
        stats.active_registrations != 0 ||
        stats.rearm_queue_depth != 0 || stats.ready_queue_depth != 0 ||
        wepoll_ex_get_global_stats(&global_stats, sizeof(global_stats)) != 0 ||
        global_stats.version != WEPOLL_EX_STATS_VERSION ||
        global_stats.struct_size != sizeof(global_stats) ||
        wepoll_ex_get_stats(epfd, (wepoll_ex_stats *)prefix,
                            sizeof(prefix)) != 0 ||
        prefix[0] != WEPOLL_EX_STATS_VERSION ||
        prefix[1] != sizeof(stats)) {
        FAIL("statistics snapshot");
        (void)wepoll_close(epfd);
        return;
    }
    errno = 0;
    if (wepoll_ex_get_stats(epfd, &stats, sizeof(uint32_t)) != -1 ||
        errno != EINVAL) {
        FAIL("statistics size validation");
        (void)wepoll_close(epfd);
        return;
    }
    if (wepoll_close(epfd) != 0) {
        FAIL("wepoll_close");
        return;
    }
    PASS();
}

static void test_invalid_args(void)
{
    struct epoll_event event;
    struct epoll_event output[1];
    struct timespec invalid_time;
    int epfd;

    TEST("invalid create, wait, ctl, and batch arguments");
    errno = 0;
    if (epoll_create(0) != -1 || errno != EINVAL) {
        FAIL("epoll_create(0)");
        return;
    }
    errno = 0;
    if (epoll_create1(0x4000) != -1 || errno != EINVAL) {
        FAIL("epoll_create1 flags");
        return;
    }
    errno = 0;
    if (epoll_create_ex(-1, 0) != -1 || errno != EINVAL) {
        FAIL("epoll_create_ex size");
        return;
    }

    memset(&event, 0, sizeof(event));
    errno = 0;
    if (epoll_wait(-1, output, 1, 0) != -1 || errno != EBADF) {
        FAIL("bad epfd wait");
        return;
    }

    epfd = epoll_create1(0);
    if (epfd < 0) {
        FAIL("epoll_create1");
        return;
    }
    errno = 0;
    if (epoll_wait(epfd, NULL, 1, 0) != -1 || errno != EFAULT) {
        FAIL("NULL wait events");
        wepoll_close(epfd);
        return;
    }
    errno = 0;
    if (epoll_wait(epfd, output, 0, 0) != -1 || errno != EINVAL) {
        FAIL("zero maxevents");
        wepoll_close(epfd);
        return;
    }
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, EPOLL_FD_INVALID,
                  &event) != -1 || errno != EBADF) {
        FAIL("invalid socket fd");
        wepoll_close(epfd);
        return;
    }
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)1,
                  NULL) != -1 || errno != EFAULT) {
        FAIL("NULL ADD event");
        wepoll_close(epfd);
        return;
    }
    errno = 0;
    if (epoll_ctl(epfd, 99, (epoll_fd_t)1,
                  &event) != -1 || errno != EINVAL) {
        FAIL("invalid ctl operation");
        wepoll_close(epfd);
        return;
    }

    invalid_time.tv_sec = 0;
    invalid_time.tv_nsec = 1000000000L;
    errno = 0;
    if (epoll_pwait2(epfd, output, 1, &invalid_time, NULL) != -1 ||
        errno != EINVAL) {
        FAIL("invalid timespec");
        wepoll_close(epfd);
        return;
    }
    errno = 0;
    if (epoll_ctl_batch(epfd, NULL, NULL, NULL, 1) != -1 ||
        errno != EFAULT) {
        FAIL("NULL batch arrays");
        wepoll_close(epfd);
        return;
    }
    wepoll_close(epfd);
    PASS();
}

static void test_basic_io(void)
{
    tcp_pair_t pair;
    struct epoll_event event;
    struct epoll_event output;
    int epfd = -1;
    int count;

    TEST("loopback ADD/send/WAIT/DEL cycle");
    if (make_tcp_pair(&pair) != 0) {
        FAIL("make_tcp_pair");
        return;
    }
    epfd = epoll_create1(0);
    if (epfd < 0) {
        FAIL("epoll_create1");
        tcp_pair_close(&pair);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = UINT64_C(0x12345678);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair.server, &event) != 0 ||
        send_byte(pair.client) != 0) {
        FAIL("ADD/send");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    count = epoll_wait(epfd, &output, 1, 1000);
    if (count != 1 || (output.events & EPOLLIN) == 0 ||
        output.data.u64 != event.data.u64 || recv_byte(pair.server) != 0) {
        FAIL("WAIT event");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, pair.server, NULL) != 0 ||
        epoll_wait(epfd, &output, 1, 0) != 0) {
        FAIL("DEL/zero wait");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    wepoll_close(epfd);
    tcp_pair_close(&pair);
    PASS();
}

static void test_write_ready(void)
{
    tcp_pair_t pair;
    struct epoll_event event;
    struct epoll_event output;
    int epfd;

    TEST("loopback write readiness delivery");
    if (make_tcp_pair(&pair) != 0) {
        FAIL("make_tcp_pair");
        return;
    }
    epfd = epoll_create1(0);
    if (epfd < 0) {
        FAIL("epoll_create1");
        tcp_pair_close(&pair);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLOUT;
    event.data.u64 = UINT64_C(0xfeedbeef);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair.client, &event) != 0 ||
        epoll_wait(epfd, &output, 1, 1000) != 1 ||
        (output.events & EPOLLOUT) == 0 ||
        output.data.u64 != event.data.u64 || send_byte(pair.client) != 0 ||
        epoll_ctl(epfd, EPOLL_CTL_DEL, pair.client, NULL) != 0) {
        FAIL("write readiness");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    event.events = EPOLLIN;
    event.data.u64 = UINT64_C(0xfacefeed);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair.server, &event) != 0 ||
        epoll_wait(epfd, &output, 1, 1000) != 1 ||
        (output.events & EPOLLIN) == 0 ||
        output.data.u64 != event.data.u64 || recv_byte(pair.server) != 0 ||
        epoll_ctl(epfd, EPOLL_CTL_DEL, pair.server, NULL) != 0) {
        FAIL("write transfer");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    wepoll_close(epfd);
    tcp_pair_close(&pair);
    PASS();
}

static void test_remote_half_close(void)
{
    tcp_pair_t pair;
    struct epoll_event event;
    struct epoll_event output;
    char byte;
    int epfd;

    TEST("remote half-close reports IN and RDHUP");
    if (make_tcp_pair(&pair) != 0) {
        FAIL("make_tcp_pair");
        return;
    }
    epfd = epoll_create1(0);
    if (epfd < 0) {
        FAIL("epoll_create1");
        tcp_pair_close(&pair);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLRDHUP;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair.server, &event) != 0 ||
        shutdown(pair.client, SD_SEND) == SOCKET_ERROR ||
        epoll_wait(epfd, &output, 1, 1000) != 1 ||
        (output.events & (EPOLLIN | EPOLLRDHUP)) !=
            (EPOLLIN | EPOLLRDHUP) ||
        recv(pair.server, &byte, 1, 0) != 0 ||
        epoll_ctl(epfd, EPOLL_CTL_DEL, pair.server, NULL) != 0) {
        FAIL("remote half-close");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    wepoll_close(epfd);
    tcp_pair_close(&pair);
    PASS();
}

static void test_remote_reset(void)
{
    tcp_pair_t pair;
    struct epoll_event event;
    struct epoll_event output;
    struct linger reset = { 1, 0 };
    char byte;
    int epfd;

    TEST("remote reset reports unrequested ERR and HUP");
    if (make_tcp_pair(&pair) != 0) {
        FAIL("make_tcp_pair");
        return;
    }
    epfd = epoll_create1(0);
    if (epfd < 0) {
        FAIL("epoll_create1");
        tcp_pair_close(&pair);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair.server, &event) != 0 ||
        setsockopt(pair.client, SOL_SOCKET, SO_LINGER,
                   (const char *)&reset, (int)sizeof(reset)) == SOCKET_ERROR) {
        FAIL("reset setup");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    closesocket(pair.client);
    pair.client = INVALID_SOCKET;
    if (epoll_wait(epfd, &output, 1, 1000) != 1 ||
        (output.events & (EPOLLERR | EPOLLHUP)) !=
            (EPOLLERR | EPOLLHUP) ||
        recv(pair.server, &byte, 1, 0) != SOCKET_ERROR ||
        WSAGetLastError() != WSAECONNRESET ||
        epoll_ctl(epfd, EPOLL_CTL_DEL, pair.server, NULL) != 0) {
        FAIL("reset delivery");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    wepoll_close(epfd);
    tcp_pair_close(&pair);
    PASS();
}

static void test_connect_failure(void)
{
    struct sockaddr_in address;
    int address_length = (int)sizeof(address);
    struct epoll_event event;
    struct epoll_event output;
    SOCKET reserved = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    u_long nonblocking = 1;
    int socket_error = 0;
    int socket_error_length = (int)sizeof(socket_error);
    int immediate_refusal = 0;
    int add_result;
    int epfd = -1;

    TEST("refused connect reports OUT, ERR, and HUP or strict rejection");
    reserved = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (reserved == INVALID_SOCKET) {
        FAIL("reserved socket");
        return;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (bind(reserved, (const struct sockaddr *)&address,
             (int)sizeof(address)) == SOCKET_ERROR ||
        getsockname(reserved, (struct sockaddr *)&address,
                    &address_length) == SOCKET_ERROR) {
        FAIL("reserve port");
        closesocket(reserved);
        return;
    }

    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET ||
        ioctlsocket(client, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        FAIL("client socket");
        closesocket(reserved);
        if (client != INVALID_SOCKET) closesocket(client);
        return;
    }
    if (connect(client, (const struct sockaddr *)&address,
                (int)sizeof(address)) == SOCKET_ERROR) {
        int connect_error = WSAGetLastError();
        if (connect_error != WSAEWOULDBLOCK &&
            connect_error != WSAEINPROGRESS &&
            connect_error != WSAEALREADY &&
            connect_error != WSAECONNREFUSED) {
            FAIL("connect");
            closesocket(client);
            closesocket(reserved);
            return;
        }
        immediate_refusal = connect_error == WSAECONNREFUSED;
    }

    epfd = epoll_create1(0);
    memset(&event, 0, sizeof(event));
    event.events = EPOLLOUT;
    if (epfd < 0) {
        FAIL("create epoll for refused connect");
        closesocket(client);
        closesocket(reserved);
        return;
    }
    add_result = epoll_ctl(epfd, EPOLL_CTL_ADD, client, &event);
    if (add_result != 0 &&
        wepoll_ex_get_socket_lifetime_policy() ==
            WEPOLL_EX_SOCKET_LIFETIME_STRICT &&
        errno == EOPNOTSUPP) {
        /* A refused connect may lose its WFP ALE endpoint token before ADD.
         * Strict mode must reject that unprovable identity instead of falling
         * back to numeric SOCKET matching. */
        if (epoll_fd_count(epfd) != 0) {
            FAIL("strict refused-connect rollback");
        } else {
            PASS();
        }
        wepoll_close(epfd);
        closesocket(client);
        closesocket(reserved);
        return;
    }
    if (add_result != 0 ||
        epoll_wait(epfd, &output, 1, 5000) != 1 ||
        (output.events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) !=
            (EPOLLOUT | EPOLLERR | EPOLLHUP) ||
        getsockopt(client, SOL_SOCKET, SO_ERROR, (char *)&socket_error,
                   &socket_error_length) == SOCKET_ERROR ||
        (!immediate_refusal && socket_error != WSAECONNREFUSED) ||
        (immediate_refusal && socket_error != 0 &&
         socket_error != WSAECONNREFUSED)) {
        FAIL("connect failure delivery");
        if (epfd >= 0) wepoll_close(epfd);
        closesocket(client);
        closesocket(reserved);
        return;
    }
    wepoll_close(epfd);
    closesocket(client);
    closesocket(reserved);
    PASS();
}

static void test_zero_timeout(void)
{
    tcp_pair_t pair;
    struct epoll_event event;
    struct epoll_event_ex extended;
    /* Exercise both sides of the basic-wait stack-buffer threshold. */
    struct epoll_event stack_output[64];
    struct epoll_event heap_output[65];
    struct timespec zero = { 0, 0 };
    int epfd;

    TEST("zero timeout never blocks");
    if (make_tcp_pair(&pair) != 0) {
        FAIL("make_tcp_pair");
        return;
    }
    epfd = epoll_create1(0);
    if (epfd < 0) {
        FAIL("epoll_create1");
        tcp_pair_close(&pair);
        return;
    }
    memset(&event, 0, sizeof(event));
    memset(stack_output, 0, sizeof(stack_output));
    memset(heap_output, 0, sizeof(heap_output));
    event.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair.server, &event) != 0 ||
        epoll_wait(epfd, &event, 1, 0) != 0 ||
        epoll_pwait(epfd, stack_output, 64, 0, NULL) != 0 ||
        epoll_pwait(epfd, heap_output, 65, 0, NULL) != 0 ||
        epoll_wait_ex(epfd, &extended, 1, 0) != 0 ||
        epoll_pwait2(epfd, &event, 1, &zero, NULL) != 0) {
        FAIL("zero timeout");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    if (send_byte(pair.client) != 0 ||
        epoll_pwait(epfd, stack_output, 64, 1000, NULL) != 1 ||
        (stack_output[0].events & EPOLLIN) == 0 ||
        recv_byte(pair.server) != 0 ||
        send_byte(pair.client) != 0 ||
        epoll_pwait(epfd, heap_output, 65, 1000, NULL) != 1 ||
        (heap_output[0].events & EPOLLIN) == 0 ||
        recv_byte(pair.server) != 0) {
        FAIL("wait stack/heap scratch buffers");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    wepoll_close(epfd);
    tcp_pair_close(&pair);
    PASS();
}

static void test_pwait2_timeouts(void)
{
    tcp_pair_t pair;
    struct epoll_event event;
    struct epoll_event output;
    struct epoll_event_ex extended;
    struct timespec submillisecond = { 0, 500000L };
    struct timespec long_timeout = { INT_MAX, 0 };
    ULONGLONG started;
    int epfd = -1;

    TEST("epoll_pwait2 preserves submillisecond and long timespecs");
    epfd = epoll_create1(0);
    if (epfd < 0) {
        FAIL("epoll_create1");
        return;
    }
    started = GetTickCount64();
    if (epoll_pwait2(epfd, &output, 1, &submillisecond, NULL) != 0 ||
        epoll_pwait2_ex(epfd, &extended, 1,
                        &submillisecond, NULL) != 0 ||
        GetTickCount64() - started > 2000) {
        FAIL("positive submillisecond timeout");
        wepoll_close(epfd);
        return;
    }
    if (wepoll_close(epfd) != 0 || make_tcp_pair(&pair) != 0) {
        FAIL("long-timeout setup");
        return;
    }

    epfd = epoll_create1(0);
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = UINT64_C(0x707761697432);
    if (epfd < 0 ||
        epoll_ctl(epfd, EPOLL_CTL_ADD, pair.server, &event) != 0 ||
        send_byte(pair.client) != 0 ||
        epoll_pwait2(epfd, &output, 1, &long_timeout, NULL) != 1 ||
        output.data.u64 != event.data.u64 ||
        (output.events & EPOLLIN) == 0 || recv_byte(pair.server) != 0 ||
        send_byte(pair.client) != 0 ||
        epoll_pwait2_ex(epfd, &extended, 1,
                        &long_timeout, NULL) != 1 ||
        extended.data.u64 != event.data.u64 ||
        (extended.events & EPOLLIN) == 0 || recv_byte(pair.server) != 0) {
        FAIL("ready event with long timeout");
        if (epfd >= 0) wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, pair.server, NULL) != 0 ||
        wepoll_close(epfd) != 0) {
        FAIL("long-timeout cleanup");
        tcp_pair_close(&pair);
        return;
    }
    tcp_pair_close(&pair);
    PASS();
}

static void test_context_clear(void)
{
    tcp_pair_t pair;
    struct epoll_event event;
    struct epoll_event_ex output;
    int context_a = 1;
    int context_b = 2;
    int epfd;

    TEST("MOD updates and clears user context");
    if (make_tcp_pair(&pair) != 0) {
        FAIL("make_tcp_pair");
        return;
    }
    epfd = epoll_create1(0);
    if (epfd < 0) {
        FAIL("epoll_create1");
        tcp_pair_close(&pair);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = 7;
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pair.server,
                      &event, &context_a) != 0 ||
        send_byte(pair.client) != 0 ||
        epoll_wait_ex(epfd, &output, 1, 1000) != 1 ||
        output.user_ctx != &context_a || recv_byte(pair.server) != 0) {
        FAIL("initial context");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_MOD, pair.server,
                      &event, &context_b) != 0 ||
        send_byte(pair.client) != 0 ||
        epoll_wait_ex(epfd, &output, 1, 1000) != 1 ||
        output.user_ctx != &context_b || recv_byte(pair.server) != 0 ||
        epoll_ctl_ctx(epfd, EPOLL_CTL_MOD, pair.server,
                      &event, NULL) != 0 ||
        send_byte(pair.client) != 0 ||
        epoll_wait_ex(epfd, &output, 1, 1000) != 1 ||
        output.user_ctx != NULL || recv_byte(pair.server) != 0) {
        FAIL("MOD context update/clear");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    wepoll_close(epfd);
    tcp_pair_close(&pair);
    PASS();
}

static void test_oneshot_rearm(void)
{
    tcp_pair_t pair;
    struct epoll_event event;
    struct epoll_event output;
    int epfd;

    TEST("EPOLLONESHOT requires explicit rearm");
    if (make_tcp_pair(&pair) != 0) {
        FAIL("make_tcp_pair");
        return;
    }
    epfd = epoll_create1(0);
    if (epfd < 0) {
        FAIL("epoll_create1");
        tcp_pair_close(&pair);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLONESHOT;
    event.data.u64 = 11;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair.server, &event) != 0 ||
        send_byte(pair.client) != 0 ||
        epoll_wait(epfd, &output, 1, 1000) != 1 ||
        epoll_wait(epfd, &output, 1, 0) != 0 ||
        epoll_rearm(epfd, pair.server) != 0 ||
        epoll_wait(epfd, &output, 1, 1000) != 1 ||
        recv_byte(pair.server) != 0 ||
        epoll_ctl(epfd, EPOLL_CTL_DEL, pair.server, NULL) != 0) {
        FAIL("oneshot/rearm");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    wepoll_close(epfd);
    tcp_pair_close(&pair);
    PASS();
}

static void test_unsupported_event_modes(void)
{
    tcp_pair_t pair;
    struct epoll_event event;
    int epfd;

    TEST("accept EPOLLET and ADD-time EPOLLEXCLUSIVE contracts");
    if (make_tcp_pair(&pair) != 0) {
        FAIL("make_tcp_pair");
        return;
    }
    epfd = epoll_create1(0);
    if (epfd < 0) {
        FAIL("epoll_create1");
        tcp_pair_close(&pair);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair.server, &event) != 0 ||
        epoll_fd_count(epfd) != 1) {
        FAIL("EPOLLET ADD should succeed");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, pair.server, NULL) != 0) {
        FAIL("DEL after EPOLLET");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    event.events = EPOLLIN | EPOLLEXCLUSIVE;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair.server, &event) != 0 ||
        epoll_fd_count(epfd) != 1) {
        FAIL("EPOLLEXCLUSIVE ADD should succeed");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    /* Linux rejects every MOD of a registration added exclusive, even when
     * the MOD event mask does not repeat EPOLLEXCLUSIVE. */
    event.events = EPOLLIN;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, pair.server, &event) != -1 ||
        errno != EINVAL) {
        FAIL("EPOLLEXCLUSIVE MOD should return EINVAL");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    wepoll_close(epfd);
    tcp_pair_close(&pair);
    PASS();
}

static void test_native_close_cleanup(void)
{
    tcp_pair_t pair;
    struct epoll_event event;
    struct epoll_event output;
    int epfd;
    int first_result;
    int second_result;
    int saw_event = 0;

    TEST("native closesocket removes a registration");
    if (make_tcp_pair(&pair) != 0) {
        FAIL("make_tcp_pair");
        return;
    }
    epfd = epoll_create1(0);
    if (epfd < 0) {
        FAIL("epoll_create1");
        tcp_pair_close(&pair);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair.server, &event) != 0) {
        FAIL("ADD");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    closesocket(pair.server);
    pair.server = INVALID_SOCKET;

    first_result = 0;
    for (int attempt = 0;
         attempt < 100 && epoll_fd_count(epfd) != 0;
         attempt++) {
        first_result = epoll_wait(epfd, &output, 1, 0);
        if (first_result != 0) {
            saw_event = first_result > 0;
            break;
        }
        Sleep(1);
    }
    errno = 0;
    second_result = epoll_wait(epfd, &output, 1, 0);
    if (first_result < 0 || saw_event || second_result != 0 ||
        epoll_fd_count(epfd) != 0) {
        FAIL("close cleanup");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    wepoll_close(epfd);
    tcp_pair_close(&pair);
    PASS();
}

static void test_oneshot_native_close_cleanup(void)
{
    tcp_pair_t pair;
    struct epoll_event event;
    struct epoll_event output;
    int epfd;
    int wait_result = 0;
    int saw_event = 0;

    TEST("native close retires a fired oneshot registration");
    if (make_tcp_pair(&pair) != 0) {
        FAIL("make_tcp_pair");
        return;
    }
    epfd = epoll_create1(0);
    if (epfd < 0) {
        FAIL("epoll_create1");
        tcp_pair_close(&pair);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLONESHOT;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair.server, &event) != 0 ||
        send_byte(pair.client) != 0 ||
        epoll_wait(epfd, &output, 1, 1000) != 1) {
        FAIL("oneshot setup");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    closesocket(pair.server);
    pair.server = INVALID_SOCKET;

    for (int attempt = 0;
         attempt < 100 && epoll_fd_count(epfd) != 0;
         attempt++) {
        wait_result = epoll_wait(epfd, &output, 1, 0);
        if (wait_result != 0) {
            saw_event = wait_result > 0;
            break;
        }
        Sleep(1);
    }
    if (wait_result < 0 || saw_event || epoll_fd_count(epfd) != 0) {
        FAIL("oneshot close cleanup");
        wepoll_close(epfd);
        tcp_pair_close(&pair);
        return;
    }
    wepoll_close(epfd);
    tcp_pair_close(&pair);
    PASS();
}

static void test_batch_safety(void)
{
    tcp_pair_t first;
    tcp_pair_t second;
    struct epoll_event events[2];
    epoll_fd_t fds[2];
    int duplicate_ops[2] = { EPOLL_CTL_ADD, EPOLL_CTL_ADD };
    int add_ops[2] = { EPOLL_CTL_ADD, EPOLL_CTL_ADD };
    int del_ops[2] = { EPOLL_CTL_DEL, EPOLL_CTL_DEL };
    int epfd;

    TEST("batch validation and ADD rollback");
    if (make_tcp_pair(&first) != 0 || make_tcp_pair(&second) != 0) {
        FAIL("make_tcp_pair");
        tcp_pair_close(&first);
        tcp_pair_close(&second);
        return;
    }
    epfd = epoll_create1(0);
    if (epfd < 0) {
        FAIL("epoll_create1");
        tcp_pair_close(&first);
        tcp_pair_close(&second);
        return;
    }
    memset(events, 0, sizeof(events));
    events[0].events = EPOLLIN;
    events[1].events = EPOLLIN;
    fds[0] = first.server;
    fds[1] = first.server;
    if (epoll_ctl_batch(epfd, duplicate_ops, fds, events, 2) != -1 ||
        epoll_fd_count(epfd) != 0) {
        FAIL("duplicate ADD rollback");
        wepoll_close(epfd);
        tcp_pair_close(&first);
        tcp_pair_close(&second);
        return;
    }
    /* Rollback removes the public registration immediately even though its
     * cancelled AFD completion may still be queued internally. */
    fds[1] = second.server;
    if (epoll_ctl_batch(epfd, add_ops, fds, events, 2) != 0 ||
        epoll_fd_count(epfd) != 2 ||
        epoll_ctl_batch(epfd, del_ops, fds, NULL, 2) != 0 ||
        epoll_fd_count(epfd) != 0) {
        FAIL("valid ADD/DEL batch");
        wepoll_close(epfd);
        tcp_pair_close(&first);
        tcp_pair_close(&second);
        return;
    }
    wepoll_close(epfd);
    tcp_pair_close(&first);
    tcp_pair_close(&second);
    PASS();
}

static void test_epfd_collision_and_reuse(void)
{
    enum { EPFD_COUNT = 70, CLOSE_COUNT = 6 };
    int epfds[EPFD_COUNT];
    int replacements[CLOSE_COUNT];

    for (int i = 0; i < EPFD_COUNT; i++) {
        epfds[i] = -1;
    }
    for (int i = 0; i < CLOSE_COUNT; i++) {
        replacements[i] = -1;
    }

    TEST("epoll descriptor collisions survive close and reuse");
    for (int i = 0; i < EPFD_COUNT; i++) {
        epfds[i] = epoll_create1(0);
        if (epfds[i] < 0) {
            FAIL("create collision set");
            for (int j = 0; j < i; j++) {
                wepoll_close(epfds[j]);
            }
            return;
        }
    }
    for (int i = 0; i < CLOSE_COUNT; i++) {
        if (wepoll_close(epfds[i]) != 0) {
            FAIL("close collision entry");
            for (int j = i + 1; j < EPFD_COUNT; j++) {
                wepoll_close(epfds[j]);
            }
            return;
        }
    }
    for (int i = CLOSE_COUNT; i < EPFD_COUNT; i++) {
        if (epoll_fd_count(epfds[i]) != 0) {
            FAIL("lookup after collision deletion");
            for (int j = i; j < EPFD_COUNT; j++) {
                wepoll_close(epfds[j]);
            }
            return;
        }
    }
    for (int i = 0; i < CLOSE_COUNT; i++) {
        replacements[i] = epoll_create1(0);
        if (replacements[i] < 0) {
            FAIL("descriptor reuse");
            for (int j = 0; j < CLOSE_COUNT; j++) {
                if (replacements[j] > 0) {
                    wepoll_close(replacements[j]);
                }
            }
            for (int j = CLOSE_COUNT; j < EPFD_COUNT; j++) {
                wepoll_close(epfds[j]);
            }
            return;
        }
    }
    for (int i = CLOSE_COUNT; i < EPFD_COUNT; i++) {
        wepoll_close(epfds[i]);
    }
    for (int i = 0; i < CLOSE_COUNT; i++) {
        wepoll_close(replacements[i]);
    }
    PASS();
}

typedef struct close_wait_context {
    int epfd;
    HANDLE started;
    int result;
    int error;
} close_wait_context_t;

static DWORD WINAPI close_wait_thread(void *opaque)
{
    close_wait_context_t *context = (close_wait_context_t *)opaque;
    struct epoll_event event;

    if (context->started != NULL) {
        SetEvent(context->started);
    }
    context->result = epoll_wait(context->epfd, &event, 1, -1);
    context->error = errno;
    return 0;
}

static void test_concurrent_close(void)
{
    close_wait_context_t context;
    HANDLE thread;
    DWORD wait_result;

    TEST("close wakes a concurrent infinite wait");
    context.epfd = epoll_create1(0);
    context.started = CreateEventA(NULL, TRUE, FALSE, NULL);
    context.result = 0;
    context.error = 0;
    if (context.epfd < 0 || context.started == NULL) {
        FAIL("setup concurrent close");
        if (context.epfd > 0) {
            wepoll_close(context.epfd);
        }
        if (context.started != NULL) {
            CloseHandle(context.started);
        }
        return;
    }

    thread = CreateThread(NULL, 0, close_wait_thread, &context, 0, NULL);
    if (thread == NULL) {
        FAIL("CreateThread");
        wepoll_close(context.epfd);
        CloseHandle(context.started);
        return;
    }
    wait_result = WaitForSingleObject(context.started, 2000);
    if (wait_result != WAIT_OBJECT_0 || wepoll_close(context.epfd) != 0 ||
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 ||
        context.result != -1) {
        FAIL("concurrent close/wait");
        TerminateThread(thread, 1);
        CloseHandle(thread);
        CloseHandle(context.started);
        return;
    }
    CloseHandle(thread);
    CloseHandle(context.started);
    PASS();
}

typedef struct bounded_wait_context {
    int epfd;
    int timeout;
    int use_drain;
    HANDLE started;
    int result;
    int error;
    ULONGLONG elapsed;
} bounded_wait_context_t;

static DWORD WINAPI bounded_wait_thread(void *opaque)
{
    bounded_wait_context_t *context =
        (bounded_wait_context_t *)opaque;
    struct epoll_event event;
    ULONGLONG started = GetTickCount64();

    if (context->started != NULL) {
        SetEvent(context->started);
    }
    if (context->use_drain) {
        context->result = epoll_drain(context->epfd, &event, 1);
    } else {
        context->result = epoll_wait(context->epfd, &event, 1,
                                     context->timeout);
    }
    context->elapsed = GetTickCount64() - started;
    context->error = errno;
    return 0;
}

static void test_bounded_wait_under_contention(void)
{
    bounded_wait_context_t first;
    bounded_wait_context_t second;
    bounded_wait_context_t timed;
    HANDLE first_thread = NULL;
    HANDLE second_thread = NULL;
    HANDLE timed_thread = NULL;
    DWORD wait_result;
    int close_result = -1;
    int epfd = -1;
    int ok = 0;

    TEST("bounded waits stay bounded behind an infinite waiter");
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    memset(&timed, 0, sizeof(timed));
    epfd = epoll_create1(0);
    first.started = CreateEventA(NULL, TRUE, FALSE, NULL);
    first.epfd = epfd;
    first.timeout = -1;
    second.epfd = epfd;
    second.timeout = 0;
    second.use_drain = 1;
    timed.epfd = epfd;
    timed.timeout = 100;
    if (epfd < 0 || first.started == NULL) {
        FAIL("setup bounded wait");
        goto bounded_wait_cleanup;
    }

    first_thread = CreateThread(NULL, 0, bounded_wait_thread,
                                &first, 0, NULL);
    if (first_thread == NULL ||
        WaitForSingleObject(first.started, 2000) != WAIT_OBJECT_0) {
        FAIL("start infinite waiter");
        goto bounded_wait_cleanup;
    }
    Sleep(100);

    second_thread = CreateThread(NULL, 0, bounded_wait_thread,
                                 &second, 0, NULL);
    if (second_thread == NULL) {
        FAIL("start bounded waiter");
        goto bounded_wait_cleanup;
    }
    wait_result = WaitForSingleObject(second_thread, 1500);
    if (wait_result != WAIT_OBJECT_0 ||
        second.result != 0 || second.elapsed >= 1200) {
        goto bounded_wait_cleanup;
    }

    timed_thread = CreateThread(NULL, 0, bounded_wait_thread,
                                &timed, 0, NULL);
    if (timed_thread == NULL) {
        goto bounded_wait_cleanup;
    }
    wait_result = WaitForSingleObject(timed_thread, 1500);
    if (wait_result == WAIT_OBJECT_0 && timed.result == 0 &&
        timed.elapsed >= 50 && timed.elapsed < 1200 &&
        WaitForSingleObject(first_thread, 0) == WAIT_TIMEOUT) {
        ok = 1;
    }

bounded_wait_cleanup:
    if (epfd >= 0) {
        close_result = wepoll_close(epfd);
        epfd = -1;
    }
    if (first_thread != NULL &&
        WaitForSingleObject(first_thread, 5000) != WAIT_OBJECT_0) {
        TerminateThread(first_thread, 1);
    }
    if (second_thread != NULL &&
        WaitForSingleObject(second_thread, 5000) != WAIT_OBJECT_0) {
        TerminateThread(second_thread, 1);
    }
    if (timed_thread != NULL &&
        WaitForSingleObject(timed_thread, 5000) != WAIT_OBJECT_0) {
        TerminateThread(timed_thread, 1);
    }
    if (first_thread != NULL) CloseHandle(first_thread);
    if (second_thread != NULL) CloseHandle(second_thread);
    if (timed_thread != NULL) CloseHandle(timed_thread);
    if (first.started != NULL) CloseHandle(first.started);

    if (!ok || close_result != 0 || first.result != -1 ||
        first.error != EBADF) {
        FAIL("bounded waiter contention");
        return;
    }
    PASS();
}

int main(void)
{
    WSADATA wsa_data;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 2;
    }

    test_create_close();
    test_operational_stats();
    test_invalid_args();
    test_basic_io();
    test_write_ready();
    test_remote_half_close();
    test_remote_reset();
    test_connect_failure();
    test_zero_timeout();
    test_pwait2_timeouts();
    test_context_clear();
    test_oneshot_rearm();
    test_unsupported_event_modes();
    test_native_close_cleanup();
    test_oneshot_native_close_cleanup();
    test_batch_safety();
    test_epfd_collision_and_reuse();
    test_concurrent_close();
    test_bounded_wait_under_contention();

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    WSACleanup();
    return tests_failed == 0 ? 0 : 1;
}
