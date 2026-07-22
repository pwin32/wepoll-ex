/*
 * test_api.c — smoke test for the wepoll-ex public API.
 *
 * These POSIX tests verify the contract of the public API surface
 * (return values, errno on failure, basic event delivery) against
 * native epoll.
 *
 * On POSIX we're exercising the wrapper layer that forwards to native
 * epoll — so the tests double as portability checks for code that
 * uses wepoll-ex as a portable epoll shim.
 */
#define _POSIX_C_SOURCE 200809L

#include "wepoll_ex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/socket.h>
#include <signal.h>
#include <assert.h>

static int tests_passed = 0;
static int tests_failed = 0;
static volatile sig_atomic_t sigusr1_seen = 0;

#define TEST(name)  do { printf("  [test] %-40s ", name); fflush(stdout); } while (0)
#define PASS()      do { printf("OK\n");   tests_passed++; } while (0)
#define FAIL(why)   do { printf("FAIL: %s (errno=%d %s)\n", why, errno, strerror(errno)); tests_failed++; } while (0)

static void sigusr1_handler(int signal_number)
{
    (void)signal_number;
    sigusr1_seen++;
}

/* --------------------------------------------------------------------- */
/* Basic create / close.                                              */
/* --------------------------------------------------------------------- */

static void test_create_close(void)
{
    TEST("epoll_create(1) returns valid fd");
    int fd = epoll_create(1);
    if (fd < 0) { FAIL("epoll_create failed"); return; }
    PASS();

    TEST("epoll_close(fd) succeeds");
    if (wepoll_close(fd) != 0) { FAIL("close failed"); return; }
    PASS();

    TEST("epoll_create1(0) returns valid fd");
    fd = epoll_create1(0);
    if (fd < 0) { FAIL("epoll_create1 failed"); return; }
    PASS();

    TEST("wepoll_close(fd) succeeds");
    if (wepoll_close(fd) != 0) { FAIL("close failed"); return; }
    PASS();
}

/* --------------------------------------------------------------------- */
/* Argument validation.                                              */
/* --------------------------------------------------------------------- */

static void test_invalid_args(void)
{
    TEST("epoll_create(0) returns -1 with EINVAL");
    errno = 0;
    int fd = epoll_create(0);
    if (fd != -1 || errno != EINVAL) { FAIL("expected -1/EINVAL"); wepoll_close(fd); return; }
    PASS();

    TEST("epoll_create1(bad_flag) returns -1 with EINVAL");
    errno = 0;
    fd = epoll_create1(0xDEAD);
    if (fd != -1 || errno != EINVAL) { FAIL("expected -1/EINVAL"); wepoll_close(fd); return; }
    PASS();

    TEST("epoll_ctl on bad epfd returns -1 with EBADF");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) { FAIL("socketpair"); return; }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = pair[0] };
    errno = 0;
    int rc = epoll_ctl(-1, EPOLL_CTL_ADD, pair[0], &ev);
    if (rc != -1 || errno != EBADF) { FAIL("expected -1/EBADF"); close(pair[0]); close(pair[1]); return; }
    PASS();
    close(pair[0]); close(pair[1]);

    TEST("epoll_wait on bad epfd returns -1 with EBADF");
    errno = 0;
    struct epoll_event out[1];
    int n = epoll_wait(-1, out, 1, 0);
    if (n != -1 || errno != EBADF) { FAIL("expected -1/EBADF"); return; }
    PASS();

    TEST("epoll_wait with maxevents=0 returns -1 with EINVAL");
    fd = epoll_create1(0);
    if (fd < 0) { FAIL("epoll_create1"); return; }
    errno = 0;
    n = epoll_wait(fd, out, 0, 0);
    if (n != -1 || errno != EINVAL) { FAIL("expected -1/EINVAL"); wepoll_close(fd); return; }
    PASS();
    wepoll_close(fd);
}

/* --------------------------------------------------------------------- */
/* Add / wait / del cycle.                                          */
/* --------------------------------------------------------------------- */

static void test_basic_event(void)
{
    TEST("basic ADD/WAIT/DEL cycle on socketpair");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) { FAIL("socketpair"); return; }

    int epfd = epoll_create1(0);
    if (epfd < 0) { FAIL("epoll_create1"); close(pair[0]); close(pair[1]); return; }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = pair[0] };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair[0], &ev) != 0) {
        FAIL("epoll_ctl ADD"); goto cleanup;
    }

    /* Write something to trigger EPOLLIN. */
    if (write(pair[1], "x", 1) != 1) { FAIL("write"); goto cleanup; }

    struct epoll_event out[1];
    int n = epoll_wait(epfd, out, 1, 100);
    if (n != 1) { FAIL("expected 1 event"); goto cleanup; }
    if ((out[0].events & EPOLLIN) == 0) { FAIL("expected EPOLLIN"); goto cleanup; }
    if (out[0].data.fd != pair[0]) { FAIL("data mismatch"); goto cleanup; }

    /* DEL should succeed. */
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, pair[0], &ev) != 0) {
        FAIL("epoll_ctl DEL"); goto cleanup;
    }

    /* After DEL, no more events should fire. */
    n = epoll_wait(epfd, out, 1, 0);
    if (n != 0) { FAIL("expected 0 events"); goto cleanup; }

    PASS();
cleanup:
    wepoll_close(epfd);
    close(pair[0]); close(pair[1]);
}

/* --------------------------------------------------------------------- */
/* Double-add should fail with EEXIST.                              */
/* --------------------------------------------------------------------- */

static void test_double_add(void)
{
    TEST("double ADD returns -1 with EEXIST");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) { FAIL("socketpair"); return; }

    int epfd = epoll_create1(0);
    if (epfd < 0) { FAIL("epoll_create1"); close(pair[0]); close(pair[1]); return; }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = pair[0] };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair[0], &ev) != 0) {
        FAIL("first ADD"); goto cleanup;
    }
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair[0], &ev) != -1 || errno != EEXIST) {
        FAIL("expected -1/EEXIST"); goto cleanup;
    }
    PASS();
