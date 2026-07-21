/*
 * test_api.c — smoke test for the wepoll-ex public API.
 *
 * These tests verify the contract of the public API surface (return
 * values, errno on failure, basic event delivery) without depending
 * on Windows-specific behaviour.  They run on both POSIX and Windows.
 *
 * On POSIX we're exercising the wrapper layer that forwards to native
 * epoll — so the tests double as portability checks for code that
 * uses wepoll-ex as a portable epoll shim.
 */
#include "wepoll_ex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <assert.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)  do { printf("  [test] %-40s ", name); fflush(stdout); } while (0)
#define PASS()      do { printf("OK\n");   tests_passed++; } while (0)
#define FAIL(why)   do { printf("FAIL: %s (errno=%d %s)\n", why, errno, strerror(errno)); tests_failed++; } while (0)

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

    printf("\n");
    printf("Summary: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
