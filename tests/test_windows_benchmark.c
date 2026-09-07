/* Exercise the real benchmark driver with failures at its API boundary. */
#include "wepoll_ex.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned ctl_calls;
static unsigned fail_at;
static int fail_wait;
static int empty_wait;
static int fail_send;
static int fail_recv;
static int fail_join;
static volatile LONG injected;

static int test_ctl(int epfd, int op, epoll_fd_t fd,
                    struct epoll_event *event)
{
    if (++ctl_calls == fail_at) {
        InterlockedExchange(&injected, 1);
        errno = EIO;
        return -1;
    }
    return epoll_ctl(epfd, op, fd, event);
}

static int test_wait(int epfd, struct epoll_event *events,
                     int maxevents, int timeout)
{
    if (fail_wait || empty_wait) {
        InterlockedExchange(&injected, 1);
        Sleep(1);
        errno = EIO;
        return fail_wait ? -1 : 0;
    }
    return epoll_wait(epfd, events, maxevents, timeout);
}

static int test_send(SOCKET fd, const char *buffer, int length, int flags)
{
    if (fail_send) {
        InterlockedExchange(&injected, 1);
        WSASetLastError(WSAECONNRESET);
        return SOCKET_ERROR;
    }
    if (empty_wait) return length;
    return send(fd, buffer, length, flags);
}

static int test_recv(SOCKET fd, char *buffer, int length, int flags)
{
    if (fail_recv) {
        InterlockedExchange(&injected, 1);
        WSASetLastError(WSAECONNRESET);
        return SOCKET_ERROR;
    }
    return recv(fd, buffer, length, flags);
}

static DWORD test_join(HANDLE thread, DWORD timeout)
{
    if (fail_join) {
        InterlockedExchange(&injected, 1);
        return WAIT_TIMEOUT;
    }
    return WaitForSingleObject(thread, timeout);
}

static _Noreturn void test_exit_process(UINT code)
{
    /* The join-failure case must terminate before freeing worker storage. */
    _Exit(fail_join && injected && code == 1 ? 0 : 1);
}

#define epoll_ctl test_ctl
#define epoll_wait test_wait
#define send test_send
#define recv test_recv
#define WaitForSingleObject test_join
#define ExitProcess test_exit_process
#define main benchmark_main
#include "../bench/bench_mt_contention.c"
#undef main
#undef ExitProcess
#undef WaitForSingleObject
#undef recv
#undef send
#undef epoll_wait
#undef epoll_ctl

int main(int argc, char **argv)
{
    char *benchmark_argv[] = {"bench_mt_contention", "2", "1", "1", NULL};
    int expected_failure = 1;
    int result;

    if (argc != 2) return 2;
    if (strcmp(argv[1], "add") == 0) fail_at = 3;
    else if (strcmp(argv[1], "mod") == 0) fail_at = 4;
    else if (strcmp(argv[1], "del") == 0) fail_at = 5;
    else if (strcmp(argv[1], "wait") == 0) fail_wait = 1;
    else if (strcmp(argv[1], "empty") == 0) empty_wait = 1;
    else if (strcmp(argv[1], "send") == 0) fail_send = 1;
    else if (strcmp(argv[1], "recv") == 0) fail_recv = 1;
    else if (strcmp(argv[1], "join") == 0) fail_join = 1;
    else if (strcmp(argv[1], "success") == 0) expected_failure = 0;
    else return 2;

    result = benchmark_main(4, benchmark_argv);
    if (expected_failure ? result == 0 || !injected : result != 0) {
        fprintf(stderr, "benchmark mode=%s injected=%ld exit=%d\n",
                argv[1], injected, result);
        return 1;
    }
    return 0;
}
