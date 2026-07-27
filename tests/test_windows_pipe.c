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
        "hup", "mod", "exclusive", "cancel", "timer", "disk"
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