cleanup:
    wepoll_close(epfd);
    close(pair[0]); close(pair[1]);
}

/* --------------------------------------------------------------------- */
/* DEL on non-registered fd should fail with ENOENT.              */
/* --------------------------------------------------------------------- */

static void test_del_noent(void)
{
    TEST("DEL on unknown fd returns -1 with ENOENT");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) { FAIL("socketpair"); return; }

    int epfd = epoll_create1(0);
    if (epfd < 0) { FAIL("epoll_create1"); close(pair[0]); close(pair[1]); return; }

    struct epoll_event ev;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, pair[0], &ev) != -1 || errno != ENOENT) {
        FAIL("expected -1/ENOENT"); goto cleanup;
    }
    PASS();
cleanup:
    wepoll_close(epfd);
    close(pair[0]); close(pair[1]);
}

/* --------------------------------------------------------------------- */
/* Edge-triggered semantics.                                        */
/* --------------------------------------------------------------------- */

static void test_edge_triggered(void)
{
    TEST("EPOLLET fires once per edge");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) { FAIL("socketpair"); return; }

    int epfd = epoll_create1(0);
    if (epfd < 0) { FAIL("epoll_create1"); close(pair[0]); close(pair[1]); return; }

    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLET,
        .data.fd = pair[0]
    };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair[0], &ev) != 0) {
        FAIL("epoll_ctl ADD"); goto cleanup;
    }

    if (write(pair[1], "x", 1) != 1) { FAIL("write"); goto cleanup; }

    struct epoll_event out[1];
    int n = epoll_wait(epfd, out, 1, 100);
    if (n != 1) { FAIL("expected 1 first time"); goto cleanup; }

    /* Second wait should not return the same event — edge-triggered. */
    n = epoll_wait(epfd, out, 1, 10);
    if (n != 0) { FAIL("expected 0 on second ET wait"); goto cleanup; }

    /* Drain to reset the edge. */
    char buf[1];
    if (read(pair[0], buf, 1) != 1) { FAIL("read"); goto cleanup; }

    /* Write again — should fire once. */
    if (write(pair[1], "y", 1) != 1) { FAIL("write2"); goto cleanup; }
    n = epoll_wait(epfd, out, 1, 100);
    if (n != 1) { FAIL("expected 1 after drain+write"); goto cleanup; }
    PASS();
cleanup:
    wepoll_close(epfd);
    close(pair[0]); close(pair[1]);
}

/* --------------------------------------------------------------------- */
/* Extension API.                                                    */
/* --------------------------------------------------------------------- */

static void test_extension_api(void)
{
    TEST("wepoll_ex_version_string returns non-null");
    const char *v = wepoll_ex_version_string();
    if (v == NULL || v[0] == '\0') { FAIL("null version"); return; }
    PASS();

    TEST("wepoll_ex_version returns non-zero");
    uint32_t vn = wepoll_ex_version();
    if (vn == 0) { FAIL("zero version"); return; }
    PASS();

    TEST("epoll_fd_count returns 0 on fresh instance");
    int epfd = epoll_create1(0);
    if (epfd < 0) { FAIL("epoll_create1"); return; }
    if (epoll_fd_count(epfd) != 0) { FAIL("expected 0"); wepoll_close(epfd); return; }
    PASS();

    TEST("epoll_fd_count reflects registered fds");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) { FAIL("socketpair"); wepoll_close(epfd); return; }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = pair[0] };
    /* Use the extension API so the per-port tracking table is updated. */
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pair[0], &ev, NULL) != 0) { FAIL("ADD"); goto cleanup; }
    if (epoll_fd_count(epfd) != 1) { FAIL("expected 1"); goto cleanup; }
    PASS();
cleanup:
    wepoll_close(epfd);
    close(pair[0]); close(pair[1]);
}

/* --------------------------------------------------------------------- */
/* epoll_ctl_ctx + epoll_wait_ex.                                    */
/* --------------------------------------------------------------------- */

static void test_user_ctx(void)
{
    TEST("epoll_ctl_ctx surfaces user_ctx in epoll_wait_ex");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) { FAIL("socketpair"); return; }

    int epfd = epoll_create1(0);
    if (epfd < 0) { FAIL("epoll_create1"); close(pair[0]); close(pair[1]); return; }

    /* Use a sentinel pointer as user_ctx. */
    int sentinel = 0xC0FFEE;
    void *expected_ctx = &sentinel;

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = pair[0] };
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pair[0], &ev, expected_ctx) != 0) {
        FAIL("ADD"); goto cleanup;
    }

    if (write(pair[1], "x", 1) != 1) { FAIL("write"); goto cleanup; }

    epoll_event_ex out[1];
    int n = epoll_wait_ex(epfd, out, 1, 100);
    if (n != 1) { FAIL("expected 1 event"); goto cleanup; }
    if (out[0].user_ctx != expected_ctx) {
        FAIL("user_ctx mismatch"); goto cleanup;
    }
    PASS();
cleanup:
    wepoll_close(epfd);
    close(pair[0]); close(pair[1]);
}

/* --------------------------------------------------------------------- */
/* Extended argument and timeout validation.                         */
/* --------------------------------------------------------------------- */

