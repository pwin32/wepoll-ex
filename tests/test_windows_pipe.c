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

static int expect_exact_event(int epfd, int timeout_ms,
                              uint32_t expected_events,
                              uint64_t expected_data,
                              const char *label)
{
    struct epoll_event event;
    int n;

    memset(&event, 0, sizeof(event));
    n = epoll_wait(epfd, &event, 1, timeout_ms);
    if (n != 1 || event.events != expected_events ||
        event.data.u64 != expected_data) {
        fprintf(stderr,
                "%s: expected events=0x%08lx data=%llu, "
                "got n=%d events=0x%08lx data=%llu errno=%d\n",
                label, (unsigned long)expected_events,
                (unsigned long long)expected_data, n,
                (unsigned long)(n > 0 ? event.events : 0),
                (unsigned long long)(n > 0 ? event.data.u64 : 0), errno);
        return -1;
    }
    return 0;
}

static int expect_no_event(int epfd, int timeout_ms, const char *label)
{
    uint32_t events = 0;
    int n = wait_one(epfd, timeout_ms, &events);

    if (n != 0) {
        fprintf(stderr, "%s: expected no event, got n=%d events=0x%08lx "
                "errno=%d\n", label, n, (unsigned long)events, errno);
        return -1;
    }
    return 0;
}

static volatile LONG pipe_name_serial;

static void make_named_pipe_name(wchar_t *name, size_t name_count,
                                 const wchar_t *tag)
{
    LONG id = InterlockedIncrement(&pipe_name_serial);

    _snwprintf(name, name_count,
               L"\\\\.\\pipe\\wepoll-ex-%ls-%lu-%llu-%ld", tag,
               (unsigned long)GetCurrentProcessId(),
               (unsigned long long)GetTickCount64(), (long)id);
    name[name_count - 1] = L'\0';
}

static int create_named_pipe_pair(const wchar_t *tag, DWORD server_open_mode,
                                  DWORD wait_mode, DWORD client_access,
                                  DWORD client_flags, HANDLE *server_out,
                                  HANDLE *client_out)
{
    wchar_t name[160];
    HANDLE server = INVALID_HANDLE_VALUE;
    HANDLE client = INVALID_HANDLE_VALUE;

    make_named_pipe_name(name, sizeof(name) / sizeof(name[0]), tag);
    server = CreateNamedPipeW(
        name, server_open_mode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | wait_mode,
        1, 4096, 4096, 0, NULL);
    if (server == INVALID_HANDLE_VALUE)
        goto fail;

    client = CreateFileW(name, client_access, 0, NULL, OPEN_EXISTING,
                         client_flags, NULL);
    if (client == INVALID_HANDLE_VALUE)
        goto fail;

    *server_out = server;
    *client_out = client;
    return 0;

fail:
    if (client != INVALID_HANDLE_VALUE)
        CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE)
        CloseHandle(server);
    return -1;
}

static int create_nonblocking_duplex_pipe(HANDLE *server_out,
                                          HANDLE *client_out)
{
    return create_named_pipe_pair(
        L"backpressure", PIPE_ACCESS_DUPLEX, PIPE_NOWAIT,
        GENERIC_READ | GENERIC_WRITE, 0, server_out, client_out);
}

static int fill_nonblocking_pipe(HANDLE writer, size_t *written_out)
{
    char buffer[4096];
    size_t total = 0;

    memset(buffer, 'p', sizeof(buffer));
    for (;;) {
        DWORD written = 0;

        if (!WriteFile(writer, buffer, sizeof(buffer), &written, NULL)) {
            DWORD error = GetLastError();

            if (error == ERROR_NO_DATA || error == ERROR_PIPE_BUSY)
                break;
            fprintf(stderr, "pipe-backpressure: WriteFile failed error=%lu\n",
                    (unsigned long)error);
            return -1;
        }
        if (written == 0)
            break;
        total += written;
        if (total > 64U * 1024U * 1024U) {
            fputs("pipe-backpressure: nonblocking pipe never filled\n",
                  stderr);
            return -1;
        }
    }
    if (total == 0) {
        fputs("pipe-backpressure: pipe accepted no initial data\n", stderr);
        return -1;
    }
    *written_out = total;
    return 0;
}

static int drain_pipe(HANDLE reader, size_t byte_count)
{
    char buffer[4096];
    size_t total = 0;

    while (total < byte_count) {
        size_t remaining = byte_count - total;
        DWORD requested = remaining < sizeof(buffer)
                              ? (DWORD)remaining
                              : (DWORD)sizeof(buffer);
        DWORD readn = 0;

        if (!ReadFile(reader, buffer, requested, &readn, NULL) || readn == 0) {
            fprintf(stderr, "pipe-backpressure: ReadFile failed error=%lu\n",
                    (unsigned long)GetLastError());
            return -1;
        }
        total += readn;
    }
    return 0;
}

static int test_pipe_readable(void)
{
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    struct epoll_event event;
    char buffer[8];
    DWORD written = 0;
    DWORD readn = 0;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    if (!CreatePipe(&rd, &wr, NULL, 0)) {
        fputs("pipe: CreatePipe failed\n", stderr);
        return 1;
    }

    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = 1;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)rd, &event) != 0) {
        fprintf(stderr, "pipe: ADD failed errno=%d\n", errno);
        goto fail;
    }

    n = wait_one(epfd, 30, &events);
    if (n != 0) {
        fprintf(stderr, "pipe: empty pipe ready n=%d events=%u\n", n, events);
        goto fail;
    }

    if (!WriteFile(wr, "abc", 3, &written, NULL) || written != 3)
        goto fail;

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr, "pipe: readable missing n=%d events=%u\n", n, events);
        goto fail;
    }

    if (!ReadFile(rd, buffer, sizeof(buffer), &readn, NULL) || readn != 3)
        goto fail;

    n = wait_one(epfd, 50, &events);
    if (n != 0) {
        fprintf(stderr, "pipe: drained pipe still ready n=%d\n", n);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(rd);
    CloseHandle(wr);
    puts("pipe-readable: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return 1;
}

static int test_pipe_et(void)
{
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    struct epoll_event event;
    char buffer[8];
    DWORD written = 0;
    DWORD readn = 0;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    if (!CreatePipe(&rd, &wr, NULL, 0))
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET;
    event.data.u64 = 2;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)rd, &event) != 0)
        goto fail;

    if (!WriteFile(wr, "xy", 2, &written, NULL) || written != 2)
        goto fail;

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0)
        goto fail;

    n = wait_one(epfd, 30, &events);
    if (n != 0) {
        fprintf(stderr, "pipe-et: redelivery n=%d\n", n);
        goto fail;
    }

    if (!ReadFile(rd, buffer, sizeof(buffer), &readn, NULL) || readn != 2)
        goto fail;

    n = wait_one(epfd, 0, &events);
    if (n != 0) {
        fprintf(stderr, "pipe-et: post-drain wait n=%d\n", n);
        goto fail;
    }

    if (!WriteFile(wr, "z", 1, &written, NULL) || written != 1)
        goto fail;

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr, "pipe-et: second edge missing n=%d\n", n);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(rd);
    CloseHandle(wr);
    puts("pipe-et: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return 1;
}

