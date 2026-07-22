/*
 * test_windows_events.c -- focused Windows AFD/epoll mapping regressions.
 *
 * Modes are intentionally selectable so the CPU-spin regression can run in
 * its own CTest process:
 *
 *     test_wepoll_ex_windows_events mapping
 *     test_wepoll_ex_windows_events fin
 *     test_wepoll_ex_windows_events spin
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex_internal.h"

#ifdef _WIN32

#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct tcp_pair {
    SOCKET client;
    SOCKET server;
} tcp_pair_t;

static int check_mask(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        return 0;
    }
    fprintf(stderr, "%s: expected 0x%08lx, got 0x%08lx\n",
            name, (unsigned long)expected, (unsigned long)actual);
    return -1;
}

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
    if (pair->server == INVALID_SOCKET) {
        tcp_pair_close(pair);
        return -1;
    }
    return 0;
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

static int test_mapping(void)
{
    const uint32_t always_afd =
        AFD_POLL_ABORT | AFD_POLL_CONNECT_FAIL | AFD_POLL_LOCAL_CLOSE;
    const uint32_t all_epoll =
        EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLERR | EPOLLHUP |
        EPOLLRDNORM | EPOLLRDBAND | EPOLLWRNORM | EPOLLWRBAND |
        EPOLLRDHUP;
    const ULONG all_afd =
        AFD_POLL_RECEIVE | AFD_POLL_RECEIVE_EXPEDITED | AFD_POLL_SEND |
        AFD_POLL_DISCONNECT | AFD_POLL_ABORT | AFD_POLL_LOCAL_CLOSE |
        AFD_POLL_ACCEPT | AFD_POLL_CONNECT_FAIL;

    if (check_mask("AFD receive",
                   ep_afd_to_epoll_events(AFD_POLL_RECEIVE),
                   EPOLLIN | EPOLLRDNORM) != 0 ||
        check_mask("AFD accept",
                   ep_afd_to_epoll_events(AFD_POLL_ACCEPT),
                   EPOLLIN | EPOLLRDNORM) != 0 ||
        check_mask("AFD expedited",
                   ep_afd_to_epoll_events(AFD_POLL_RECEIVE_EXPEDITED),
                   EPOLLPRI | EPOLLRDBAND) != 0 ||
        check_mask("AFD send",
                   ep_afd_to_epoll_events(AFD_POLL_SEND),
                   EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND) != 0 ||
        check_mask("AFD disconnect",
                   ep_afd_to_epoll_events(AFD_POLL_DISCONNECT),
                   EPOLLIN | EPOLLRDNORM | EPOLLRDHUP) != 0 ||
        check_mask("AFD abort",
                   ep_afd_to_epoll_events(AFD_POLL_ABORT),
                   EPOLLHUP) != 0 ||
        check_mask("AFD connect failure",
                   ep_afd_to_epoll_events(AFD_POLL_CONNECT_FAIL),
                   EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLRDNORM |
                       EPOLLWRNORM | EPOLLRDHUP) != 0 ||
        check_mask("AFD local close",
                   ep_afd_to_epoll_events(AFD_POLL_LOCAL_CLOSE),
                   EPOLLHUP) != 0 ||
        check_mask("AFD combined", ep_afd_to_epoll_events(all_afd),
                   all_epoll) != 0) {
        return -1;
    }

    if (check_mask("epoll zero", ep_epoll_to_afd_events(0),
                   always_afd) != 0 ||
        check_mask("epoll IN", ep_epoll_to_afd_events(EPOLLIN),
                   always_afd | AFD_POLL_RECEIVE | AFD_POLL_ACCEPT |
                       AFD_POLL_DISCONNECT) != 0 ||
        check_mask("epoll RDNORM", ep_epoll_to_afd_events(EPOLLRDNORM),
                   always_afd | AFD_POLL_RECEIVE | AFD_POLL_ACCEPT |
                       AFD_POLL_DISCONNECT) != 0 ||
        check_mask("epoll RDHUP", ep_epoll_to_afd_events(EPOLLRDHUP),
                   always_afd | AFD_POLL_DISCONNECT) != 0 ||
        check_mask("epoll PRI", ep_epoll_to_afd_events(EPOLLPRI),
                   always_afd | AFD_POLL_RECEIVE_EXPEDITED) != 0 ||
        check_mask("epoll RDBAND", ep_epoll_to_afd_events(EPOLLRDBAND),
                   always_afd | AFD_POLL_RECEIVE_EXPEDITED) != 0 ||
        check_mask("epoll OUT", ep_epoll_to_afd_events(EPOLLOUT),
                   always_afd | AFD_POLL_SEND) != 0 ||
        check_mask("epoll ERR", ep_epoll_to_afd_events(EPOLLERR),
                   always_afd) != 0 ||
        check_mask("epoll HUP", ep_epoll_to_afd_events(EPOLLHUP),
                   always_afd) != 0) {
        return -1;
    }

    puts("mapping: OK");
    return 0;
}

static int run_fin_case(uint32_t requested, uint32_t expected,
                        uint64_t data, const char *name)
{
    tcp_pair_t pair;
    struct epoll_event output;
    char byte;
    int epfd = -1;
    int result = -1;

    if (make_tcp_pair(&pair) != 0) {
        fprintf(stderr, "%s: make_tcp_pair failed, WSA=%d\n",
                name, WSAGetLastError());
        return -1;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 || add_socket(epfd, pair.server, requested, data) != 0 ||
        shutdown(pair.client, SD_SEND) == SOCKET_ERROR) {
        fprintf(stderr, "%s: setup failed, errno=%d WSA=%d\n",
                name, errno, WSAGetLastError());
        goto cleanup;
    }

    if (epoll_wait(epfd, &output, 1, 1500) != 1) {
        fprintf(stderr, "%s: wait did not deliver, errno=%d\n", name, errno);
        goto cleanup;
    }
    if (check_mask(name, output.events, expected) != 0 ||
        output.data.u64 != data) {
        fprintf(stderr, "%s: event data mismatch\n", name);
        goto cleanup;
    }
    if (recv(pair.server, &byte, 1, 0) != 0) {
        fprintf(stderr, "%s: EOF recv failed, WSA=%d\n",
                name, WSAGetLastError());
        goto cleanup;
    }
    result = 0;

cleanup:
    if (epfd >= 0) {
        (void)wepoll_close(epfd);
    }
    tcp_pair_close(&pair);
    return result;
}

static int test_fin_filters(void)
{
    tcp_pair_t pair;
    struct epoll_event output;
    int epfd = -1;
    int result = -1;

    if (run_fin_case(EPOLLIN, EPOLLIN, UINT64_C(0x1001),
                     "FIN IN-only") != 0 ||
        run_fin_case(EPOLLRDHUP, EPOLLRDHUP, UINT64_C(0x1002),
                     "FIN RDHUP-only") != 0 ||
        run_fin_case(EPOLLIN | EPOLLRDHUP, EPOLLIN | EPOLLRDHUP,
                     UINT64_C(0x1003), "FIN IN+RDHUP") != 0) {
        return -1;
    }

    if (make_tcp_pair(&pair) != 0) {
        fprintf(stderr, "FIN PRI-only: make_tcp_pair failed, WSA=%d\n",
                WSAGetLastError());
        return -1;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        add_socket(epfd, pair.server, EPOLLPRI, UINT64_C(0x1004)) != 0 ||
        shutdown(pair.client, SD_SEND) == SOCKET_ERROR) {
        fprintf(stderr, "FIN PRI-only: setup failed, errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }
    if (epoll_wait(epfd, &output, 1, 150) != 0 ||
        epoll_fd_count(epfd) != 1) {
        fprintf(stderr, "FIN PRI-only: unexpected event or registration loss\n");
        goto cleanup;
    }
    result = 0;

cleanup:
    if (epfd >= 0) {
        (void)wepoll_close(epfd);
    }
    tcp_pair_close(&pair);
    if (result == 0) {
        puts("fin: OK");
    }
    return result;
}

static uint64_t filetime_to_u64(FILETIME value)
{
    ULARGE_INTEGER converted;

    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

static int process_cpu_100ns(uint64_t *cpu_time)
{
    FILETIME create_time;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;

    if (!GetProcessTimes(GetCurrentProcess(), &create_time, &exit_time,
                         &kernel_time, &user_time)) {
        return -1;
    }
    *cpu_time = filetime_to_u64(kernel_time) + filetime_to_u64(user_time);
    return 0;
}

static int test_fin_spin(void)
{
    enum { WAIT_MS = 350, MAX_CPU_MS = 120 };
    tcp_pair_t pair;
    struct epoll_event output;
    uint64_t cpu_before;
    uint64_t cpu_after;
    uint64_t wall_before;
    uint64_t wall_after;
    int epfd = -1;
    int result = -1;

    if (make_tcp_pair(&pair) != 0) {
        fprintf(stderr, "spin: make_tcp_pair failed, WSA=%d\n",
                WSAGetLastError());
        return -1;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 || add_socket(epfd, pair.server, EPOLLPRI,
                               UINT64_C(0x2001)) != 0 ||
        shutdown(pair.client, SD_SEND) == SOCKET_ERROR ||
        process_cpu_100ns(&cpu_before) != 0) {
        fprintf(stderr, "spin: setup failed, errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }

    wall_before = GetTickCount64();
    if (epoll_wait(epfd, &output, 1, WAIT_MS) != 0 ||
        process_cpu_100ns(&cpu_after) != 0) {
        fprintf(stderr, "spin: wait failed or delivered an event, errno=%d\n",
                errno);
        goto cleanup;
    }
    wall_after = GetTickCount64();

    {
        uint64_t wall_ms = wall_after - wall_before;
        uint64_t cpu_ms = (cpu_after - cpu_before) / UINT64_C(10000);

        if (wall_ms + 25 < WAIT_MS || wall_ms > UINT64_C(2000) ||
            cpu_ms > MAX_CPU_MS) {
            fprintf(stderr,
                    "spin: wall=%llu ms CPU=%llu ms (limit %d ms)\n",
                    (unsigned long long)wall_ms,
                    (unsigned long long)cpu_ms, MAX_CPU_MS);
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    if (epfd >= 0) {
        (void)wepoll_close(epfd);
    }
    tcp_pair_close(&pair);
    if (result == 0) {
        puts("spin: OK");
    }
    return result;
}

static int run_mode(const char *mode)
{
    if (strcmp(mode, "mapping") == 0) {
        return test_mapping();
    }
    if (strcmp(mode, "fin") == 0) {
        return test_fin_filters();
    }
    if (strcmp(mode, "spin") == 0) {
        return test_fin_spin();
    }
    if (strcmp(mode, "all") == 0) {
        int failed = 0;

        failed |= test_mapping() != 0;
        failed |= test_fin_filters() != 0;
        failed |= test_fin_spin() != 0;
        return failed ? -1 : 0;
    }
    fprintf(stderr, "usage: test_windows_events [mapping|fin|spin|all]\n");
    return -1;
}

int main(int argc, char **argv)
{
    WSADATA wsa_data;
    const char *mode = argc == 1 ? "all" : argv[1];
    int result;

    if (argc > 2) {
        fprintf(stderr, "usage: test_windows_events [mapping|fin|spin|all]\n");
        return 2;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 2;
    }
    result = run_mode(mode);
    WSACleanup();
    return result == 0 ? 0 : 1;
}

#else

int main(void)
{
    return 0;
}

#endif