static void test_create_ex_and_timeout_validation(void)
{
    TEST("epoll_create_ex validates size/flags and tracks the instance");

    errno = 0;
    int epfd = epoll_create_ex(-1, 0);
    if (epfd != -1 || errno != EINVAL) {
        FAIL("negative size should return EINVAL");
        if (epfd >= 0) wepoll_close(epfd);
        return;
    }

    errno = 0;
    epfd = epoll_create_ex(0, EPOLL_CLOEXEC | 0x40000000);
    if (epfd != -1 || errno != EINVAL) {
        FAIL("unknown flag should return EINVAL");
        if (epfd >= 0) wepoll_close(epfd);
        return;
    }

    epfd = epoll_create_ex(64, EPOLL_CLOEXEC);
    if (epfd < 0) {
        FAIL("epoll_create_ex failed");
        return;
    }
    int fd_flags = fcntl(epfd, F_GETFD);
    if (fd_flags < 0 || !(fd_flags & FD_CLOEXEC)) {
        FAIL("EPOLL_CLOEXEC was not applied");
        wepoll_close(epfd);
        return;
    }
    if (epoll_fd_count(epfd) != 0) {
        FAIL("new epoll_create_ex instance should have zero registrations");
        wepoll_close(epfd);
        return;
    }
    wepoll_close(epfd);
    PASS();

    TEST("zero-timeout waits and sigmask path return immediately");
    epfd = epoll_create_ex(0, 0);
    if (epfd < 0) { FAIL("epoll_create_ex"); return; }
    epoll_event_ex out[1];
    struct timespec zero = { 0, 0 };
    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    int n = epoll_wait_ex(epfd, out, 1, 0);
    if (n != 0) { FAIL("epoll_wait_ex zero timeout"); goto timeout_cleanup; }
    n = epoll_pwait2_ex(epfd, out, 1, &zero, &empty_mask);
    if (n != 0) { FAIL("epoll_pwait2_ex zero timeout"); goto timeout_cleanup; }
    PASS();

    TEST("epoll_pwait2_ex rejects invalid and overflowing timespecs");
    struct timespec invalid = { -1, 0 };
    errno = 0;
    n = epoll_pwait2_ex(epfd, out, 1, &invalid, NULL);
    if (n != -1 || errno != EINVAL) {
        FAIL("negative tv_sec should return EINVAL");
        goto timeout_cleanup;
    }
    invalid.tv_sec = 0;
    invalid.tv_nsec = -1;
    errno = 0;
    n = epoll_pwait2_ex(epfd, out, 1, &invalid, NULL);
    if (n != -1 || errno != EINVAL) {
        FAIL("negative tv_nsec should return EINVAL");
        goto timeout_cleanup;
    }
    invalid.tv_nsec = 1000000000L;
    errno = 0;
    n = epoll_pwait2_ex(epfd, out, 1, &invalid, NULL);
    if (n != -1 || errno != EINVAL) {
        FAIL("large tv_nsec should return EINVAL");
        goto timeout_cleanup;
    }
    invalid.tv_nsec = 0;
    invalid.tv_sec = INT_MAX;
    errno = 0;
    n = epoll_pwait2_ex(epfd, out, 1, &invalid, NULL);
    if (n != -1 || errno != EOVERFLOW) {
        FAIL("overflowing timeout should return EOVERFLOW");
        goto timeout_cleanup;
    }
    PASS();

timeout_cleanup:
    wepoll_close(epfd);
}

/* --------------------------------------------------------------------- */
/* epoll_pwait2_ex must apply the supplied signal mask atomically.       */
/* --------------------------------------------------------------------- */

static void test_sigmask_semantics(void)
{
    TEST("epoll_pwait2_ex atomically applies the signal mask");

    int epfd = epoll_create_ex(0, 0);
    if (epfd < 0) { FAIL("epoll_create_ex"); return; }

    struct sigaction action;
    struct sigaction old_action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = sigusr1_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, &old_action) != 0) {
        FAIL("sigaction");
        wepoll_close(epfd);
        return;
    }

    sigset_t blocked;
    sigset_t previous;
    sigset_t wait_mask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigemptyset(&wait_mask);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) != 0) {
        FAIL("sigprocmask block");
        sigaction(SIGUSR1, &old_action, NULL);
        wepoll_close(epfd);
        return;
    }

    sigusr1_seen = 0;
    int raise_result = raise(SIGUSR1);
    struct timespec timeout = { 1, 0 };
    epoll_event_ex out[1];
    errno = 0;
    int n = raise_result == 0
                ? epoll_pwait2_ex(epfd, out, 1, &timeout, &wait_mask)
                : -1;
    int wait_errno = errno;
    int mask_result = sigprocmask(SIG_SETMASK, &previous, NULL);
    int action_result = sigaction(SIGUSR1, &old_action, NULL);

    if (raise_result != 0 || n != -1 || wait_errno != EINTR ||
        sigusr1_seen != 1 || mask_result != 0 || action_result != 0) {
        errno = wait_errno;
        FAIL("pending SIGUSR1 did not interrupt the masked wait");
        wepoll_close(epfd);
        return;
    }

    wepoll_close(epfd);
    PASS();
}

/* --------------------------------------------------------------------- */
/* Context lookup for all epoll_data union forms.                     */
/* --------------------------------------------------------------------- */

static void test_ptr_and_u64_context(void)
{
    TEST("epoll_wait_ex resolves unique data.ptr and data.u64 contexts");
    int pair_ptr[2] = { -1, -1 };
    int pair_u64[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair_ptr) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, pair_u64) != 0) {
        FAIL("socketpair");
        if (pair_ptr[0] >= 0) { close(pair_ptr[0]); close(pair_ptr[1]); }
        if (pair_u64[0] >= 0) { close(pair_u64[0]); close(pair_u64[1]); }
        return;
    }

    int epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        close(pair_ptr[0]); close(pair_ptr[1]);
        close(pair_u64[0]); close(pair_u64[1]);
        return;
    }

    int ptr_data;
    int ptr_ctx;
    int u64_ctx;
    const uint64_t u64_data = UINT64_C(0x1122334455667788);
    struct epoll_event ptr_ev = { .events = EPOLLIN };
    struct epoll_event u64_ev = { .events = EPOLLIN };
    ptr_ev.data.ptr = &ptr_data;
    u64_ev.data.u64 = u64_data;
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pair_ptr[0], &ptr_ev,
                      &ptr_ctx) != 0 ||
        epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pair_u64[0], &u64_ev,
                      &u64_ctx) != 0) {
        FAIL("epoll_ctl_ctx ADD");
        goto ctx_cleanup;
    }
    if (write(pair_ptr[1], "p", 1) != 1 ||
        write(pair_u64[1], "u", 1) != 1) {
        FAIL("write");
        goto ctx_cleanup;
    }

    epoll_event_ex out[2];
    int n = epoll_wait_ex(epfd, out, 2, 100);
    if (n != 2) {
        FAIL("expected both events");
        goto ctx_cleanup;
    }
    int saw_ptr = 0;
    int saw_u64 = 0;
    for (int i = 0; i < n; i++) {
        if (out[i].data.ptr == &ptr_data) {
            if (out[i].user_ctx != &ptr_ctx) {
                FAIL("data.ptr context mismatch");
                goto ctx_cleanup;
            }
            saw_ptr = 1;
        } else if (out[i].data.u64 == u64_data) {
            if (out[i].user_ctx != &u64_ctx) {
                FAIL("data.u64 context mismatch");
                goto ctx_cleanup;
            }
            saw_u64 = 1;
        } else {
            FAIL("unexpected event data");
            goto ctx_cleanup;
        }
    }
    if (!saw_ptr || !saw_u64) {
        FAIL("one event data form was not returned");
        goto ctx_cleanup;
    }
    PASS();