static int test_pipe_writable(void)
{
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    struct epoll_event event;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    if (!CreatePipe(&rd, &wr, NULL, 0))
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLOUT;
    event.data.u64 = 4;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)wr, &event) != 0)
        goto fail;

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLOUT) == 0) {
        fprintf(stderr, "pipe-writable: missing n=%d events=%u\n", n, events);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(rd);
    CloseHandle(wr);
    puts("pipe-writable: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return 1;
}

static int test_pipe_direction(void)
{
    const uint32_t read_events = EPOLLIN | EPOLLRDNORM | EPOLLRDBAND;
    const uint32_t write_events = EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND;
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    struct epoll_event event;
    DWORD written = 0;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    if (!CreatePipe(&rd, &wr, NULL, 0))
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = write_events;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)rd, &event) != 0)
        goto fail;
    n = wait_one(epfd, 30, &events);
    if (n != 0) {
        fprintf(stderr,
                "pipe-direction: read end reported write readiness "
                "n=%d events=%u\n",
                n, events);
        goto fail;
    }

    CloseHandle(wr);
    wr = NULL;
    events = 0;
    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & (EPOLLHUP | EPOLLERR)) == 0 ||
        (events & write_events) != 0) {
        fprintf(stderr,
                "pipe-direction: read-end HUP leaked write readiness "
                "n=%d events=%u\n",
                n, events);
        goto fail;
    }
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, (epoll_fd_t)rd, NULL) != 0)
        goto fail;
    CloseHandle(rd);
    rd = NULL;
    if (wepoll_close(epfd) != 0)
        goto fail;
    epfd = -1;

    if (!CreatePipe(&rd, &wr, NULL, 0))
        goto fail;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = read_events;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)wr, &event) != 0 ||
        !WriteFile(wr, "x", 1, &written, NULL) || written != 1)
        goto fail;
    events = 0;
    n = wait_one(epfd, 30, &events);
    if (n != 0) {
        fprintf(stderr,
                "pipe-direction: write end reported read readiness "
                "n=%d events=%u\n",
                n, events);
        goto fail;
    }

    CloseHandle(rd);
    rd = NULL;
    events = 0;
    /* PeekNamedPipe commonly remains ERROR_ACCESS_DENIED on a write-only
     * anonymous endpoint even after its reader closes.  A terminal report is
     * therefore optional here, but it must never fabricate read readiness. */
    n = wait_one(epfd, 50, &events);
    if (n < 0 || (n > 0 && (events & read_events) != 0)) {
        fprintf(stderr,
                "pipe-direction: write-end failure leaked read readiness "
                "n=%d events=%u\n",
                n, events);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(wr);
    puts("pipe-direction: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return 1;
}

static int test_pipe_oneshot(void)
{
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    struct epoll_event event;
    char buffer[8];
    DWORD written = 0;
    DWORD readn = 0;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    if (!CreatePipe(&rd, &wr, NULL, 0))
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLONESHOT;
    event.data.u64 = 5;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)rd, &event) != 0)
        goto fail;
    if (!WriteFile(wr, "a", 1, &written, NULL) || written != 1)
        goto fail;

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0)
        goto fail;
    n = wait_one(epfd, 30, &events);
    if (n != 0) {
        fprintf(stderr, "pipe-oneshot: redelivery n=%d\n", n);
        goto fail;
    }

    if (!ReadFile(rd, buffer, sizeof(buffer), &readn, NULL) || readn != 1)
        goto fail;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, (epoll_fd_t)rd, &event) != 0)
        goto fail;
    if (!WriteFile(wr, "b", 1, &written, NULL) || written != 1)
        goto fail;
    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr, "pipe-oneshot: rearm missing n=%d events=%u\n",
                n, events);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(rd);
    CloseHandle(wr);
    puts("pipe-oneshot: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return 1;
}

static int test_named_pipe_readable(void)
{
    wchar_t name[128];
    HANDLE rd = INVALID_HANDLE_VALUE;
    HANDLE wr = INVALID_HANDLE_VALUE;
    struct epoll_event event;
    DWORD written = 0;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    _snwprintf(name, sizeof(name) / sizeof(name[0]),
               L"\\\\.\\pipe\\wepoll-ex-%lu-%llu",
               (unsigned long)GetCurrentProcessId(),
               (unsigned long long)GetTickCount64());
    name[(sizeof(name) / sizeof(name[0])) - 1] = L'\0';

    rd = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND,
                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                          1, 4096, 4096, 0, NULL);
    if (rd == INVALID_HANDLE_VALUE)
        return 1;
    wr = CreateFileW(name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (wr == INVALID_HANDLE_VALUE)
        goto fail;
    if (!ConnectNamedPipe(rd, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
        goto fail;

    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = 6;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)rd, &event) != 0)
        goto fail;
    if (!WriteFile(wr, "named", 5, &written, NULL) || written != 5)
        goto fail;

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr, "named-pipe: missing n=%d events=%u\n", n, events);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(rd);
    CloseHandle(wr);
    puts("named-pipe: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (rd != INVALID_HANDLE_VALUE) CloseHandle(rd);
    if (wr != INVALID_HANDLE_VALUE) CloseHandle(wr);
    return 1;
}

