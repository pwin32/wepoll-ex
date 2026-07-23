/* Public Windows regressions for native closesocket() plus numeric SOCKET
 * reuse.  Each mode deliberately creates the replacement socket in the same
 * freed handle slot so stale registration metadata cannot hide behind timing. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct reuse_fixture {
    int epfd;
    SOCKET first_listener;
    SOCKET first_client;
    SOCKET first_server;
    SOCKET second_listener;
    SOCKET second_client;
    SOCKET second_server;
} reuse_fixture_t;

static void fixture_init(reuse_fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->epfd = -1;
    fixture->first_listener = INVALID_SOCKET;
    fixture->first_client = INVALID_SOCKET;
    fixture->first_server = INVALID_SOCKET;
    fixture->second_listener = INVALID_SOCKET;
    fixture->second_client = INVALID_SOCKET;
    fixture->second_server = INVALID_SOCKET;
}

static void fixture_close(reuse_fixture_t *fixture)
{
    if (fixture->epfd >= 0) {
        (void)wepoll_close(fixture->epfd);
        fixture->epfd = -1;
    }
    if (fixture->second_server != INVALID_SOCKET) {
        (void)closesocket(fixture->second_server);
    }
    if (fixture->second_client != INVALID_SOCKET) {
        (void)closesocket(fixture->second_client);
    }
    if (fixture->second_listener != INVALID_SOCKET) {
        (void)closesocket(fixture->second_listener);
    }
    if (fixture->first_server != INVALID_SOCKET) {
        (void)closesocket(fixture->first_server);
    }
    if (fixture->first_client != INVALID_SOCKET) {
        (void)closesocket(fixture->first_client);
    }
    if (fixture->first_listener != INVALID_SOCKET) {
        (void)closesocket(fixture->first_listener);
    }
}

static int open_listener(SOCKET *listener, struct sockaddr_in *address)
{
    int address_length = (int)sizeof(*address);

    *listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (*listener == INVALID_SOCKET) return -1;

    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address->sin_port = htons(0);
    if (bind(*listener, (const struct sockaddr *)address,
             (int)sizeof(*address)) == SOCKET_ERROR ||
        listen(*listener, 1) == SOCKET_ERROR ||
        getsockname(*listener, (struct sockaddr *)address,
                    &address_length) == SOCKET_ERROR) {
        return -1;
    }
    return 0;
}

static int connect_pair(SOCKET listener, const struct sockaddr_in *address,
                        SOCKET client, SOCKET *server)
{
    u_long nonblocking = 1;

    if (connect(client, (const struct sockaddr *)address,
                (int)sizeof(*address)) == SOCKET_ERROR) {
        return -1;
    }
    *server = accept(listener, NULL, NULL);
    if (*server == INVALID_SOCKET ||
        ioctlsocket(client, FIONBIO, &nonblocking) == SOCKET_ERROR ||
        ioctlsocket(*server, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        return -1;
    }
    return 0;
}

static int fixture_open_first(reuse_fixture_t *fixture)
{
    struct sockaddr_in address;

    if (open_listener(&fixture->first_listener, &address) != 0) return -1;
    fixture->first_client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fixture->first_client == INVALID_SOCKET ||
        connect_pair(fixture->first_listener, &address,
                     fixture->first_client, &fixture->first_server) != 0) {
        return -1;
    }
    fixture->epfd = epoll_create1(0);
    return fixture->epfd >= 0 ? 0 : -1;
}

/* Open the second listener before releasing expected_fd; the next socket
 * allocation then deterministically receives that just-freed handle slot on
 * the supported Windows handle allocator.  Retain unexpected candidates so
 * a provider that skips a slot still has a bounded chance to reach it. */
static int fixture_reuse_server_fd(reuse_fixture_t *fixture,
                                   SOCKET expected_fd)
{
    enum { MAX_CANDIDATES = 64 };
    SOCKET candidates[MAX_CANDIDATES];
    struct sockaddr_in address;
    int candidate_count = 0;
    int result = -1;

    for (int i = 0; i < MAX_CANDIDATES; i++) {
        candidates[i] = INVALID_SOCKET;
    }
    if (open_listener(&fixture->second_listener, &address) != 0) {
        return -1;
    }
    if (closesocket(fixture->first_server) == SOCKET_ERROR) return -1;
    fixture->first_server = INVALID_SOCKET;

    for (; candidate_count < MAX_CANDIDATES; candidate_count++) {
        SOCKET candidate = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (candidate == INVALID_SOCKET) goto cleanup;
        if (candidate == expected_fd) {
            fixture->second_client = candidate;
            break;
        }
        candidates[candidate_count] = candidate;
    }
    if (fixture->second_client == INVALID_SOCKET) {
        fprintf(stderr, "SOCKET value was not reused: expected %llu\n",
                (unsigned long long)expected_fd);
        goto cleanup;
    }
    if (connect_pair(fixture->second_listener, &address,
                     fixture->second_client,
                     &fixture->second_server) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    for (int i = 0; i < candidate_count; i++) {
        if (candidates[i] != INVALID_SOCKET) {
            (void)closesocket(candidates[i]);
        }
    }
    return result;
}

static int add_socket(int epfd, SOCKET socket_fd, uint32_t events,
                      uint64_t data)
{
    struct epoll_event event;

    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.u64 = data;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, socket_fd, &event);
}