ctx_cleanup:
    wepoll_close(epfd);
    close(pair_ptr[0]); close(pair_ptr[1]);
    close(pair_u64[0]); close(pair_u64[1]);
}

static void test_duplicate_data_context(void)
{
    TEST("duplicate epoll_data values return an unambiguous NULL context");

    int pairs[2][2] = { { -1, -1 }, { -1, -1 } };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[0]) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[1]) != 0) {
        FAIL("socketpair");
        if (pairs[0][0] >= 0) { close(pairs[0][0]); close(pairs[0][1]); }
        if (pairs[1][0] >= 0) { close(pairs[1][0]); close(pairs[1][1]); }
        return;
    }

    int epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        goto duplicate_socket_cleanup;
    }

    int contexts[2];
    const uint64_t duplicate_data = UINT64_C(0x123456789abcdef0);
    struct epoll_event event = { .events = EPOLLIN };
    event.data.u64 = duplicate_data;
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pairs[0][0], &event,
                      &contexts[0]) != 0 ||
        epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pairs[1][0], &event,
                      &contexts[1]) != 0 ||
        write(pairs[0][1], "a", 1) != 1 ||
        write(pairs[1][1], "b", 1) != 1) {
        FAIL("ADD/write");
        goto duplicate_cleanup;
    }

    epoll_event_ex out[2];
    int n = epoll_wait_ex(epfd, out, 2, 100);
    if (n != 2) {
        FAIL("expected both duplicate-data events");
        goto duplicate_cleanup;
    }
    for (int i = 0; i < n; i++) {
        if (out[i].data.u64 != duplicate_data || out[i].user_ctx != NULL) {
            FAIL("ambiguous event was assigned the wrong context");
            goto duplicate_cleanup;
        }
    }

    if (read(pairs[0][0], &(char){0}, 1) != 1 ||
        read(pairs[1][0], &(char){0}, 1) != 1 ||
        epoll_ctl_ctx(epfd, EPOLL_CTL_DEL, pairs[0][0], NULL, NULL) != 0 ||
        write(pairs[1][1], "c", 1) != 1) {
        FAIL("resolve duplicate setup");
        goto duplicate_cleanup;
    }
    n = epoll_wait_ex(epfd, out, 1, 100);
    if (n != 1 || out[0].user_ctx != &contexts[1]) {
        FAIL("unique remaining payload did not recover its context");
        goto duplicate_cleanup;
    }
    PASS();

duplicate_cleanup:
    wepoll_close(epfd);
duplicate_socket_cleanup:
    close(pairs[0][0]); close(pairs[0][1]);
    close(pairs[1][0]); close(pairs[1][1]);
}

/* --------------------------------------------------------------------- */
/* MOD context updates and clearing.                                  */
/* --------------------------------------------------------------------- */

static void test_mod_context_and_data(void)
{
    TEST("EPOLL_CTL_MOD updates and clears user_ctx");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        FAIL("socketpair");
        return;
    }
    int epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        close(pair[0]); close(pair[1]);
        return;
    }

    int data_a;
    int ctx_a;
    int ctx_b;
    const uint64_t data_b = UINT64_C(0xaabbccddeeff0011);
    struct epoll_event ev = { .events = EPOLLIN };
    ev.data.ptr = &data_a;
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pair[0], &ev, &ctx_a) != 0 ||
        write(pair[1], "a", 1) != 1) {
        FAIL("ADD/write");
        goto mod_cleanup;
    }
    epoll_event_ex out[1];
    int n = epoll_wait_ex(epfd, out, 1, 100);
    if (n != 1 || out[0].user_ctx != &ctx_a ||
        out[0].data.ptr != &data_a) {
        FAIL("initial context/data mismatch");
        goto mod_cleanup;
    }
    read(pair[0], &(char){0}, 1);

    ev.events = EPOLLIN;
    ev.data.u64 = data_b;
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_MOD, pair[0], &ev, &ctx_b) != 0 ||
        write(pair[1], "b", 1) != 1) {
        FAIL("MOD/write");
        goto mod_cleanup;
    }
    n = epoll_wait_ex(epfd, out, 1, 100);
    if (n != 1 || out[0].user_ctx != &ctx_b ||
        out[0].data.u64 != data_b) {
        FAIL("updated context/data mismatch");
        goto mod_cleanup;
    }
    read(pair[0], &(char){0}, 1);

    if (epoll_ctl_ctx(epfd, EPOLL_CTL_MOD, pair[0], &ev, NULL) != 0 ||
        write(pair[1], "c", 1) != 1) {
        FAIL("MOD clear/write");
        goto mod_cleanup;
    }
    n = epoll_wait_ex(epfd, out, 1, 100);
    if (n != 1 || out[0].user_ctx != NULL || out[0].data.u64 != data_b) {
        FAIL("cleared context/data mismatch");
        goto mod_cleanup;
    }
    PASS();

mod_cleanup:
    wepoll_close(epfd);
    close(pair[0]); close(pair[1]);
}