static int test_pipe_hup(void)
{
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    struct epoll_event event;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    if (!CreatePipe(&rd, &wr, NULL, 0))
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)rd, &event) != 0)
        goto fail;

    CloseHandle(wr);
    wr = NULL;
    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLHUP) == 0) {
        fprintf(stderr, "pipe-hup: missing n=%d events=%u\n", n, events);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(rd);
    puts("pipe-hup: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return 1;
}

static int test_pipe_exact_eof(void)
{
    const uint32_t interest =
        EPOLLIN | EPOLLRDNORM | EPOLLRDBAND |
        EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND;
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    struct epoll_event event;
    char byte;
    DWORD written = 0;
    DWORD readn = 0;
    int epfd = -1;

    if (!CreatePipe(&rd, &wr, NULL, 0))
        return 1;
    CloseHandle(wr);
    wr = NULL;

    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = interest;
    event.data.u64 = UINT64_C(0x7001);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)rd, &event) != 0 ||
        expect_exact_event(epfd, 1000, EPOLLHUP, event.data.u64,
                           "pipe-eof-empty") != 0 ||
        expect_exact_event(epfd, 1000, EPOLLHUP, event.data.u64,
                           "pipe-eof-empty-repeat") != 0)
        goto fail;

    if (wepoll_close(epfd) != 0)
        goto fail;
    epfd = -1;
    CloseHandle(rd);
    rd = NULL;

    if (!CreatePipe(&rd, &wr, NULL, 0) ||
        !WriteFile(wr, "x", 1, &written, NULL) || written != 1)
        goto fail;
    CloseHandle(wr);
    wr = NULL;

    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = interest;
    event.data.u64 = UINT64_C(0x7002);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)rd, &event) != 0 ||
        expect_exact_event(epfd, 1000,
                           EPOLLIN | EPOLLRDNORM | EPOLLHUP,
                           event.data.u64, "pipe-eof-buffered") != 0 ||
        expect_exact_event(epfd, 1000,
                           EPOLLIN | EPOLLRDNORM | EPOLLHUP,
                           event.data.u64,
                           "pipe-eof-buffered-repeat") != 0)
        goto fail;
    if (!ReadFile(rd, &byte, 1, &readn, NULL) || readn != 1 || byte != 'x')
        goto fail;
    if (expect_exact_event(epfd, 1000, EPOLLHUP, event.data.u64,
                           "pipe-eof-drained") != 0 ||
        expect_exact_event(epfd, 1000, EPOLLHUP, event.data.u64,
                           "pipe-eof-drained-repeat") != 0)
        goto fail;

    (void)wepoll_close(epfd);
    CloseHandle(rd);
    puts("pipe-exact-eof: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return 1;
}

static int run_pipe_alias_case(const char *label, int monitor_read_end,
                               int peer_closed, uint32_t interest,
                               uint32_t expected)
{
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    HANDLE monitored;
    struct epoll_event event;
    DWORD written = 0;
    int epfd = -1;
    int result = 1;

    if (!CreatePipe(&rd, &wr, NULL, 0))
        goto done;
    if (monitor_read_end) {
        if (!WriteFile(wr, "a", 1, &written, NULL) || written != 1)
            goto done;
        if (peer_closed) {
            CloseHandle(wr);
            wr = NULL;
        }
    } else if (peer_closed) {
        CloseHandle(rd);
        rd = NULL;
    }

    monitored = monitor_read_end ? rd : wr;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto done;
    memset(&event, 0, sizeof(event));
    event.events = interest;
    event.data.u64 = UINT64_C(0x7100) + expected;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)monitored, &event) != 0)
        goto done;
    if (expected != 0) {
        if (expect_exact_event(epfd, 1000, expected, event.data.u64,
                               label) != 0)
            goto done;
    } else if (expect_no_event(epfd, 50, label) != 0) {
        goto done;
    }
    result = 0;

done:
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = 1;
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return result;
}

static int test_pipe_alias_masks(void)
{
    const uint32_t all_interest =
        EPOLLIN | EPOLLRDNORM | EPOLLRDBAND |
        EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND;

    if (run_pipe_alias_case("pipe-alias-read-all", 1, 0, all_interest,
                            EPOLLIN | EPOLLRDNORM) != 0 ||
        run_pipe_alias_case("pipe-alias-read-in", 1, 0, EPOLLIN,
                            EPOLLIN) != 0 ||
        run_pipe_alias_case("pipe-alias-read-rdnorm", 1, 0,
                            EPOLLRDNORM, EPOLLRDNORM) != 0 ||
        run_pipe_alias_case("pipe-alias-read-both", 1, 0,
                            EPOLLIN | EPOLLRDNORM,
                            EPOLLIN | EPOLLRDNORM) != 0 ||
        run_pipe_alias_case("pipe-alias-rdband", 1, 0,
                            EPOLLRDBAND, 0) != 0 ||
        run_pipe_alias_case("pipe-alias-write-all", 0, 0, all_interest,
                            EPOLLOUT | EPOLLWRNORM) != 0 ||
        run_pipe_alias_case("pipe-alias-write-out", 0, 0, EPOLLOUT,
                            EPOLLOUT) != 0 ||
        run_pipe_alias_case("pipe-alias-write-wrnorm", 0, 0,
                            EPOLLWRNORM, EPOLLWRNORM) != 0 ||
        run_pipe_alias_case("pipe-alias-write-both", 0, 0,
                            EPOLLOUT | EPOLLWRNORM,
                            EPOLLOUT | EPOLLWRNORM) != 0 ||
        run_pipe_alias_case("pipe-alias-wrband", 0, 0,
                            EPOLLWRBAND, 0) != 0 ||
        run_pipe_alias_case("pipe-alias-eof-all", 1, 1, all_interest,
                            EPOLLIN | EPOLLRDNORM | EPOLLHUP) != 0 ||
        run_pipe_alias_case("pipe-alias-eof-in", 1, 1, EPOLLIN,
                            EPOLLIN | EPOLLHUP) != 0 ||
        run_pipe_alias_case("pipe-alias-eof-rdnorm", 1, 1,
                            EPOLLRDNORM,
                            EPOLLRDNORM | EPOLLHUP) != 0 ||
        run_pipe_alias_case("pipe-alias-eof-both", 1, 1,
                            EPOLLIN | EPOLLRDNORM,
                            EPOLLIN | EPOLLRDNORM | EPOLLHUP) != 0 ||
        run_pipe_alias_case("pipe-alias-eof-rdband", 1, 1,
                            EPOLLRDBAND, EPOLLHUP) != 0 ||
        run_pipe_alias_case("pipe-alias-error-all", 0, 1, all_interest,
                            EPOLLOUT | EPOLLWRNORM | EPOLLERR) != 0 ||
        run_pipe_alias_case("pipe-alias-error-out", 0, 1, EPOLLOUT,
                            EPOLLOUT | EPOLLERR) != 0 ||
        run_pipe_alias_case("pipe-alias-error-wrnorm", 0, 1,
                            EPOLLWRNORM,
                            EPOLLWRNORM | EPOLLERR) != 0 ||
        run_pipe_alias_case("pipe-alias-error-both", 0, 1,
                            EPOLLOUT | EPOLLWRNORM,
                            EPOLLOUT | EPOLLWRNORM | EPOLLERR) != 0 ||
        run_pipe_alias_case("pipe-alias-error-wrband", 0, 1,
                            EPOLLWRBAND, EPOLLERR) != 0)
        return 1;
    puts("pipe-alias-masks: OK");
    return 0;
}