static int modify_socket(int epfd, SOCKET socket_fd, uint32_t events,
                         uint64_t data)
{
    struct epoll_event event;

    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.u64 = data;
    return epoll_ctl(epfd, EPOLL_CTL_MOD, socket_fd, &event);
}

static int send_and_expect(reuse_fixture_t *fixture, uint64_t expected_data)
{
    static const char byte = 'x';
    struct epoll_event event;
    char received;

    if (send(fixture->second_server, &byte, 1, 0) != 1 ||
        epoll_wait(fixture->epfd, &event, 1, 2000) != 1 ||
        (event.events & EPOLLIN) == 0 ||
        event.data.u64 != expected_data ||
        recv(fixture->second_client, &received, 1, 0) != 1) {
        return -1;
    }
    return 0;
}

static int test_pending_add_reuse(void)
{
    static const uint64_t old_data = UINT64_C(0x1111222233334444);
    static const uint64_t new_data = UINT64_C(0xaaaabbbbccccdddd);
    reuse_fixture_t fixture;
    SOCKET reused_fd;
    int result = -1;

    fixture_init(&fixture);
    if (fixture_open_first(&fixture) != 0) goto cleanup;
    reused_fd = fixture.first_server;
    if (add_socket(fixture.epfd, reused_fd, EPOLLIN, old_data) != 0 ||
        fixture_reuse_server_fd(&fixture, reused_fd) != 0 ||
        fixture.second_client != reused_fd) {
        goto cleanup;
    }

    /* ADD must identify and retire the old pending registration.  Its late
     * cancellation/LOCAL_CLOSE completion must not remove this new entry. */
    if (add_socket(fixture.epfd, fixture.second_client,
                   EPOLLIN, new_data) != 0 ||
        epoll_fd_count(fixture.epfd) != 1 ||
        send_and_expect(&fixture, new_data) != 0 ||
        epoll_fd_count(fixture.epfd) != 1) {
        goto cleanup;
    }
    result = 0;

cleanup:
    fixture_close(&fixture);
    return result;
}