static void test_mod_adopts_native_registration(void)
{
    TEST("EPOLL_CTL_MOD adopts a native registration into metadata");

    int pair[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        FAIL("socketpair");
        return;
    }
    int epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        goto native_mod_socket_cleanup;
    }

    struct epoll_event event = {
        .events = EPOLLIN,
        .data.u64 = UINT64_C(0x1020304050607080)
    };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair[0], &event) != 0) {
        FAIL("native ADD");
        goto native_mod_cleanup;
    }

    int context;
    event.data.u64 = UINT64_C(0x8070605040302010);
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_MOD, pair[0], &event, &context) != 0 ||
        epoll_fd_count(epfd) != 1 || write(pair[1], "m", 1) != 1) {
        FAIL("extension MOD/count/write");
        goto native_mod_cleanup;
    }

    epoll_event_ex out[1];
    int n = epoll_wait_ex(epfd, out, 1, 100);
    if (n != 1 || out[0].data.u64 != event.data.u64 ||
        out[0].user_ctx != &context) {
        FAIL("adopted registration metadata mismatch");
        goto native_mod_cleanup;
    }
    PASS();

native_mod_cleanup:
    wepoll_close(epfd);
native_mod_socket_cleanup:
    close(pair[0]); close(pair[1]);
}

/* --------------------------------------------------------------------- */
/* One-shot rearm must preserve the complete registration.             */
/* --------------------------------------------------------------------- */

static void test_rearm_preserves_registration(void)
{
    TEST("epoll_rearm preserves data and re-enables EPOLLONESHOT");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        FAIL("socketpair");
        return;
    }
    int epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        close(pair[0]); close(pair[1]);
        return;
    }

    const uint64_t data = UINT64_C(0xdeadbeefcafebabe);
    int context;
    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLONESHOT,
        .data.u64 = data
    };
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pair[0], &ev, &context) != 0 ||
        write(pair[1], "1", 1) != 1) {
        FAIL("ADD/write");
        goto rearm_cleanup;
    }

    epoll_event_ex out[1];
    int n = epoll_wait_ex(epfd, out, 1, 100);
    if (n != 1 || out[0].data.u64 != data ||
        out[0].user_ctx != &context ||
        !(out[0].flags & WEPOLL_FLAG_ONESHOT_FIRED)) {
        FAIL("first oneshot event mismatch");
        goto rearm_cleanup;
    }
    read(pair[0], &(char){0}, 1);

    if (epoll_rearm(epfd, pair[0]) != 0 || write(pair[1], "2", 1) != 1) {
        FAIL("rearm/write");
        goto rearm_cleanup;
    }
    n = epoll_wait_ex(epfd, out, 1, 100);
    if (n != 1 || out[0].data.u64 != data || out[0].user_ctx != &context) {
        FAIL("rearmed event did not preserve registration");
        goto rearm_cleanup;
    }
    PASS();

rearm_cleanup:
    wepoll_close(epfd);
    close(pair[0]); close(pair[1]);
}

/* --------------------------------------------------------------------- */
/* Drain and batch operations.                                        */
/* --------------------------------------------------------------------- */

static void test_drain_and_batch(void)
{
    TEST("epoll_drain returns ready events without blocking");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        FAIL("socketpair");
        return;
    }
    int epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        close(pair[0]); close(pair[1]);
        return;
    }
    struct epoll_event ev = { .events = EPOLLIN };
    ev.data.u64 = UINT64_C(0x55);
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pair[0], &ev, NULL) != 0 ||
        write(pair[1], "d", 1) != 1) {
        FAIL("ADD/write");
        goto drain_cleanup;
    }
    struct epoll_event out[1];
    int n = epoll_drain(epfd, out, 1);
    if (n != 1 || out[0].data.u64 != ev.data.u64 ||
        !(out[0].events & EPOLLIN)) {
        FAIL("drain result mismatch");
        goto drain_cleanup;
    }
    read(pair[0], &(char){0}, 1);
    n = epoll_drain(epfd, out, 1);
    if (n != 0) {
        FAIL("empty drain should return zero");
        goto drain_cleanup;
    }
    PASS();