static int run_pipe_broken_writer_transition(int edge_triggered)
{
    uint32_t interest = EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND;
    const uint32_t initial = EPOLLOUT | EPOLLWRNORM;
    const uint32_t expected = EPOLLOUT | EPOLLWRNORM | EPOLLERR;
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    struct epoll_event event;
    int epfd = -1;

    if (!CreatePipe(&rd, &wr, NULL, 0))
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    if (edge_triggered)
        interest |= EPOLLET;
    event.events = interest;
    event.data.u64 = edge_triggered
                         ? UINT64_C(0x7202)
                         : UINT64_C(0x7201);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)wr, &event) != 0 ||
        expect_exact_event(epfd, 1000, initial, event.data.u64,
                           edge_triggered ? "pipe-writer-et-initial"
                                          : "pipe-writer-lt-initial") != 0)
        goto fail;
    if (edge_triggered &&
        expect_no_event(epfd, 75, "pipe-writer-et-initial-stable") != 0)
        goto fail;

    CloseHandle(rd);
    rd = NULL;
    if (expect_exact_event(epfd, 1000, expected, event.data.u64,
                           edge_triggered ? "pipe-writer-et-error"
                                          : "pipe-writer-lt-error") != 0)
        goto fail;
    if (edge_triggered) {
        if (expect_no_event(epfd, 100,
                            "pipe-writer-et-error-stable") != 0)
            goto fail;
    } else if (expect_exact_event(
                   epfd, 1000, expected, event.data.u64,
                   "pipe-writer-lt-error-repeat") != 0) {
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(wr);
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return 1;
}

static int test_pipe_broken_writer(void)
{
    if (run_pipe_broken_writer_transition(0) != 0)
        return 1;
    puts("pipe-broken-writer: OK");
    return 0;
}

static int test_pipe_broken_writer_et(void)
{
    if (run_pipe_broken_writer_transition(1) != 0)
        return 1;
    puts("pipe-broken-writer-et: OK");
    return 0;
}

static int run_pipe_terminal_oneshot_case(int monitor_read_end)
{
    const uint32_t read_interest =
        EPOLLIN | EPOLLRDNORM | EPOLLRDBAND | EPOLLONESHOT;
    const uint32_t write_interest =
        EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND | EPOLLONESHOT;
    const uint32_t expected = monitor_read_end
                                  ? EPOLLHUP
                                  : EPOLLOUT | EPOLLWRNORM | EPOLLERR;
    const char *first_label = monitor_read_end
                                  ? "pipe-oneshot-hup-first"
                                  : "pipe-oneshot-err-first";
    const char *rearm_label = monitor_read_end
                                  ? "pipe-oneshot-hup-rearm"
                                  : "pipe-oneshot-err-rearm";
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    HANDLE monitored;
    struct epoll_event event;
    int epfd = -1;
    int result = 1;

    if (!CreatePipe(&rd, &wr, NULL, 0))
        goto done;
    if (monitor_read_end) {
        CloseHandle(wr);
        wr = NULL;
        monitored = rd;
    } else {
        CloseHandle(rd);
        rd = NULL;
        monitored = wr;
    }

    epfd = epoll_create1(0);
    if (epfd < 0)
        goto done;
    memset(&event, 0, sizeof(event));
    event.events = monitor_read_end ? read_interest : write_interest;
    event.data.u64 = monitor_read_end
                         ? UINT64_C(0x7301)
                         : UINT64_C(0x7302);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)monitored, &event) != 0 ||
        expect_exact_event(epfd, 1000, expected, event.data.u64,
                           first_label) != 0 ||
        expect_no_event(epfd, 50, first_label) != 0)
        goto done;
    if (epoll_fd_count(epfd) != 1) {
        fprintf(stderr, "%s: fired ONESHOT registration was removed\n",
                first_label);
        goto done;
    }

    event.data.u64 += UINT64_C(0x100);
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, (epoll_fd_t)monitored, &event) != 0 ||
        expect_exact_event(epfd, 1000, expected, event.data.u64,
                           rearm_label) != 0 ||
        expect_no_event(epfd, 50, rearm_label) != 0)
        goto done;
    result = 0;

done:
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = 1;
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return result;
}

static int test_pipe_terminal_oneshot(void)
{
    if (run_pipe_terminal_oneshot_case(1) != 0 ||
        run_pipe_terminal_oneshot_case(0) != 0)
        return 1;
    puts("pipe-terminal-oneshot: OK");
    return 0;
}

static int run_pipe_terminal_oneshot_rearm_case(int monitor_read_end)
{
    const uint32_t read_interest =
        EPOLLIN | EPOLLRDNORM | EPOLLRDBAND | EPOLLET | EPOLLONESHOT;
    const uint32_t write_interest =
        EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND | EPOLLET | EPOLLONESHOT;
    const uint32_t expected = monitor_read_end
                                  ? EPOLLHUP
                                  : EPOLLOUT | EPOLLWRNORM | EPOLLERR;
    const char *first_label = monitor_read_end
                                  ? "pipe-et-oneshot-hup-first"
                                  : "pipe-et-oneshot-err-first";
    const char *rearm_label = monitor_read_end
                                  ? "pipe-et-oneshot-hup-rearm"
                                  : "pipe-et-oneshot-err-rearm";
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    HANDLE monitored;
    struct epoll_event event;
    int epfd = -1;
    int result = 1;

    if (!CreatePipe(&rd, &wr, NULL, 0))
        goto done;
    if (monitor_read_end) {
        CloseHandle(wr);
        wr = NULL;
        monitored = rd;
    } else {
        CloseHandle(rd);
        rd = NULL;
        monitored = wr;
    }

    epfd = epoll_create1(0);
    if (epfd < 0)
        goto done;
    memset(&event, 0, sizeof(event));
    event.events = monitor_read_end ? read_interest : write_interest;
    event.data.u64 = monitor_read_end
                         ? UINT64_C(0x7311)
                         : UINT64_C(0x7312);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)monitored, &event) != 0 ||
        expect_exact_event(epfd, 1000, expected, event.data.u64,
                           first_label) != 0 ||
        expect_no_event(epfd, 50, first_label) != 0)
        goto done;
    if (epoll_fd_count(epfd) != 1) {
        fprintf(stderr, "%s: fired ONESHOT registration was removed\n",
                first_label);
        goto done;
    }

    if (epoll_rearm(epfd, (epoll_fd_t)monitored) != 0 ||
        expect_exact_event(epfd, 1000, expected, event.data.u64,
                           rearm_label) != 0 ||
        expect_no_event(epfd, 50, rearm_label) != 0)
        goto done;
    result = 0;