static int test_oneshot_rearm_reuse(void)
{
    static const char byte = 'o';
    static const uint64_t old_data = UINT64_C(0x0102030405060708);
    static const uint64_t new_data = UINT64_C(0x8899aabbccddeeff);
    reuse_fixture_t fixture;
    struct epoll_event event;
    SOCKET reused_fd;
    char received;
    int result = -1;

    fixture_init(&fixture);
    if (fixture_open_first(&fixture) != 0) goto cleanup;
    reused_fd = fixture.first_server;
    if (add_socket(fixture.epfd, reused_fd,
                   EPOLLIN | EPOLLONESHOT, old_data) != 0 ||
        send(fixture.first_client, &byte, 1, 0) != 1 ||
        epoll_wait(fixture.epfd, &event, 1, 2000) != 1 ||
        event.data.u64 != old_data ||
        recv(fixture.first_server, &received, 1, 0) != 1 ||
        fixture_reuse_server_fd(&fixture, reused_fd) != 0 ||
        fixture.second_client != reused_fd) {
        goto cleanup;
    }

    errno = 0;
    if (epoll_rearm(fixture.epfd, fixture.second_client) != -1 ||
        errno != ENOENT || epoll_fd_count(fixture.epfd) != 0 ||
        add_socket(fixture.epfd, fixture.second_client,
                   EPOLLIN, new_data) != 0 ||
        send_and_expect(&fixture, new_data) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    fixture_close(&fixture);
    return result;
}

static int test_queued_ready_reuse(void)
{
    static const char byte = 'q';
    static const uint64_t data_a = UINT64_C(0x1020304050607080);
    static const uint64_t data_b = UINT64_C(0x1122334455667788);
    static const uint64_t new_data = UINT64_C(0xfedcba9876543210);
    reuse_fixture_t first;
    reuse_fixture_t second;
    reuse_fixture_t replacement;
    reuse_fixture_t *delivered_fixture;
    reuse_fixture_t *stale_fixture;
    struct epoll_event event;
    SOCKET reused_fd;
    char received;
    int epfd = -1;
    int result = -1;

    fixture_init(&first);
    fixture_init(&second);
    fixture_init(&replacement);
    if (fixture_open_first(&first) != 0 ||
        fixture_open_first(&second) != 0) {
        goto cleanup;
    }
    epfd = first.epfd;
    first.epfd = -1;
    (void)wepoll_close(second.epfd);
    second.epfd = -1;

    if (add_socket(epfd, first.first_server, EPOLLIN, data_a) != 0 ||
        add_socket(epfd, second.first_server, EPOLLIN, data_b) != 0 ||
        send(first.first_client, &byte, 1, 0) != 1 ||
        send(second.first_client, &byte, 1, 0) != 1 ||
        epoll_wait(epfd, &event, 1, 2000) != 1) {
        goto cleanup;
    }

    if (event.data.u64 == data_a) {
        delivered_fixture = &first;
        stale_fixture = &second;
    } else if (event.data.u64 == data_b) {
        delivered_fixture = &second;
        stale_fixture = &first;
    } else {
        goto cleanup;
    }
    if (recv(delivered_fixture->first_server, &received, 1, 0) != 1 ||
        epoll_ctl(epfd, EPOLL_CTL_DEL,
                  delivered_fixture->first_server, NULL) != 0) {
        goto cleanup;
    }

    reused_fd = stale_fixture->first_server;
    replacement.first_server = stale_fixture->first_server;
    stale_fixture->first_server = INVALID_SOCKET;
    if (fixture_reuse_server_fd(&replacement, reused_fd) != 0 ||
        replacement.second_client != reused_fd) {
        goto cleanup;
    }

    memset(&event, 0, sizeof(event));
    if (epoll_wait(epfd, &event, 1, 500) != 0 ||
        epoll_fd_count(epfd) != 0 ||
        add_socket(epfd, replacement.second_client,
                   EPOLLIN, new_data) != 0 ||
        send(replacement.second_server, &byte, 1, 0) != 1 ||
        epoll_wait(epfd, &event, 1, 2000) != 1 ||
        event.data.u64 != new_data || (event.events & EPOLLIN) == 0 ||
        recv(replacement.second_client, &received, 1, 0) != 1) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (epfd >= 0) {
        (void)wepoll_close(epfd);
    }
    fixture_close(&replacement);
    fixture_close(&second);
    fixture_close(&first);
    return result;
}

static int test_transitional_connect(void)
{
    static const char byte = 't';
    static const uint64_t data = UINT64_C(0x13579bdf2468ace0);
    struct sockaddr_in address;
    struct epoll_event event;
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET server = INVALID_SOCKET;
    u_long nonblocking = 1;
    char received;
    int epfd = -1;
    int result = -1;

    if (open_listener(&listener, &address) != 0) goto cleanup;
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    epfd = epoll_create1(0);
    if (client == INVALID_SOCKET || epfd < 0 ||
        ioctlsocket(client, FIONBIO, &nonblocking) == SOCKET_ERROR ||
        add_socket(epfd, client, EPOLLIN, data) != 0) {
        goto cleanup;
    }

    if (connect(client, (const struct sockaddr *)&address,
                (int)sizeof(address)) == SOCKET_ERROR) {
        int connect_error = WSAGetLastError();
        if (connect_error != WSAEWOULDBLOCK &&
            connect_error != WSAEINPROGRESS &&
            connect_error != WSAEALREADY) {
            goto cleanup;
        }
    }
    server = accept(listener, NULL, NULL);
    if (server == INVALID_SOCKET || send(server, &byte, 1, 0) != 1 ||
        epoll_wait(epfd, &event, 1, 2000) != 1 ||
        event.data.u64 != data || (event.events & EPOLLIN) == 0 ||
        recv(client, &received, 1, 0) != 1 ||
        epoll_fd_count(epfd) != 1) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (server != INVALID_SOCKET) (void)closesocket(server);
    if (client != INVALID_SOCKET) (void)closesocket(client);
    if (listener != INVALID_SOCKET) (void)closesocket(listener);
    return result;
}

static int test_transitional_mod_before_connect(void)
{
    static const char byte = 'm';
    static const uint64_t old_data = UINT64_C(0x2468ace013579bdf);
    static const uint64_t new_data = UINT64_C(0xf0e1d2c3b4a59687);
    struct sockaddr_in address;
    struct epoll_event event;
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET server = INVALID_SOCKET;
    u_long nonblocking = 1;
    char received;
    int epfd = -1;
    int result = -1;

    if (open_listener(&listener, &address) != 0) goto cleanup;
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    epfd = epoll_create1(0);
    if (client == INVALID_SOCKET || epfd < 0 ||
        ioctlsocket(client, FIONBIO, &nonblocking) == SOCKET_ERROR ||
        add_socket(epfd, client, EPOLLRDHUP, old_data) != 0 ||
        /* This is an expansion from RDHUP to IN.  Before connect the
         * submitted transitional mask must already cover both interests. */
        modify_socket(epfd, client, EPOLLIN, new_data) != 0 ||
        epoll_fd_count(epfd) != 1) {
        goto cleanup;
    }

    if (connect(client, (const struct sockaddr *)&address,
                (int)sizeof(address)) == SOCKET_ERROR) {
        int connect_error = WSAGetLastError();
        if (connect_error != WSAEWOULDBLOCK &&
            connect_error != WSAEINPROGRESS &&
            connect_error != WSAEALREADY) {
            goto cleanup;
        }
    }
    server = accept(listener, NULL, NULL);
    if (server == INVALID_SOCKET || send(server, &byte, 1, 0) != 1 ||
        epoll_wait(epfd, &event, 1, 2000) != 1 ||
        event.data.u64 != new_data || (event.events & EPOLLIN) == 0 ||
        recv(client, &received, 1, 0) != 1 || epoll_fd_count(epfd) != 1) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (server != INVALID_SOCKET) (void)closesocket(server);
    if (client != INVALID_SOCKET) (void)closesocket(client);
    if (listener != INVALID_SOCKET) (void)closesocket(listener);
    return result;
}

static int test_transitional_reuse(void)
{
    static const uint64_t old_data = UINT64_C(0x0badf00d0badf00d);
    static const uint64_t new_data = UINT64_C(0x600dcafe600dcafe);
    reuse_fixture_t fixture;
    SOCKET reused_fd;
    int result = -1;

    fixture_init(&fixture);
    fixture.epfd = epoll_create1(0);
    fixture.first_server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fixture.epfd < 0 || fixture.first_server == INVALID_SOCKET) {
        goto cleanup;
    }
    reused_fd = fixture.first_server;
    if (add_socket(fixture.epfd, reused_fd, EPOLLIN, old_data) != 0 ||
        fixture_reuse_server_fd(&fixture, reused_fd) != 0 ||
        fixture.second_client != reused_fd ||
        add_socket(fixture.epfd, fixture.second_client,
                   EPOLLIN, new_data) != 0 ||
        epoll_fd_count(fixture.epfd) != 1 ||
        send_and_expect(&fixture, new_data) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    fixture_close(&fixture);
    return result;
}

int main(int argc, char **argv)
{
    WSADATA wsa_data;
    int result;

    if (argc != 2 || WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return 2;
    }
#ifdef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    if (strcmp(argv[1], "pending-add") == 0 ||
        strcmp(argv[1], "oneshot-rearm") == 0 ||
        strcmp(argv[1], "queued-ready") == 0 ||
        strcmp(argv[1], "transitional-reuse") == 0) {
        puts("identity mode skipped: synchronized socket lifetime contract");
        (void)WSACleanup();
        return 77;
    }
#endif
    if (strcmp(argv[1], "pending-add") == 0) {
        result = test_pending_add_reuse();
    } else if (strcmp(argv[1], "oneshot-rearm") == 0) {
        result = test_oneshot_rearm_reuse();
    } else if (strcmp(argv[1], "queued-ready") == 0) {
        result = test_queued_ready_reuse();
    } else if (strcmp(argv[1], "transitional-connect") == 0) {
        result = test_transitional_connect();
    } else if (strcmp(argv[1], "transitional-mod-before-connect") == 0) {
        result = test_transitional_mod_before_connect();
    } else if (strcmp(argv[1], "transitional-reuse") == 0) {
        result = test_transitional_reuse();
    } else {
        result = -1;
    }
    WSACleanup();
    if (result != 0) {
        fprintf(stderr, "identity mode failed: %s (errno=%d WSA=%d)\n",
                argc > 1 ? argv[1] : "<missing>", errno, WSAGetLastError());
        return 1;
    }
    printf("identity %s: OK\n", argv[1]);
    return 0;
}