drain_cleanup:
    wepoll_close(epfd);
    close(pair[0]); close(pair[1]);

    TEST("epoll_ctl_batch validates arguments and rolls back ADDs");
    int pairs[2][2] = { { -1, -1 }, { -1, -1 } };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[0]) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[1]) != 0) {
        FAIL("socketpair");
        if (pairs[0][0] >= 0) { close(pairs[0][0]); close(pairs[0][1]); }
        if (pairs[1][0] >= 0) { close(pairs[1][0]); close(pairs[1][1]); }
        return;
    }
    epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        goto batch_socket_cleanup;
    }
    int ops[2] = { EPOLL_CTL_ADD, EPOLL_CTL_ADD };
    int fds[2] = { pairs[0][0], pairs[1][0] };
    struct epoll_event events[2] = {
        { .events = EPOLLIN, .data.fd = pairs[0][0] },
        { .events = EPOLLIN, .data.fd = pairs[1][0] }
    };
    if (epoll_ctl_batch(epfd, ops, fds, events, 2) != 0 ||
        epoll_fd_count(epfd) != 2) {
        FAIL("batch success/count");
        goto batch_cleanup;
    }
    if (write(pairs[0][1], "a", 1) != 1 ||
        write(pairs[1][1], "b", 1) != 1) {
        FAIL("batch write");
        goto batch_cleanup;
    }
    epoll_event_ex ex_out[2];
    int ex_n = epoll_wait_ex(epfd, ex_out, 2, 100);
    if (ex_n != 2) {
        FAIL("batch registrations did not deliver");
        goto batch_cleanup;
    }
    read(pairs[0][0], &(char){0}, 1);
    read(pairs[1][0], &(char){0}, 1);

    int duplicate_ops[2] = { EPOLL_CTL_ADD, EPOLL_CTL_ADD };
    int duplicate_fds[2] = { pairs[0][0], pairs[0][0] };
    struct epoll_event duplicate_events[2] = {
        { .events = EPOLLIN, .data.fd = pairs[0][0] },
        { .events = EPOLLIN, .data.fd = pairs[0][0] }
    };
    /* Remove the successful setup before testing rollback. */
    epoll_ctl_ctx(epfd, EPOLL_CTL_DEL, pairs[0][0], NULL, NULL);
    epoll_ctl_ctx(epfd, EPOLL_CTL_DEL, pairs[1][0], NULL, NULL);
    errno = 0;
    if (epoll_ctl_batch(epfd, duplicate_ops, duplicate_fds,
                        duplicate_events, 2) != -1 || errno != EEXIST ||
        epoll_fd_count(epfd) != 0) {
        FAIL("batch failure/errno/rollback");
        goto batch_cleanup;
    }
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pairs[0][0],
                      &duplicate_events[0], NULL) != 0) {
        FAIL("rolled-back ADD was not removed");
        goto batch_cleanup;
    }
    epoll_ctl_ctx(epfd, EPOLL_CTL_DEL, pairs[0][0], NULL, NULL);

    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pairs[0][0],
                      &events[0], NULL) != 0 ||
        epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pairs[1][0],
                      &events[1], NULL) != 0) {
        FAIL("setup for NULL-event DEL batch");
        goto batch_cleanup;
    }
    int delete_ops[2] = { EPOLL_CTL_DEL, EPOLL_CTL_DEL };
    if (epoll_ctl_batch(epfd, delete_ops, fds, NULL, 2) != 0 ||
        epoll_fd_count(epfd) != 0) {
        FAIL("all-DEL batch should not require events");
        goto batch_cleanup;
    }

    int invalid_op = 99;
    errno = 0;
    if (epoll_ctl_batch(epfd, &invalid_op, fds, NULL, 1) != -1 ||
        errno != EINVAL) {
        FAIL("invalid op with NULL events should return EINVAL");
        goto batch_cleanup;
    }

    errno = 0;
    if (epoll_ctl_batch(epfd, NULL, fds, events, 1) != -1 ||
        errno != EFAULT) {
        FAIL("NULL ops should return EFAULT");
        goto batch_cleanup;
    }
    errno = 0;
    if (epoll_ctl_batch(epfd, ops, NULL, events, 1) != -1 ||
        errno != EFAULT) {
        FAIL("NULL fds should return EFAULT");
        goto batch_cleanup;
    }
    errno = 0;
    if (epoll_ctl_batch(epfd, ops, fds, NULL, 1) != -1 ||
        errno != EFAULT) {
        FAIL("NULL events should return EFAULT");
        goto batch_cleanup;
    }
    errno = 0;
    if (epoll_ctl_batch(epfd, ops, fds, events, 0) != -1 ||
        errno != EINVAL) {
        FAIL("zero count should return EINVAL");
        goto batch_cleanup;
    }
    PASS();

batch_cleanup:
    wepoll_close(epfd);
batch_socket_cleanup:
    if (pairs[0][0] >= 0) { close(pairs[0][0]); close(pairs[0][1]); }
    if (pairs[1][0] >= 0) { close(pairs[1][0]); close(pairs[1][1]); }
}

/* --------------------------------------------------------------------- */
/* Invalid operations must not create metadata; closed descriptors      */
/* retain EBADF consistently.                                          */
/* --------------------------------------------------------------------- */

static void test_invalid_and_closed_descriptors(void)
{
    TEST("invalid extension operations do not create registrations");
    int epfd = epoll_create1(0);
    if (epfd < 0) { FAIL("epoll_create1"); return; }
    struct epoll_event ev = { .events = EPOLLIN };
    errno = 0;
    if (epoll_ctl_ctx(epfd, 99, 0, &ev, NULL) != -1 || errno != EINVAL ||
        epoll_fd_count(epfd) != 0) {
        FAIL("invalid operation changed metadata");
        wepoll_close(epfd);
        return;
    }
    errno = 0;
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, -1, &ev, NULL) != -1 ||
        errno != EBADF || epoll_fd_count(epfd) != 0) {
        FAIL("failed ADD changed metadata");
        wepoll_close(epfd);
        return;
    }
    wepoll_close(epfd);
    PASS();

    TEST("open non-epoll descriptors report EINVAL");
    int pipe_fds[2] = { -1, -1 };
    if (pipe(pipe_fds) != 0) {
        FAIL("pipe");
        return;
    }
    errno = 0;
    if (epoll_fd_count(pipe_fds[0]) != -1 || errno != EINVAL) {
        FAIL("fd_count(non-epoll)");
        close(pipe_fds[0]); close(pipe_fds[1]);
        return;
    }
    errno = 0;
    if (epoll_rearm(pipe_fds[0], pipe_fds[1]) != -1 || errno != EINVAL) {
        FAIL("rearm(non-epoll)");
        close(pipe_fds[0]); close(pipe_fds[1]);
        return;
    }
    close(pipe_fds[0]); close(pipe_fds[1]);
    PASS();

    TEST("closed and invalid descriptors report EBADF");
    errno = 0;
    if (epoll_fd_count(-1) != -1 || errno != EBADF) {
        FAIL("fd_count(-1)");
        return;
    }
    errno = 0;
    if (epoll_rearm(-1, 0) != -1 || errno != EBADF) {
        FAIL("rearm(-1)");
        return;
    }
    epfd = epoll_create_ex(0, 0);
    if (epfd < 0) { FAIL("epoll_create_ex"); return; }
    errno = 0;
    if (epoll_rearm(epfd, -1) != -1 || errno != EBADF) {
        FAIL("rearm(valid epfd, negative fd)");
        wepoll_close(epfd);
        return;
    }
    int closed_target[2] = { -1, -1 };
    if (pipe(closed_target) != 0) {
        FAIL("pipe for closed target");
        wepoll_close(epfd);
        return;
    }
    int closed_fd = closed_target[0];
    close(closed_target[0]);
    closed_target[0] = -1;
    errno = 0;
    if (epoll_rearm(epfd, closed_fd) != -1 || errno != EBADF) {
        FAIL("rearm(valid epfd, closed fd)");
        close(closed_target[1]);
        wepoll_close(epfd);
        return;
    }
    close(closed_target[1]);
    int closed_epfd = epfd;
    if (wepoll_close(epfd) != 0) {
        FAIL("wepoll_close");
        return;
    }
    errno = 0;
    if (epoll_fd_count(closed_epfd) != -1 || errno != EBADF) {
        FAIL("fd_count(closed)");
        return;
    }
    errno = 0;
    if (epoll_rearm(closed_epfd, 0) != -1 || errno != EBADF) {
        FAIL("rearm(closed)");
        return;
    }
    epoll_event_ex out[1];
    errno = 0;
    if (epoll_wait_ex(closed_epfd, out, 1, 0) != -1 || errno != EBADF) {
        FAIL("wait_ex(closed)");
        return;
    }
    errno = 0;
    if (wepoll_close(closed_epfd) != -1 || errno != EBADF) {
        FAIL("second close");
        return;
    }
    PASS();
}