done:
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = 1;
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return result;
}

static int test_pipe_terminal_oneshot_rearm(void)
{
    if (run_pipe_terminal_oneshot_rearm_case(1) != 0 ||
        run_pipe_terminal_oneshot_rearm_case(0) != 0)
        return 1;
    puts("pipe-terminal-oneshot-rearm: OK");
    return 0;
}

static int test_pipe_et_hup_transitions(void)
{
    const uint32_t interest =
        EPOLLIN | EPOLLRDNORM | EPOLLRDBAND | EPOLLET;
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    struct epoll_event event;
    char byte;
    DWORD written = 0;
    DWORD readn = 0;
    int epfd = -1;

    if (!CreatePipe(&rd, &wr, NULL, 0))
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = interest;
    event.data.u64 = UINT64_C(0x7401);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)rd, &event) != 0 ||
        !WriteFile(wr, "e", 1, &written, NULL) || written != 1 ||
        expect_exact_event(epfd, 1000, EPOLLIN | EPOLLRDNORM,
                           event.data.u64, "pipe-et-data") != 0 ||
        expect_no_event(epfd, 50, "pipe-et-data-stable") != 0)
        goto fail;

    CloseHandle(wr);
    wr = NULL;
    if (expect_exact_event(epfd, 1000,
                           EPOLLIN | EPOLLRDNORM | EPOLLHUP,
                           event.data.u64, "pipe-et-buffered-hup") != 0 ||
        expect_no_event(epfd, 50, "pipe-et-buffered-hup-stable") != 0)
        goto fail;
    if (!ReadFile(rd, &byte, 1, &readn, NULL) || readn != 1 || byte != 'e')
        goto fail;
    if (expect_exact_event(epfd, 1000, EPOLLHUP, event.data.u64,
                           "pipe-et-drained-hup") != 0 ||
        expect_no_event(epfd, 50, "pipe-et-drained-hup-stable") != 0)
        goto fail;

    (void)wepoll_close(epfd);
    CloseHandle(rd);
    puts("pipe-et-hup-transitions: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return 1;
}

static int run_pipe_write_backpressure(int edge_triggered)
{
    const uint32_t expected = EPOLLOUT | EPOLLWRNORM;
    HANDLE server = INVALID_HANDLE_VALUE;
    HANDLE client = INVALID_HANDLE_VALUE;
    struct epoll_event event;
    size_t queued = 0;
    int epfd = -1;
    int result = 1;

    if (create_nonblocking_duplex_pipe(&server, &client) != 0)
        goto done;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto done;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND;
    if (edge_triggered)
        event.events |= EPOLLET;
    event.data.u64 = edge_triggered
                         ? UINT64_C(0x7502)
                         : UINT64_C(0x7501);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)server, &event) != 0 ||
        expect_exact_event(epfd, 1000, expected, event.data.u64,
                           edge_triggered ? "pipe-backpressure-et-initial"
                                          : "pipe-backpressure-lt-initial") != 0)
        goto done;
    if (edge_triggered) {
        if (expect_no_event(epfd, 50,
                            "pipe-backpressure-et-initial-stable") != 0)
            goto done;
    } else if (expect_exact_event(epfd, 1000, expected, event.data.u64,
                                  "pipe-backpressure-lt-repeat") != 0) {
        goto done;
    }

    if (fill_nonblocking_pipe(server, &queued) != 0)
        goto done;
    if (expect_no_event(epfd, 100,
                        edge_triggered ? "pipe-backpressure-et-full"
                                       : "pipe-backpressure-lt-full") != 0)
        goto done;
    if (drain_pipe(client, queued) != 0)
        goto done;
    if (expect_exact_event(epfd, 1000, expected, event.data.u64,
                           edge_triggered ? "pipe-backpressure-et-restored"
                                          : "pipe-backpressure-lt-restored") != 0)
        goto done;
    if (edge_triggered) {
        if (expect_no_event(epfd, 50,
                            "pipe-backpressure-et-restored-stable") != 0)
            goto done;
        if (fill_nonblocking_pipe(server, &queued) != 0 ||
            expect_no_event(epfd, 100,
                            "pipe-backpressure-et-refill") != 0 ||
            drain_pipe(client, queued) != 0 ||
            expect_exact_event(epfd, 1000, expected, event.data.u64,
                               "pipe-backpressure-et-rerestored") != 0 ||
            expect_no_event(epfd, 50,
                            "pipe-backpressure-et-rerestored-stable") != 0)
            goto done;
    } else if (expect_exact_event(epfd, 1000, expected, event.data.u64,
                                  "pipe-backpressure-lt-restored-repeat") != 0) {
        goto done;
    }
    result = 0;

done:
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = 1;
    if (client != INVALID_HANDLE_VALUE)
        CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE)
        CloseHandle(server);
    return result;
}

static int test_pipe_write_backpressure_lt(void)
{
    if (run_pipe_write_backpressure(0) != 0)
        return 1;
    puts("pipe-write-backpressure-lt: OK");
    return 0;
}

static int test_pipe_write_backpressure_et(void)
{
    if (run_pipe_write_backpressure(1) != 0)
        return 1;
    puts("pipe-write-backpressure-et: OK");
    return 0;
}

static int run_named_overlapped_exact_case(const char *label,
                                           int buffered,
                                           int peer_closed,
                                           uint32_t expected)
{
    const uint32_t interest =
        EPOLLIN | EPOLLRDNORM | EPOLLRDBAND |
        EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND;
    HANDLE server = INVALID_HANDLE_VALUE;
    HANDLE client = INVALID_HANDLE_VALUE;
    struct epoll_event event;
    DWORD written = 0;
    int epfd = -1;
    int result = 1;

    if (create_named_pipe_pair(
            L"overlapped", PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_WAIT, GENERIC_WRITE, 0, &server, &client) != 0)
        goto done;
    if (buffered &&
        (!WriteFile(client, "o", 1, &written, NULL) || written != 1))
        goto done;
    if (peer_closed) {
        CloseHandle(client);
        client = INVALID_HANDLE_VALUE;
    }

    epfd = epoll_create1(0);
    if (epfd < 0)
        goto done;
    memset(&event, 0, sizeof(event));
    event.events = interest;
    event.data.u64 = UINT64_C(0x7600) + expected;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)server, &event) != 0 ||
        expect_exact_event(epfd, 1000, expected, event.data.u64,
                           label) != 0)
        goto done;
    result = 0;