/* --------------------------------------------------------------------- */
/* Closing an instance must not free metadata while control calls use it. */
/* --------------------------------------------------------------------- */

#define CLOSE_STRESS_WORKERS     4
#define CLOSE_STRESS_ITERATIONS  2000

typedef struct close_stress_context {
    int         epfd;
    int         fd;
    atomic_int *ready;
    atomic_int *go;
    atomic_int *abort;
    atomic_int *operations;
    atomic_int *failed;
} close_stress_context_t;

static int wait_atomic_at_least(atomic_int *value, int target,
                                int timeout_ms)
{
    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return -1;

    for (;;) {
        if (atomic_load_explicit(value, memory_order_acquire) >= target) {
            return 0;
        }

        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
        int64_t elapsed_ms = (int64_t)(now.tv_sec - start.tv_sec) * 1000 +
                             (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed_ms >= timeout_ms) {
            errno = ETIMEDOUT;
            return -1;
        }
        sched_yield();
    }
}

static void *close_stress_worker(void *argument)
{
    close_stress_context_t *context = argument;
    atomic_fetch_add_explicit(context->ready, 1, memory_order_release);
    while (!atomic_load_explicit(context->go, memory_order_acquire)) {
        sched_yield();
    }

    for (int i = 0; i < CLOSE_STRESS_ITERATIONS; i++) {
        if (atomic_load_explicit(context->abort, memory_order_relaxed)) break;

        errno = 0;
        int count = epoll_fd_count(context->epfd);
        if (count == -1 && errno == EBADF) break;
        if (count != 1) {
            atomic_store_explicit(context->failed, 1, memory_order_relaxed);
            break;
        }

        errno = 0;
        int result = epoll_rearm(context->epfd, context->fd);
        if (result == -1 && errno == EBADF) break;
        if (result != 0) {
            atomic_store_explicit(context->failed, 1, memory_order_relaxed);
            break;
        }
        atomic_fetch_add_explicit(context->operations, 1,
                                  memory_order_release);
    }
    return NULL;
}

static void test_concurrent_close_and_reuse(void)
{
    TEST("concurrent close/control keeps metadata alive and permits fd reuse");

    int pair[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        FAIL("socketpair");
        return;
    }

    int epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        goto close_stress_socket_cleanup;
    }
    int old_epfd = epfd;
    int old_context;
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLONESHOT,
        .data.u64 = UINT64_C(0x1111222233334444)
    };
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pair[0], &event,
                      &old_context) != 0) {
        FAIL("epoll_ctl_ctx ADD");
        goto close_stress_epoll_cleanup;
    }

    pthread_t threads[CLOSE_STRESS_WORKERS];
    atomic_int ready = 0;
    atomic_int go = 0;
    atomic_int abort = 0;
    atomic_int operations = 0;
    atomic_int failed = 0;
    close_stress_context_t context = {
        .epfd = epfd,
        .fd = pair[0],
        .ready = &ready,
        .go = &go,
        .abort = &abort,
        .operations = &operations,
        .failed = &failed
    };

    int created = 0;
    for (; created < CLOSE_STRESS_WORKERS; created++) {
        int error = pthread_create(&threads[created], NULL,
                                   close_stress_worker, &context);
        if (error != 0) {
            errno = error;
            break;
        }
    }
    if (created != CLOSE_STRESS_WORKERS) {
        atomic_store_explicit(&abort, 1, memory_order_relaxed);
        atomic_store_explicit(&go, 1, memory_order_release);
        for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
        FAIL("pthread_create");
        goto close_stress_epoll_cleanup;
    }

    if (wait_atomic_at_least(&ready, CLOSE_STRESS_WORKERS, 2000) != 0) {
        atomic_store_explicit(&abort, 1, memory_order_relaxed);
        atomic_store_explicit(&go, 1, memory_order_release);
        for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
        FAIL("workers did not start");
        goto close_stress_epoll_cleanup;
    }

    atomic_store_explicit(&go, 1, memory_order_release);
    if (wait_atomic_at_least(&operations, CLOSE_STRESS_WORKERS, 2000) != 0) {
        atomic_store_explicit(&abort, 1, memory_order_relaxed);
        for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
        FAIL("workers did not enter extension calls");
        goto close_stress_epoll_cleanup;
    }

    if (wepoll_close(epfd) != 0) {
        atomic_store_explicit(&abort, 1, memory_order_relaxed);
        for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
        FAIL("concurrent wepoll_close");
        goto close_stress_epoll_cleanup;
    }
    epfd = -1;
    atomic_store_explicit(&abort, 1, memory_order_relaxed);
    for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
    if (atomic_load_explicit(&failed, memory_order_relaxed)) {
        FAIL("worker observed an invalid intermediate result");
        goto close_stress_socket_cleanup;
    }

    /* Reuse the exact integer only after every old user has joined.  The new
     * epoll instance must start with a fresh metadata generation. */
    int new_epfd = epoll_create1(0);
    if (new_epfd < 0) {
        FAIL("epoll_create1 for reuse");
        goto close_stress_socket_cleanup;
    }
    if (new_epfd != old_epfd) {
        if (dup2(new_epfd, old_epfd) != old_epfd) {
            FAIL("dup2 fd reuse");
            close(new_epfd);
            goto close_stress_socket_cleanup;
        }
        close(new_epfd);
        new_epfd = old_epfd;
    }

    int new_context;
    event.data.u64 = UINT64_C(0xaaaabbbbccccdddd);
    if (epoll_fd_count(new_epfd) != 0 ||
        epoll_ctl_ctx(new_epfd, EPOLL_CTL_ADD, pair[0], &event,
                      &new_context) != 0 ||
        write(pair[1], "r", 1) != 1) {
        FAIL("fresh registration after fd reuse");
        wepoll_close(new_epfd);
        goto close_stress_socket_cleanup;
    }
    epoll_event_ex out[1];
    int n = epoll_wait_ex(new_epfd, out, 1, 100);
    if (n != 1 || out[0].data.u64 != event.data.u64 ||
        out[0].user_ctx != &new_context) {
        FAIL("fd reuse surfaced stale metadata");
        wepoll_close(new_epfd);
        goto close_stress_socket_cleanup;
    }
    wepoll_close(new_epfd);
    PASS();
    goto close_stress_socket_cleanup;

close_stress_epoll_cleanup:
    if (epfd >= 0) wepoll_close(epfd);
close_stress_socket_cleanup:
    close(pair[0]); close(pair[1]);
}

/* --------------------------------------------------------------------- */
/* Plain POSIX close must not make metadata follow a reused fd number.  */
/* --------------------------------------------------------------------- */

static void test_native_close_and_reuse(void)
{
    TEST("plain close and fd reuse start a fresh metadata generation");

    int pair[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        FAIL("socketpair");
        return;
    }

    int epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        goto native_close_socket_cleanup;
    }
    int old_epfd = epfd;
    int old_context;
    struct epoll_event event = {
        .events = EPOLLIN,
        .data.u64 = UINT64_C(0x0102030405060708)
    };
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pair[0], &event,
                      &old_context) != 0) {
        FAIL("initial ADD");
        goto native_close_epoll_cleanup;
    }

    if (close(epfd) != 0) {
        FAIL("plain close");
        goto native_close_epoll_cleanup;
    }
    epfd = -1;

    int replacement = epoll_create1(0);
    if (replacement < 0) {
        FAIL("replacement epoll_create1");
        goto native_close_socket_cleanup;
    }
    if (replacement != old_epfd) {
        if (dup2(replacement, old_epfd) != old_epfd) {
            FAIL("dup2 fd reuse");
            close(replacement);
            replacement = -1;
            goto native_close_socket_cleanup;
        }
        close(replacement);
        replacement = old_epfd;
    }

    /* A wait may be the first operation after reuse.  It must not decorate
     * an event from the replacement epoll set with the old context. */
    if (epoll_ctl(replacement, EPOLL_CTL_ADD, pair[0], &event) != 0 ||
        write(pair[1], "o", 1) != 1) {
        FAIL("native replacement ADD/write");
        wepoll_close(replacement);
        replacement = -1;
        goto native_close_socket_cleanup;
    }
    epoll_event_ex stale_out[1];
    int stale_n = epoll_wait_ex(replacement, stale_out, 1, 100);
    if (stale_n != 1 || stale_out[0].user_ctx != NULL ||
        stale_out[0].data.u64 != event.data.u64) {
        FAIL("first replacement wait surfaced stale context");
        wepoll_close(replacement);
        replacement = -1;
        goto native_close_socket_cleanup;
    }
    read(pair[0], &(char){0}, 1);
    if (epoll_ctl(replacement, EPOLL_CTL_DEL, pair[0], NULL) != 0) {
        FAIL("native replacement DEL");
        wepoll_close(replacement);
        replacement = -1;
        goto native_close_socket_cleanup;
    }

    if (epoll_fd_count(replacement) != 0) {
        FAIL("reused epfd retained stale metadata");
        wepoll_close(replacement);
        replacement = -1;
        goto native_close_socket_cleanup;
    }

    int new_context;
    event.data.u64 = UINT64_C(0x1112131415161718);
    if (epoll_ctl_ctx(replacement, EPOLL_CTL_ADD, pair[0], &event,
                      &new_context) != 0 ||
        write(pair[1], "n", 1) != 1) {
        FAIL("replacement ADD/write");
        wepoll_close(replacement);
        replacement = -1;
        goto native_close_socket_cleanup;
    }
    epoll_event_ex out[1];
    int n = epoll_wait_ex(replacement, out, 1, 100);
    if (n != 1 || out[0].data.u64 != event.data.u64 ||
        out[0].user_ctx != &new_context) {
        FAIL("replacement surfaced stale context");
        wepoll_close(replacement);
        replacement = -1;
        goto native_close_socket_cleanup;
    }
    read(pair[0], &(char){0}, 1);
    wepoll_close(replacement);
    PASS();
    goto native_close_socket_cleanup;

native_close_epoll_cleanup:
    if (epfd >= 0) (void)wepoll_close(epfd);
native_close_socket_cleanup:
    close(pair[0]);
    close(pair[1]);
}

/* --------------------------------------------------------------------- */
/* Main.                                                            */
/* --------------------------------------------------------------------- */

int main(void)
{
    printf("wepoll-ex test suite\n");
    printf("====================\n");

    test_create_close();
    test_invalid_args();
    test_basic_event();
    test_double_add();
    test_del_noent();
    test_edge_triggered();
    test_extension_api();
    test_user_ctx();
    test_create_ex_and_timeout_validation();
    test_sigmask_semantics();
    test_ptr_and_u64_context();
    test_duplicate_data_context();
    test_mod_context_and_data();
    test_mod_adopts_native_registration();
    test_rearm_preserves_registration();
    test_drain_and_batch();
    test_invalid_and_closed_descriptors();
    test_concurrent_close_and_reuse();
    test_native_close_and_reuse();

    printf("\n");
    printf("Summary: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