done:
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = 1;
    if (client != INVALID_HANDLE_VALUE)
        CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE)
        CloseHandle(server);
    return result;
}

static int run_named_overlapped_client_case(const char *label,
                                            int peer_closed,
                                            uint32_t expected)
{
    const uint32_t interest =
        EPOLLIN | EPOLLRDNORM | EPOLLRDBAND |
        EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND;
    HANDLE server = INVALID_HANDLE_VALUE;
    HANDLE client = INVALID_HANDLE_VALUE;
    struct epoll_event event;
    int epfd = -1;
    int result = 1;

    if (create_named_pipe_pair(
            L"overlapped-client", PIPE_ACCESS_INBOUND, PIPE_WAIT,
            GENERIC_WRITE, FILE_FLAG_OVERLAPPED, &server, &client) != 0)
        goto done;
    if (peer_closed) {
        CloseHandle(server);
        server = INVALID_HANDLE_VALUE;
    }

    epfd = epoll_create1(0);
    if (epfd < 0)
        goto done;
    memset(&event, 0, sizeof(event));
    event.events = interest;
    event.data.u64 = UINT64_C(0x7680) + expected;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)client, &event) != 0 ||
        expect_exact_event(epfd, 1000, expected, event.data.u64,
                           label) != 0)
        goto done;
    result = 0;

done:
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = 1;
    if (client != INVALID_HANDLE_VALUE)
        CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE)
        CloseHandle(server);
    return result;
}

static int test_named_pipe_overlapped_exact(void)
{
    if (run_named_overlapped_exact_case(
            "pipe-overlapped-data", 1, 0,
            EPOLLIN | EPOLLRDNORM) != 0 ||
        run_named_overlapped_exact_case(
            "pipe-overlapped-buffered-eof", 1, 1,
            EPOLLIN | EPOLLRDNORM | EPOLLHUP) != 0 ||
        run_named_overlapped_exact_case(
            "pipe-overlapped-empty-eof", 0, 1, EPOLLHUP) != 0 ||
        run_named_overlapped_client_case(
            "pipe-overlapped-client-writable", 0,
            EPOLLOUT | EPOLLWRNORM) != 0 ||
        run_named_overlapped_client_case(
            "pipe-overlapped-client-broken", 1,
            EPOLLOUT | EPOLLWRNORM | EPOLLERR) != 0)
        return 1;
    puts("pipe-named-overlapped-exact: OK");
    return 0;
}

static int test_named_pipe_outbound_fallback(void)
{
    const uint32_t interest =
        EPOLLIN | EPOLLRDNORM | EPOLLRDBAND |
        EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND | EPOLLET;
    const uint32_t writable = EPOLLOUT | EPOLLWRNORM;
    const uint32_t broken = writable | EPOLLERR;
    wchar_t name[160];
    HANDLE server = INVALID_HANDLE_VALUE;
    HANDLE client = INVALID_HANDLE_VALUE;
    struct epoll_event event;
    DWORD error;
    int epfd = -1;
    int result = 1;

    make_named_pipe_name(name, sizeof(name) / sizeof(name[0]),
                         L"outbound-fallback");
    server = CreateNamedPipeW(
        name, PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
        1, 4096, 4096, 0, NULL);
    if (server == INVALID_HANDLE_VALUE)
        goto done;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto done;
    memset(&event, 0, sizeof(event));
    event.events = interest;
    event.data.u64 = UINT64_C(0x7701);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)server, &event) != 0 ||
        expect_no_event(epfd, 75,
                        "pipe-outbound-fallback-listening") != 0)
        goto done;

    client = CreateFileW(name, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0,
                         NULL);
    if (client == INVALID_HANDLE_VALUE ||
        expect_exact_event(epfd, 1000, writable, event.data.u64,
                           "pipe-outbound-fallback-connected") != 0 ||
        expect_no_event(epfd, 75,
                        "pipe-outbound-fallback-connected-stable") != 0)
        goto done;

    CloseHandle(client);
    client = INVALID_HANDLE_VALUE;
    if (expect_exact_event(epfd, 1000, broken, event.data.u64,
                           "pipe-outbound-fallback-broken") != 0 ||
        expect_no_event(epfd, 75,
                        "pipe-outbound-fallback-broken-stable") != 0)
        goto done;

    if (!DisconnectNamedPipe(server)) {
        fprintf(stderr, "pipe-outbound-fallback: disconnect error=%lu\n",
                (unsigned long)GetLastError());
        goto done;
    }
    if (ConnectNamedPipe(server, NULL)) {
        fputs("pipe-outbound-fallback: empty reconnect succeeded\n",
              stderr);
        goto done;
    }
    error = GetLastError();
    if (error != ERROR_PIPE_LISTENING) {
        fprintf(stderr,
                "pipe-outbound-fallback: reconnect error=%lu\n",
                (unsigned long)error);
        goto done;
    }
    if (expect_no_event(epfd, 75,
                        "pipe-outbound-fallback-relistening") != 0)
        goto done;

    client = CreateFileW(name, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0,
                         NULL);
    if (client == INVALID_HANDLE_VALUE ||
        expect_exact_event(epfd, 1000, writable, event.data.u64,
                           "pipe-outbound-fallback-reconnected") != 0 ||
        expect_no_event(epfd, 75,
                        "pipe-outbound-fallback-reconnected-stable") != 0)
        goto done;
    result = 0;

done:
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = 1;
    if (client != INVALID_HANDLE_VALUE)
        CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE)
        CloseHandle(server);
    if (result == 0)
        puts("pipe-named-outbound-fallback: OK");
    return result;
}

static int test_named_pipe_et_reconnect(void)
{
    const uint32_t interest =
        EPOLLIN | EPOLLRDNORM | EPOLLRDBAND | EPOLLET;
    const uint32_t readable = EPOLLIN | EPOLLRDNORM;
    wchar_t name[160];
    HANDLE server = INVALID_HANDLE_VALUE;
    HANDLE client = INVALID_HANDLE_VALUE;
    struct epoll_event event;
    char byte;
    DWORD written = 0;
    DWORD readn = 0;
    DWORD error;
    int epfd = -1;
    int result = 1;

    make_named_pipe_name(name, sizeof(name) / sizeof(name[0]),
                         L"reconnect");
    server = CreateNamedPipeW(
        name, PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
        1, 4096, 4096, 0, NULL);
    if (server == INVALID_HANDLE_VALUE)
        goto done;
    if (ConnectNamedPipe(server, NULL)) {
        fputs("pipe-reconnect: empty nonblocking connect succeeded\n",
              stderr);
        goto done;
    }
    error = GetLastError();
    if (error != ERROR_PIPE_LISTENING) {
        fprintf(stderr, "pipe-reconnect: initial listen error=%lu\n",
                (unsigned long)error);
        goto done;
    }
    client = CreateFileW(name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0,
                         NULL);
    if (client == INVALID_HANDLE_VALUE)
        goto done;

    epfd = epoll_create1(0);
    if (epfd < 0)
        goto done;
    memset(&event, 0, sizeof(event));
    event.events = interest;
    event.data.u64 = UINT64_C(0x7801);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)server, &event) != 0 ||
        !WriteFile(client, "r", 1, &written, NULL) || written != 1 ||
        expect_exact_event(epfd, 1000, readable, event.data.u64,
                           "pipe-reconnect-first-data") != 0 ||
        expect_no_event(epfd, 75,
                        "pipe-reconnect-first-data-stable") != 0)
        goto done;
    if (!ReadFile(server, &byte, 1, &readn, NULL) ||
        readn != 1 || byte != 'r')
        goto done;

    CloseHandle(client);
    client = INVALID_HANDLE_VALUE;
    if (expect_exact_event(epfd, 1000, EPOLLHUP, event.data.u64,
                           "pipe-reconnect-first-hup") != 0 ||
        expect_no_event(epfd, 100,
                        "pipe-reconnect-first-hup-stable") != 0)
        goto done;
    if (!DisconnectNamedPipe(server)) {
        fprintf(stderr, "pipe-reconnect: disconnect error=%lu\n",
                (unsigned long)GetLastError());
        goto done;
    }
    if (ConnectNamedPipe(server, NULL)) {
        fputs("pipe-reconnect: second empty connect succeeded\n", stderr);
        goto done;
    }
    error = GetLastError();
    if (error != ERROR_PIPE_LISTENING) {
        fprintf(stderr, "pipe-reconnect: second listen error=%lu\n",
                (unsigned long)error);
        goto done;
    }
    client = CreateFileW(name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0,
                         NULL);
    if (client == INVALID_HANDLE_VALUE ||
        !WriteFile(client, "s", 1, &written, NULL) || written != 1)
        goto done;

    if (expect_exact_event(epfd, 1000, readable, event.data.u64,
                           "pipe-reconnect-reused-data") != 0 ||
        expect_no_event(epfd, 75,
                        "pipe-reconnect-reused-stable") != 0)
        goto done;
    result = 0;

done:
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = 1;
    if (client != INVALID_HANDLE_VALUE)
        CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE)
        CloseHandle(server);
    if (result == 0)
        puts("pipe-named-et-reconnect: OK");
    return result;
}

static int test_named_pipe_duplex_et_mixed(void)
{
    const uint32_t interest =
        EPOLLIN | EPOLLRDNORM | EPOLLOUT | EPOLLWRNORM | EPOLLET;
    const uint32_t readable = EPOLLIN | EPOLLRDNORM;
    const uint32_t writable = EPOLLOUT | EPOLLWRNORM;
    HANDLE server = INVALID_HANDLE_VALUE;
    HANDLE client = INVALID_HANDLE_VALUE;
    struct epoll_event event;
    char byte;
    DWORD written = 0;
    DWORD readn = 0;
    int epfd = -1;
    int result = 1;

    if (create_nonblocking_duplex_pipe(&server, &client) != 0)
        goto done;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto done;

    memset(&event, 0, sizeof(event));
    event.events = interest;
    event.data.u64 = UINT64_C(0x7901);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)server, &event) != 0 ||
        expect_exact_event(epfd, 1000, writable, event.data.u64,
                           "pipe-duplex-et-initial-out") != 0 ||
        expect_no_event(epfd, 75,
                        "pipe-duplex-et-initial-out-stable") != 0)
        goto done;

    if (!WriteFile(client, "m", 1, &written, NULL) || written != 1 ||
        expect_exact_event(epfd, 1000, readable, event.data.u64,
                           "pipe-duplex-et-read-rise") != 0 ||
        expect_no_event(epfd, 75,
                        "pipe-duplex-et-read-rise-stable") != 0)
        goto done;

    if (!ReadFile(server, &byte, 1, &readn, NULL) ||
        readn != 1 || byte != 'm' ||
        expect_no_event(epfd, 100,
                        "pipe-duplex-et-read-fall") != 0)
        goto done;

    written = 0;
    if (!WriteFile(client, "n", 1, &written, NULL) || written != 1 ||
        expect_exact_event(epfd, 1000, readable, event.data.u64,
                           "pipe-duplex-et-read-rerise") != 0 ||
        expect_no_event(epfd, 75,
                        "pipe-duplex-et-read-rerise-stable") != 0)
        goto done;
    result = 0;

done:
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = 1;
    if (client != INVALID_HANDLE_VALUE)
        CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE)
        CloseHandle(server);
    if (result == 0)
        puts("pipe-named-duplex-et-mixed: OK");
    return result;
}

static int test_pipe_pending_mod(void)
{
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    struct epoll_event event;
    struct epoll_event output;
    DWORD written = 0;
    int epfd = -1;
    int n;

    if (!CreatePipe(&rd, &wr, NULL, 0))
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = 10;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)rd, &event) != 0 ||
        epoll_wait(epfd, &output, 1, 0) != 0)
        goto fail;

    event.events = EPOLLIN | EPOLLONESHOT;
    event.data.u64 = 11;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, (epoll_fd_t)rd, &event) != 0)
        goto fail;
    if (!WriteFile(wr, "m", 1, &written, NULL) || written != 1)
        goto fail;
    n = epoll_wait(epfd, &output, 1, 1000);
    if (n != 1 || (output.events & EPOLLIN) == 0 ||
        output.data.u64 != 11) {
        fprintf(stderr,
                "pipe-mod: n=%d events=%u data=%llu errno=%d\n",
                n, n > 0 ? output.events : 0,
                (unsigned long long)(n > 0 ? output.data.u64 : 0), errno);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(rd);
    CloseHandle(wr);
    puts("pipe-mod: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return 1;
}

static int test_pipe_exclusive_rejected(void)
{
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    struct epoll_event event;
    int epfd = -1;

    if (!CreatePipe(&rd, &wr, NULL, 0))
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLEXCLUSIVE;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)rd, &event) != -1 ||
        errno != EINVAL) {
        fprintf(stderr, "pipe-exclusive: expected EINVAL errno=%d\n", errno);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(rd);
    CloseHandle(wr);
    puts("pipe-exclusive: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return 1;
}

static int run_pipe_cancel_iteration(int let_timer_fire)
{
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    struct epoll_event event;
    uint32_t events = 0;
    int epfd = -1;
    int result = 1;

    if (!CreatePipe(&rd, &wr, NULL, 0))
        goto done;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto done;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)rd, &event) != 0)
        goto done;
    if (wait_one(epfd, 0, &events) != 0)
        goto done;
    if (let_timer_fire)
        Sleep(2);
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, (epoll_fd_t)rd, NULL) != 0)
        goto done;
    result = 0;

done:
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = 1;
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    return result;
}

static int test_pipe_cancel_lifecycle(void)
{
    DWORD before = 0;
    DWORD after = 0;
    int i;

    for (i = 0; i < 8; i++) {
        if (run_pipe_cancel_iteration(i & 1) != 0)
            return 1;
    }
    if (!GetProcessHandleCount(GetCurrentProcess(), &before))
        return 1;
    for (i = 0; i < 300; i++) {
        if (run_pipe_cancel_iteration(i & 1) != 0) {
            fprintf(stderr, "pipe-cancel: iteration %d failed errno=%d\n",
                    i, errno);
            return 1;
        }
    }
    Sleep(20);
    if (!GetProcessHandleCount(GetCurrentProcess(), &after))
        return 1;
    if (after > before + 16U) {
        fprintf(stderr, "pipe-cancel: handle growth before=%lu after=%lu\n",
                (unsigned long)before, (unsigned long)after);
        return 1;
    }
    puts("pipe-cancel: OK");
    return 0;
}

static int test_timer_waitable(void)
{
    HANDLE timer = NULL;
    LARGE_INTEGER due;
    struct epoll_event event;
    uint32_t events = 0;
    int epfd = -1;
    int n;

    timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    if (timer == NULL) {
        fputs("timer: CreateWaitableTimer failed\n", stderr);
        return 1;
    }

    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = 3;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)timer, &event) != 0) {
        fprintf(stderr, "timer: ADD failed errno=%d\n", errno);
        goto fail;
    }

    due.QuadPart = -2000000LL; /* 200ms relative */
    if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        goto fail;

    n = wait_one(epfd, 30, &events);
    if (n != 0) {
        fprintf(stderr, "timer: early fire n=%d\n", n);
        goto fail;
    }

    n = wait_one(epfd, 1000, &events);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr, "timer: fire missing n=%d events=%u\n", n, events);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(timer);
    puts("timer-waitable: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (timer) CloseHandle(timer);
    return 1;
}

static int test_disk_file_rejected(void)
{
    char path[MAX_PATH];
    HANDLE file = INVALID_HANDLE_VALUE;
    struct epoll_event event;
    int epfd = -1;
    DWORD n;

    n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n >= MAX_PATH)
        return 1;
    if (n + 12 >= MAX_PATH)
        return 1;
    memcpy(path + n, "wepoll.tmp", 11);

    file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                       NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 1;

    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (epoll_fd_t)file, &event) != -1 ||
        errno != EPERM) {
        fprintf(stderr, "disk file: expected EPERM errno=%d\n", errno);
        goto fail;
    }

    (void)wepoll_close(epfd);
    CloseHandle(file);
    puts("disk-file: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return 1;
}

static int run_mode(const char *mode)
{
    if (strcmp(mode, "readable") == 0)
        return test_pipe_readable();
    if (strcmp(mode, "et") == 0)
        return test_pipe_et();
    if (strcmp(mode, "writable") == 0)
        return test_pipe_writable();
    if (strcmp(mode, "direction") == 0)
        return test_pipe_direction();
    if (strcmp(mode, "oneshot") == 0)
        return test_pipe_oneshot();
    if (strcmp(mode, "named") == 0)
        return test_named_pipe_readable();
    if (strcmp(mode, "hup") == 0)
        return test_pipe_hup();
    if (strcmp(mode, "exact-eof") == 0)
        return test_pipe_exact_eof();
    if (strcmp(mode, "aliases") == 0)
        return test_pipe_alias_masks();
    if (strcmp(mode, "broken-writer") == 0)
        return test_pipe_broken_writer();
    if (strcmp(mode, "broken-writer-et") == 0)
        return test_pipe_broken_writer_et();
    if (strcmp(mode, "terminal-oneshot") == 0)
        return test_pipe_terminal_oneshot();
    if (strcmp(mode, "terminal-oneshot-rearm") == 0)
        return test_pipe_terminal_oneshot_rearm();
    if (strcmp(mode, "et-hup") == 0)
        return test_pipe_et_hup_transitions();
    if (strcmp(mode, "write-lt") == 0)
        return test_pipe_write_backpressure_lt();
    if (strcmp(mode, "write-et") == 0)
        return test_pipe_write_backpressure_et();
    if (strcmp(mode, "named-overlapped") == 0)
        return test_named_pipe_overlapped_exact();
    if (strcmp(mode, "named-outbound-fallback") == 0)
        return test_named_pipe_outbound_fallback();
    if (strcmp(mode, "named-reconnect-et") == 0)
        return test_named_pipe_et_reconnect();
    if (strcmp(mode, "named-duplex-et-mixed") == 0)
        return test_named_pipe_duplex_et_mixed();
    if (strcmp(mode, "mod") == 0)
        return test_pipe_pending_mod();
    if (strcmp(mode, "exclusive") == 0)
        return test_pipe_exclusive_rejected();
    if (strcmp(mode, "cancel") == 0)
        return test_pipe_cancel_lifecycle();
    if (strcmp(mode, "timer") == 0)
        return test_timer_waitable();
    if (strcmp(mode, "disk") == 0)
        return test_disk_file_rejected();
    fprintf(stderr, "unknown pipe mode: %s\n", mode);
    return 2;
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    int failures = 0;
    const char *modes[] = {
        "readable", "et", "writable", "direction", "oneshot", "named",
        "hup", "exact-eof", "aliases", "broken-writer",
        "broken-writer-et", "terminal-oneshot", "terminal-oneshot-rearm",
        "et-hup", "write-lt", "write-et", "named-overlapped",
        "named-outbound-fallback", "named-reconnect-et",
        "named-duplex-et-mixed", "mod", "exclusive", "cancel", "timer",
        "disk"
    };
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
