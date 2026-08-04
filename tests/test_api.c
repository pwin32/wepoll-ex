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
#define _GNU_SOURCE
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
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/utsname.h>
#include <signal.h>
#include <assert.h>

static int tests_passed = 0;
static int tests_failed = 0;
static int tests_skipped = 0;
static volatile sig_atomic_t sigusr1_seen = 0;

#define TEST(name)  do { printf("  [test] %-40s ", name); fflush(stdout); } while (0)
#define PASS()      do { printf("OK\n");   tests_passed++; } while (0)
#define FAIL(why)   do { printf("FAIL: %s (errno=%d %s)\n", why, errno, strerror(errno)); tests_failed++; } while (0)
#define SKIP(why)   do { printf("SKIP: %s\n", why); tests_skipped++; } while (0)

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

static int test_epoll_maxevents(void)
{
    return INT_MAX / (int)sizeof(struct epoll_event);
}

static struct epoll_event_ex *test_map_extended_events(int count,
                                                       size_t *mapped_size)
{
    size_t size = (size_t)count * sizeof(struct epoll_event_ex);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif

    void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (mapping == MAP_FAILED) return NULL;

    *mapped_size = size;
    return mapping;
}

static void test_wait_maxevents_bounds(void)
{
    struct epoll_event event;
    struct epoll_event_ex output;
    struct epoll_event_ex *limit_output = NULL;
    struct epoll_event_ex *large_output = NULL;
    size_t limit_output_size = 0;
    struct timespec zero = { 0, 0 };
    struct timespec invalid = { 0, 1000000000L };
    int pair[2] = { -1, -1 };
    int epfd = -1;
    int limit = test_epoll_maxevents();
    int extended_limit = SIZE_MAX / sizeof(struct epoll_event_ex) <
            (size_t)limit
        ? (int)(SIZE_MAX / sizeof(struct epoll_event_ex)) : limit;
    int extended_over_limit = extended_limit + 1;
    int exact_limit_skipped = 0;
    int result;

    TEST("extended waits enforce Linux maxevents bounds");
    epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        return;
    }

    errno = 0;
    result = epoll_wait_ex(epfd, NULL, 0, 0);
    if (result != -1 || errno != EINVAL) {
        FAIL("zero maxevents must precede NULL events");
        goto cleanup;
    }
    errno = 0;
    result = epoll_wait_ex(epfd, NULL, extended_over_limit, 0);
    if (result != -1 || errno != EINVAL) {
        FAIL("over-limit maxevents must precede NULL events");
        goto cleanup;
    }
    errno = 0;
    result = epoll_wait_ex(epfd, NULL, 1, 0);
    if (result != -1 || errno != EFAULT) {
        FAIL("valid maxevents with NULL events must be EFAULT");
        goto cleanup;
    }
    errno = 0;
    result = epoll_wait_ex(epfd, &output, extended_over_limit, 0);
    if (result != -1 || errno != EINVAL) {
        FAIL("epoll_wait_ex over-limit rejection");
        goto cleanup;
    }
    limit_output = test_map_extended_events(extended_limit,
                                            &limit_output_size);
    if (limit_output == NULL) {
        exact_limit_skipped = 1;
    } else {
        errno = 0;
        result = epoll_wait_ex(epfd, limit_output, extended_limit, 0);
        if (result != 0) {
            FAIL("epoll_wait_ex exact limit should be safe on empty epfd");
            goto cleanup;
        }
    }

    errno = 0;
    result = epoll_pwait2_ex(epfd, NULL, 1, &invalid, NULL);
    if (result != -1 || errno != EINVAL) {
        FAIL("invalid timespec must precede the NULL events check");
        goto cleanup;
    }
    errno = 0;
    result = epoll_pwait2_ex(epfd, NULL, extended_over_limit,
                             &zero, NULL);
    if (result != -1 || errno != EINVAL) {
        FAIL("pwait2_ex over-limit rejection");
        goto cleanup;
    }
    errno = 0;
    result = epoll_pwait2_ex(epfd, NULL, 1, &zero, NULL);
    if (result != -1 || errno != EFAULT) {
        FAIL("pwait2_ex valid maxevents with NULL events");
        goto cleanup;
    }
    if (limit_output != NULL) {
        errno = 0;
        result = epoll_pwait2_ex(epfd, limit_output, extended_limit,
                                 &zero, NULL);
        if (result != 0) {
            FAIL("pwait2_ex exact limit should be safe on empty epfd");
            goto cleanup;
        }
    }

    large_output = calloc(4097, sizeof(*large_output));
    if (large_output == NULL) {
        FAIL("4097-event output allocation");
        goto cleanup;
    }
    result = epoll_wait_ex(epfd, large_output, 4097, 0);
    if (result != 0) {
        FAIL("4097-event epoll_wait_ex request");
        goto cleanup;
    }
    result = epoll_pwait2_ex(epfd, large_output, 4097, &zero, NULL);
    if (result != 0) {
        FAIL("4097-event epoll_pwait2_ex request");
        goto cleanup;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        FAIL("socketpair for large wait request");
        goto cleanup;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = UINT64_C(0x4d41584556454e54);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair[0], &event) != 0 ||
        write(pair[1], "x", 1) != 1) {
        FAIL("prepare 4097-event ready wait");
        goto cleanup;
    }
    result = epoll_wait_ex(epfd, large_output, 4097, 1000);
    if (result != 1 || (large_output[0].events & EPOLLIN) == 0 ||
        large_output[0].data.u64 != event.data.u64) {
        FAIL("4097-event wait did not return readiness");
        goto cleanup;
    }

    if (exact_limit_skipped) {
        printf("(exact-limit mapping unavailable) ");
    }
    PASS();

cleanup:
    if (limit_output != NULL) {
        munmap(limit_output, limit_output_size);
    }
    free(large_output);
    if (pair[0] >= 0) close(pair[0]);
    if (pair[1] >= 0) close(pair[1]);
    if (epfd >= 0) wepoll_close(epfd);
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
/* A bounded wait must rotate through a larger persistent ready set.     */
/* --------------------------------------------------------------------- */

static void test_ready_set_round_robin(void)
{
    enum { READY_COUNT = 4 };
    int read_fds[READY_COUNT];
    int write_fds[READY_COUNT];
    int seen[READY_COUNT] = { 0 };
    int epfd = -1;
    struct epoll_event event;
    struct epoll_event output;

    TEST("successive bounded waits round-robin a larger ready set");
    for (int i = 0; i < READY_COUNT; i++) {
        read_fds[i] = -1;
        write_fds[i] = -1;
    }

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        FAIL("epoll_create1");
        return;
    }
    for (int i = 0; i < READY_COUNT; i++) {
        int pipe_fds[2];
        if (pipe(pipe_fds) != 0) {
            FAIL("pipe");
            goto cleanup;
        }
        read_fds[i] = pipe_fds[0];
        write_fds[i] = pipe_fds[1];
        memset(&event, 0, sizeof(event));
        event.events = EPOLLIN;
        event.data.u32 = (uint32_t)i + 1;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, read_fds[i], &event) != 0 ||
            write(write_fds[i], "r", 1) != 1) {
            FAIL("ADD/write");
            goto cleanup;
        }
    }

    {
        struct epoll_event ready[READY_COUNT];
        int count = epoll_wait(epfd, ready, READY_COUNT, 1000);

        if (count != READY_COUNT) {
            FAIL("ready-set preflight count");
            goto cleanup;
        }
        for (int i = 0; i < count; i++) {
            if (ready[i].events != EPOLLIN || ready[i].data.u32 == 0 ||
                ready[i].data.u32 > (uint32_t)READY_COUNT ||
                seen[ready[i].data.u32 - 1]) {
                FAIL("ready-set preflight event");
                goto cleanup;
            }
            seen[ready[i].data.u32 - 1] = 1;
        }
        memset(seen, 0, sizeof(seen));
    }

    for (int i = 0; i < READY_COUNT; i++) {
        memset(&output, 0, sizeof(output));
        int count = epoll_wait(epfd, &output, 1, 1000);
        if (count != 1 || output.events != EPOLLIN ||
            output.data.u32 == 0 ||
            output.data.u32 > (uint32_t)READY_COUNT ||
            seen[output.data.u32 - 1]) {
            struct utsname host;
            int legacy_wsl1 = count == 1 && output.events == EPOLLIN &&
                output.data.u32 > 0 &&
                output.data.u32 <= (uint32_t)READY_COUNT &&
                seen[output.data.u32 - 1] && uname(&host) == 0 &&
                strncmp(host.release, "4.4.0-", 6) == 0 &&
                strstr(host.release, "Microsoft") != NULL;

            if (legacy_wsl1) {
                SKIP("legacy WSL1 host epoll lacks ready-list rotation");
                goto cleanup;
            }
            fprintf(stderr,
                    "round-robin wait %d: count=%d events=0x%08x "
                    "token=%u seen=%d\n",
                    i, count, count > 0 ? output.events : 0,
                    count > 0 ? output.data.u32 : 0,
                    count > 0 && output.data.u32 > 0 &&
                            output.data.u32 <= (uint32_t)READY_COUNT
                        ? seen[output.data.u32 - 1] : -1);
            FAIL("ready-set rotation");
            goto cleanup;
        }
        seen[output.data.u32 - 1] = 1;
    }
    PASS();

cleanup:
    if (epfd >= 0) wepoll_close(epfd);
    for (int i = 0; i < READY_COUNT; i++) {
        if (read_fds[i] >= 0) close(read_fds[i]);
        if (write_fds[i] >= 0) close(write_fds[i]);
    }
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
/* Undefined normal bits and EPOLLMSG are accepted but never emitted.    */
/* --------------------------------------------------------------------- */

static void test_inert_event_bits(void)
{
    const uint32_t unknown_event = UINT32_C(1) << 11;
    int pair[2];
    int epfd = -1;
    struct epoll_event event;
    struct epoll_event output;
    char byte;
    int count;

    TEST("normal registrations accept and filter inert event bits");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        FAIL("socketpair");
        return;
    }
    epfd = epoll_create1(0);
    if (epfd < 0) {
        FAIL("epoll_create1");
        close(pair[0]);
        close(pair[1]);
        return;
    }

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLMSG | unknown_event;
    event.data.u64 = UINT64_C(0x494e455254);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair[0], &event) != 0 ||
        write(pair[1], "a", 1) != 1) {
        FAIL("ADD/write");
        goto cleanup;
    }
    memset(&output, 0, sizeof(output));
    count = epoll_wait(epfd, &output, 1, 100);
    if (count != 1 || output.events != EPOLLIN ||
        output.data.u64 != event.data.u64 ||
        read(pair[0], &byte, 1) != 1 || byte != 'a') {
        FAIL("inert ADD mask delivery");
        goto cleanup;
    }

    event.events = EPOLLMSG | unknown_event;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, pair[0], &event) != 0 ||
        write(pair[1], "b", 1) != 1 ||
        epoll_wait(epfd, &output, 1, 20) != 0) {
        FAIL("inert-only MOD should stay quiet");
        goto cleanup;
    }

    event.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, pair[0], &event) != 0) {
        FAIL("MOD restore");
        goto cleanup;
    }
    memset(&output, 0, sizeof(output));
    count = epoll_wait(epfd, &output, 1, 100);
    if (count != 1 || output.events != EPOLLIN ||
        output.data.u64 != event.data.u64 ||
        read(pair[0], &byte, 1) != 1 || byte != 'b') {
        FAIL("restored read interest");
        goto cleanup;
    }
    PASS();

cleanup:
    wepoll_close(epfd);
    close(pair[0]);
    close(pair[1]);
}

/* --------------------------------------------------------------------- */
/* Extension API.                                                    */
/* --------------------------------------------------------------------- */

static void test_extension_api(void)
{
    static const char version_prefix[] =
        "wepoll-ex " WEPOLL_EX_VERSION_STRING " ";

    TEST("wepoll_ex_version_string matches the public header");
    const char *v = wepoll_ex_version_string();
    if (v == NULL ||
        strncmp(v, version_prefix, sizeof(version_prefix) - 1) != 0) {
        FAIL("version string mismatch");
        return;
    }
    PASS();

    TEST("wepoll_ex_version matches the public header");
    uint32_t vn = wepoll_ex_version();
    if (vn != WEPOLL_EX_VERSION_NUMBER) {
        FAIL("version number mismatch");
        return;
    }
    PASS();

    TEST("versioned operational statistics report the POSIX contract");
    wepoll_ex_stats stats;
    wepoll_ex_global_stats global_stats;
    uint32_t stats_prefix[2] = { 0, 0 };
    int stats_epfd = epoll_create_ex(0, 0);
    if (stats_epfd < 0 ||
        wepoll_ex_get_socket_lifetime_policy() !=
            WEPOLL_EX_SOCKET_LIFETIME_NOT_APPLICABLE ||
        wepoll_ex_get_stats(stats_epfd, &stats, sizeof(stats)) != 0 ||
        stats.version != WEPOLL_EX_STATS_VERSION ||
        stats.struct_size != sizeof(stats) ||
        stats.socket_lifetime_policy !=
            WEPOLL_EX_SOCKET_LIFETIME_NOT_APPLICABLE ||
        stats.active_registrations != 0 ||
        wepoll_ex_get_global_stats(&global_stats, sizeof(global_stats)) != 0 ||
        global_stats.version != WEPOLL_EX_STATS_VERSION ||
        global_stats.struct_size != sizeof(global_stats) ||
        wepoll_ex_get_stats(stats_epfd, (wepoll_ex_stats *)stats_prefix,
                            sizeof(stats_prefix)) != 0 ||
        stats_prefix[0] != WEPOLL_EX_STATS_VERSION ||
        stats_prefix[1] != sizeof(stats)) {
        FAIL("statistics snapshot");
        if (stats_epfd >= 0) (void)wepoll_close(stats_epfd);
        return;
    }
    errno = 0;
    if (wepoll_ex_get_stats(stats_epfd, &stats,
                            sizeof(uint32_t)) != -1 || errno != EINVAL) {
        FAIL("statistics size validation");
        (void)wepoll_close(stats_epfd);
        return;
    }
    if (wepoll_close(stats_epfd) != 0) {
        FAIL("statistics epfd close");
        return;
    }
    PASS();

    TEST("epoll_fd_count returns 0 on fresh instance");
    int epfd = epoll_create1(0);
    if (epfd < 0) { FAIL("epoll_create1"); return; }
    if (epoll_fd_count(epfd) != 0) { FAIL("expected 0"); wepoll_close(epfd); return; }
    PASS();

    TEST("epoll_fd_count tracks extension-owned registrations");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) { FAIL("socketpair"); wepoll_close(epfd); return; }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = pair[0] };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair[0], &ev) != 0) {
        FAIL("native ADD");
        goto cleanup;
    }
    if (epoll_fd_count(epfd) != 0) {
        FAIL("native-only registration should be outside metadata count");
        goto cleanup;
    }
    /* An extension MOD adopts a native registration into the metadata view. */
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_MOD, pair[0], &ev, NULL) != 0) {
        FAIL("extension MOD");
        goto cleanup;
    }
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

    /* Exercise metadata expansion into a multi-record caller buffer. */
    epoll_event_ex out[64];
    int n = epoll_wait_ex(epfd, out, 64, 100);
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

    epfd = epoll_create_ex(INT_MAX, 0);
    if (epfd < 0 || wepoll_close(epfd) != 0) {
        FAIL("large POSIX capacity hint should be ignored");
        return;
    }
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
    epoll_event_ex large_output[64];
    n = epoll_wait_ex(epfd, large_output, 64, 0);
    if (n != 0) { FAIL("epoll_wait_ex larger output buffer"); goto timeout_cleanup; }
    PASS();

    TEST("sub-millisecond extended wait is accepted and bounded");
    struct timespec submillisecond = { 0, 250000L };
    struct timespec wait_start;
    struct timespec wait_end;
    if (clock_gettime(CLOCK_MONOTONIC, &wait_start) != 0) {
        FAIL("clock_gettime before sub-millisecond wait");
        goto timeout_cleanup;
    }
    n = epoll_pwait2_ex(epfd, out, 1, &submillisecond, NULL);
    int wait_error = errno;
    if (clock_gettime(CLOCK_MONOTONIC, &wait_end) != 0) {
        FAIL("clock_gettime after sub-millisecond wait");
        goto timeout_cleanup;
    }
    int64_t elapsed_ns = (int64_t)(wait_end.tv_sec - wait_start.tv_sec) *
                         INT64_C(1000000000) +
                         (int64_t)(wait_end.tv_nsec - wait_start.tv_nsec);
    if (n != 0 || elapsed_ns < 0 || elapsed_ns > INT64_C(250000000)) {
        errno = wait_error;
        FAIL("sub-millisecond wait result or duration");
        goto timeout_cleanup;
    }
    PASS();

    TEST("extended waits distinguish NULL events from invalid maxevents");
    errno = 0;
    n = epoll_wait_ex(epfd, NULL, 1, 0);
    if (n != -1 || errno != EFAULT) {
        FAIL("epoll_wait_ex NULL events should return EFAULT");
        goto timeout_cleanup;
    }
    errno = 0;
    n = epoll_wait_ex(epfd, out, 0, 0);
    if (n != -1 || errno != EINVAL) {
        FAIL("epoll_wait_ex zero maxevents should return EINVAL");
        goto timeout_cleanup;
    }
    errno = 0;
    n = epoll_pwait2_ex(epfd, NULL, 1, &zero, NULL);
    if (n != -1 || errno != EFAULT) {
        FAIL("epoll_pwait2_ex NULL events should return EFAULT");
        goto timeout_cleanup;
    }
    errno = 0;
    n = epoll_pwait2_ex(epfd, out, 0, &zero, NULL);
    if (n != -1 || errno != EINVAL) {
        FAIL("epoll_pwait2_ex zero maxevents should return EINVAL");
        goto timeout_cleanup;
    }
    PASS();

    TEST("epoll_pwait2_ex rejects invalid timespecs");
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
    PASS();

    TEST("very long epoll_pwait2_ex timeout returns ready fd");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        FAIL("socketpair for long timeout");
        goto timeout_cleanup;
    }
    struct epoll_event event = {
        .events = EPOLLIN,
        .data.u64 = UINT64_C(0x1020304050607080)
    };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair[0], &event) != 0 ||
        write(pair[1], "x", 1) != 1) {
        FAIL("prepare ready fd for long timeout");
        close(pair[0]);
        close(pair[1]);
        goto timeout_cleanup;
    }
    struct timespec very_long = { INT_MAX, 0 };
    n = epoll_pwait2_ex(epfd, out, 1, &very_long, NULL);
    int long_wait_error = errno;
    if (n != 1 || (out[0].events & EPOLLIN) == 0 ||
        out[0].data.u64 != event.data.u64) {
        close(pair[0]);
        close(pair[1]);
        errno = long_wait_error;
        FAIL("ready fd was not returned for long timeout");
        goto timeout_cleanup;
    }

    sigset_t caller_mask;
    sigset_t caller_previous;
    sigset_t caller_after;
    sigemptyset(&caller_mask);
    sigaddset(&caller_mask, SIGUSR2);
    if (pthread_sigmask(SIG_SETMASK, &caller_mask, &caller_previous) != 0) {
        close(pair[0]);
        close(pair[1]);
        FAIL("block caller mask for long timeout");
        goto timeout_cleanup;
    }
    n = epoll_pwait2_ex(epfd, out, 1, &very_long, &empty_mask);
    long_wait_error = errno;
    int mask_query_error = pthread_sigmask(SIG_SETMASK, NULL, &caller_after);
    int mask_restore_error = pthread_sigmask(SIG_SETMASK, &caller_previous,
                                             NULL);
    close(pair[0]);
    close(pair[1]);
    if (n != 1 || (out[0].events & EPOLLIN) == 0 ||
        out[0].data.u64 != event.data.u64 || mask_query_error != 0 ||
        mask_restore_error != 0 || sigismember(&caller_after, SIGUSR2) != 1 ||
        sigismember(&caller_after, SIGUSR1) != 0) {
        errno = long_wait_error;
        FAIL("masked long timeout did not restore the caller mask");
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

static void test_mod_updates_duplicate_data_index(void)
{
    TEST("MOD updates duplicate-data context indexing");

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
        goto index_socket_cleanup;
    }

    int contexts[2];
    const uint64_t data_a = UINT64_C(0x1010101010101010);
    const uint64_t data_b = UINT64_C(0x2020202020202020);
    const uint64_t data_c = UINT64_C(0x3030303030303030);
    struct epoll_event events[2] = {
        { .events = EPOLLIN, .data.u64 = data_a },
        { .events = EPOLLIN, .data.u64 = data_b }
    };
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pairs[0][0], &events[0],
                      &contexts[0]) != 0 ||
        epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pairs[1][0], &events[1],
                      &contexts[1]) != 0) {
        FAIL("initial ADD");
        goto index_cleanup;
    }

    events[1].data.u64 = data_a;
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_MOD, pairs[1][0], &events[1],
                      &contexts[1]) != 0 ||
        write(pairs[0][1], "a", 1) != 1 ||
        write(pairs[1][1], "b", 1) != 1) {
        FAIL("MOD into duplicate/write");
        goto index_cleanup;
    }

    epoll_event_ex output[2];
    int count = epoll_wait_ex(epfd, output, 2, 100);
    if (count != 2) {
        FAIL("duplicate wait count");
        goto index_cleanup;
    }
    for (int i = 0; i < count; i++) {
        if (output[i].data.u64 != data_a || output[i].user_ctx != NULL) {
            FAIL("MOD-created duplicate was not ambiguous");
            goto index_cleanup;
        }
    }
    if (read(pairs[0][0], &(char){0}, 1) != 1 ||
        read(pairs[1][0], &(char){0}, 1) != 1) {
        FAIL("read duplicate readiness");
        goto index_cleanup;
    }

    events[1].data.u64 = data_c;
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_MOD, pairs[1][0], &events[1],
                      &contexts[1]) != 0 ||
        write(pairs[0][1], "c", 1) != 1 ||
        write(pairs[1][1], "d", 1) != 1) {
        FAIL("MOD out of duplicate/write");
        goto index_cleanup;
    }

    count = epoll_wait_ex(epfd, output, 2, 100);
    if (count != 2) {
        FAIL("unique wait count");
        goto index_cleanup;
    }
    int saw_a = 0;
    int saw_c = 0;
    for (int i = 0; i < count; i++) {
        if (output[i].data.u64 == data_a &&
            output[i].user_ctx == &contexts[0]) {
            saw_a = 1;
        } else if (output[i].data.u64 == data_c &&
                   output[i].user_ctx == &contexts[1]) {
            saw_c = 1;
        } else {
            FAIL("unique contexts were not restored");
            goto index_cleanup;
        }
    }
    if (!saw_a || !saw_c) {
        FAIL("missing unique indexed event");
        goto index_cleanup;
    }
    PASS();

index_cleanup:
    wepoll_close(epfd);
index_socket_cleanup:
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
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, 0, NULL, NULL) != -1 ||
        errno != EFAULT || epoll_fd_count(epfd) != 0) {
        FAIL("NULL ADD event should return EFAULT");
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

typedef struct posix_wait_thread_context {
    int             epfd;
    atomic_int      started;
    int             result;
    int             error;
    epoll_event_ex  event;
} posix_wait_thread_context_t;

static void *posix_wait_thread(void *opaque)
{
    posix_wait_thread_context_t *context = opaque;

    /* A nonblocking probe ensures the extension metadata port is acquired
     * before the caller is told that the blocking phase is ready. */
    epoll_event_ex probe;
    (void)epoll_wait_ex(context->epfd, &probe, 1, 0);
    atomic_store_explicit(&context->started, 1, memory_order_release);

    errno = 0;
    context->result = epoll_wait_ex(context->epfd, &context->event, 1, -1);
    context->error = errno;
    return NULL;
}

static int sleep_milliseconds(long milliseconds)
{
    struct timespec request = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L
    };
    while (nanosleep(&request, &request) != 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}

typedef struct same_epfd_et_wait_context {
    int                epfd;
    atomic_int        *ready;
    atomic_int        *go;
    struct epoll_event event;
    int                result;
    int                error;
} same_epfd_et_wait_context_t;

static void *same_epfd_et_wait_thread(void *opaque)
{
    same_epfd_et_wait_context_t *context = opaque;

    atomic_fetch_add_explicit(context->ready, 1, memory_order_release);
    while (!atomic_load_explicit(context->go, memory_order_acquire)) {
        sched_yield();
    }
    errno = 0;
    context->result = epoll_wait(context->epfd, &context->event, 1, 500);
    context->error = errno;
    return NULL;
}

static void test_same_epfd_et_wakes_one_waiter(void)
{
    enum { WAITER_COUNT = 2 };
    const uint64_t data = UINT64_C(0x455457414b45);
    int pair[2] = { -1, -1 };
    int epfd = -1;
    struct epoll_event event;
    same_epfd_et_wait_context_t contexts[WAITER_COUNT];
    pthread_t threads[WAITER_COUNT];
    atomic_int ready = 0;
    atomic_int go = 0;
    int created = 0;

    TEST("same-epfd EPOLLET readiness wakes only one waiter");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        FAIL("socketpair");
        return;
    }
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        FAIL("epoll_create1");
        goto cleanup;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET;
    event.data.u64 = data;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pair[0], &event) != 0) {
        FAIL("EPOLL_CTL_ADD");
        goto cleanup;
    }

    memset(contexts, 0, sizeof(contexts));
    for (; created < WAITER_COUNT; created++) {
        contexts[created].epfd = epfd;
        contexts[created].ready = &ready;
        contexts[created].go = &go;
        contexts[created].result = -2;
        int thread_error = pthread_create(&threads[created], NULL,
                                          same_epfd_et_wait_thread,
                                          &contexts[created]);
        if (thread_error != 0) {
            errno = thread_error;
            break;
        }
    }
    if (created != WAITER_COUNT) {
        atomic_store_explicit(&go, 1, memory_order_release);
        for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
        FAIL("pthread_create");
        goto cleanup;
    }
    if (wait_atomic_at_least(&ready, WAITER_COUNT, 2000) != 0) {
        atomic_store_explicit(&go, 1, memory_order_release);
        for (int i = 0; i < WAITER_COUNT; i++) {
            pthread_join(threads[i], NULL);
        }
        FAIL("waiters did not start");
        goto cleanup;
    }
    atomic_store_explicit(&go, 1, memory_order_release);
    if (sleep_milliseconds(50) != 0 || write(pair[1], "e", 1) != 1) {
        for (int i = 0; i < WAITER_COUNT; i++) {
            pthread_join(threads[i], NULL);
        }
        FAIL("trigger readiness");
        goto cleanup;
    }
    for (int i = 0; i < WAITER_COUNT; i++) pthread_join(threads[i], NULL);

    int winners = 0;
    for (int i = 0; i < WAITER_COUNT; i++) {
        if (contexts[i].result == 1) {
            if (contexts[i].event.events != EPOLLIN ||
                contexts[i].event.data.u64 != data) {
                errno = contexts[i].error;
                FAIL("winner event mismatch");
                goto cleanup;
            }
            winners++;
        } else if (contexts[i].result != 0) {
            errno = contexts[i].error;
            FAIL("waiter result");
            goto cleanup;
        }
    }
    if (winners != 1) {
        FAIL("expected exactly one woken waiter");
        goto cleanup;
    }
    PASS();

cleanup:
    if (epfd >= 0) wepoll_close(epfd);
    if (pair[0] >= 0) close(pair[0]);
    if (pair[1] >= 0) close(pair[1]);
}

typedef struct cancel_wait_context {
    int        epfd;
    atomic_int started;
    atomic_int cleanup_seen;
    atomic_int mask_restored;
    int        setup_error;
} cancel_wait_context_t;

static void cancel_wait_mask_cleanup(void *opaque)
{
    cancel_wait_context_t *context = opaque;
    sigset_t current;
    int mask_error = pthread_sigmask(SIG_SETMASK, NULL, &current);
    int restored = mask_error == 0 &&
                   sigismember(&current, SIGUSR2) == 1 &&
                   sigismember(&current, SIGUSR1) == 0;

    atomic_store_explicit(&context->mask_restored, restored,
                          memory_order_release);
    atomic_store_explicit(&context->cleanup_seen, 1, memory_order_release);
}

static void *cancel_wait_thread(void *opaque)
{
    cancel_wait_context_t *context = opaque;
    epoll_event_ex events[64];
    struct timespec zero = { 0, 0 };
    struct timespec very_long = { INT_MAX, 0 };
    sigset_t caller_mask;
    sigset_t wait_mask;

    sigemptyset(&caller_mask);
    sigaddset(&caller_mask, SIGUSR2);
    sigemptyset(&wait_mask);
    context->setup_error = pthread_sigmask(SIG_SETMASK, &caller_mask, NULL);
    if (context->setup_error != 0) {
        atomic_store_explicit(&context->started, 1, memory_order_release);
        return context;
    }

    /* Create the metadata port before publishing readiness.  Cancelling the
     * long masked wait must release its port reference and restore the caller
     * mask; a forced fallback also exercises the multi-chunk signal bridge. */
    (void)epoll_pwait2_ex(context->epfd, events, 64, &zero, NULL);
    pthread_cleanup_push(cancel_wait_mask_cleanup, context);
    atomic_store_explicit(&context->started, 1, memory_order_release);
    (void)epoll_pwait2_ex(context->epfd, events, 64,
                         &very_long, &wait_mask);
    pthread_cleanup_pop(0);
    return context;
}

static void test_cancel_blocking_extended_wait(void)
{
    TEST("cancelling a blocking extended wait releases resources");

    int epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        return;
    }

    cancel_wait_context_t context = {
        .epfd = epfd,
        .started = ATOMIC_VAR_INIT(0),
        .cleanup_seen = ATOMIC_VAR_INIT(0),
        .mask_restored = ATOMIC_VAR_INIT(0),
        .setup_error = 0
    };
    pthread_t thread;
    int thread_error = pthread_create(&thread, NULL, cancel_wait_thread,
                                      &context);
    if (thread_error != 0) {
        errno = thread_error;
        FAIL("pthread_create");
        wepoll_close(epfd);
        return;
    }

    if (wait_atomic_at_least(&context.started, 1, 2000) != 0 ||
        context.setup_error != 0 ||
        sleep_milliseconds(50) != 0) {
        if (context.setup_error != 0) errno = context.setup_error;
        FAIL("cancel waiter did not block");
        (void)wepoll_close(epfd);
        pthread_join(thread, NULL);
        return;
    }

    thread_error = pthread_cancel(thread);
    if (thread_error != 0) {
        errno = thread_error;
        FAIL("pthread_cancel");
        (void)wepoll_close(epfd);
        pthread_join(thread, NULL);
        return;
    }

    void *thread_result = NULL;
    thread_error = pthread_join(thread, &thread_result);
    if (thread_error != 0 || thread_result != PTHREAD_CANCELED) {
        if (thread_error != 0) errno = thread_error;
        FAIL("cancelled wait thread result");
        (void)wepoll_close(epfd);
        return;
    }
    if (atomic_load_explicit(&context.cleanup_seen,
                             memory_order_acquire) != 1 ||
        atomic_load_explicit(&context.mask_restored,
                             memory_order_acquire) != 1) {
        FAIL("cancelled masked wait did not restore the caller mask");
        (void)wepoll_close(epfd);
        return;
    }
    if (wepoll_close(epfd) != 0) {
        FAIL("close after cancelled wait");
        return;
    }
    PASS();
}

static void test_close_wakes_blocking_extended_wait(void)
{
    enum { WAITER_COUNT = 2 };

    TEST("wepoll_close wakes all blocking epoll_wait_ex waiters");

    int epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        return;
    }

    posix_wait_thread_context_t contexts[WAITER_COUNT];
    pthread_t threads[WAITER_COUNT];
    int created = 0;
    for (; created < WAITER_COUNT; created++) {
        contexts[created].epfd = epfd;
        atomic_init(&contexts[created].started, 0);
        contexts[created].result = 0;
        contexts[created].error = 0;
        memset(&contexts[created].event, 0, sizeof(contexts[created].event));

        int create_error = pthread_create(&threads[created], NULL,
                                          posix_wait_thread,
                                          &contexts[created]);
        if (create_error != 0) {
            errno = create_error;
            break;
        }
    }
    if (created != WAITER_COUNT) {
        int saved_errno = errno;
        (void)wepoll_close(epfd);
        for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
        errno = saved_errno;
        FAIL("pthread_create");
        return;
    }

    for (int i = 0; i < WAITER_COUNT; i++) {
        if (wait_atomic_at_least(&contexts[i].started, 1, 2000) != 0) {
            FAIL("blocking waiter did not start");
            (void)wepoll_close(epfd);
            for (int j = 0; j < WAITER_COUNT; j++) {
                pthread_join(threads[j], NULL);
            }
            return;
        }
    }
    if (sleep_milliseconds(100) != 0) {
        FAIL("blocking waiter startup delay");
        (void)wepoll_close(epfd);
        for (int i = 0; i < WAITER_COUNT; i++) {
            pthread_join(threads[i], NULL);
        }
        return;
    }

    errno = 0;
    int close_result = wepoll_close(epfd);
    int close_error = errno;
    for (int i = 0; i < WAITER_COUNT; i++) pthread_join(threads[i], NULL);
    if (close_result != 0) {
        errno = close_error;
        FAIL("wepoll_close");
        return;
    }
    for (int i = 0; i < WAITER_COUNT; i++) {
        if (contexts[i].result != -1 || contexts[i].error != EBADF) {
            errno = contexts[i].error;
            FAIL("one blocking wait did not wake with EBADF");
            return;
        }
    }
    PASS();
}

static void test_context_change_during_wait_is_not_stale(void)
{
    TEST("overlapping context changes suppress stale user_ctx");

    int pair[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        FAIL("socketpair");
        return;
    }
    int epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        close(pair[0]);
        close(pair[1]);
        return;
    }

    int old_context;
    int new_context;
    struct epoll_event event = {
        .events = EPOLLIN,
        .data.u64 = UINT64_C(0x9988776655443322)
    };
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, pair[0], &event,
                      &old_context) != 0) {
        FAIL("initial ADD");
        wepoll_close(epfd);
        close(pair[0]);
        close(pair[1]);
        return;
    }

    posix_wait_thread_context_t context = {
        .epfd = epfd,
        .started = ATOMIC_VAR_INIT(0),
        .result = 0,
        .error = 0,
        .event = {0}
    };
    pthread_t thread;
    int create_error = pthread_create(&thread, NULL, posix_wait_thread,
                                      &context);
    if (create_error != 0) {
        errno = create_error;
        FAIL("pthread_create");
        wepoll_close(epfd);
        close(pair[0]);
        close(pair[1]);
        return;
    }
    if (wait_atomic_at_least(&context.started, 1, 2000) != 0 ||
        sleep_milliseconds(100) != 0) {
        FAIL("blocking waiter did not start");
        (void)wepoll_close(epfd);
        pthread_join(thread, NULL);
        close(pair[0]);
        close(pair[1]);
        return;
    }

    /* Keep the payload identical.  A wait which overlaps this MOD cannot
     * distinguish the old and new registration versions from epoll_data
     * alone; returning either context would be unsafe, so the implementation
     * deliberately returns NULL for the event. */
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_MOD, pair[0], &event,
                      &new_context) != 0 ||
        write(pair[1], "c", 1) != 1) {
        FAIL("MOD/write");
        (void)wepoll_close(epfd);
        pthread_join(thread, NULL);
        close(pair[0]);
        close(pair[1]);
        return;
    }
    pthread_join(thread, NULL);
    if (context.result != 1 ||
        context.event.data.u64 != event.data.u64 ||
        context.event.user_ctx != NULL) {
        errno = context.error;
        FAIL("wait returned a stale context after overlapping MOD");
        wepoll_close(epfd);
        close(pair[0]);
        close(pair[1]);
        return;
    }
    wepoll_close(epfd);
    close(pair[0]);
    close(pair[1]);
    PASS();
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
/* Linux epoll distinguishes reused fd numbers by open file description. */
/* --------------------------------------------------------------------- */

static void test_live_registration_fd_reuse(void)
{
    TEST("live dup/close fd reuse preserves both registrations");

    int old_pair[2] = { -1, -1 };
    int new_pair[2] = { -1, -1 };
    int keeper = -1;
    int epfd = -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, old_pair) != 0) {
        FAIL("old socketpair");
        return;
    }

    epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        goto live_reuse_cleanup;
    }
    keeper = dup(old_pair[0]);
    if (keeper < 0) {
        FAIL("dup old registration");
        goto live_reuse_cleanup;
    }

    int reused_fd = old_pair[0];
    int old_context;
    int new_context;
    int modified_context;
    const uint64_t old_data = UINT64_C(0x4141414141414141);
    const uint64_t new_data = UINT64_C(0x4242424242424242);
    const uint64_t modified_data = UINT64_C(0x4343434343434343);
    struct epoll_event old_event = {
        .events = EPOLLIN,
        .data.u64 = old_data
    };
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, reused_fd, &old_event,
                      &old_context) != 0) {
        FAIL("old ADD");
        goto live_reuse_cleanup;
    }

    /* The duplicated descriptor keeps the old open file description and its
     * epoll registration alive while the original integer is reused. */
    close(old_pair[0]);
    old_pair[0] = -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, new_pair) != 0) {
        FAIL("new socketpair");
        goto live_reuse_cleanup;
    }
    if (new_pair[0] != reused_fd) {
        if (new_pair[1] == reused_fd) {
            int peer = new_pair[0];
            new_pair[0] = reused_fd;
            new_pair[1] = peer;
        } else {
            if (dup2(new_pair[0], reused_fd) != reused_fd) {
                FAIL("dup2 reused fd");
                goto live_reuse_cleanup;
            }
            close(new_pair[0]);
            new_pair[0] = reused_fd;
        }
    }

    struct epoll_event new_event = {
        .events = EPOLLIN,
        .data.u64 = new_data
    };
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, new_pair[0], &new_event,
                      &new_context) != 0 ||
        epoll_fd_count(epfd) != 2 ||
        write(old_pair[1], "o", 1) != 1 ||
        write(new_pair[1], "n", 1) != 1) {
        FAIL("new ADD/count/write");
        goto live_reuse_cleanup;
    }

    epoll_event_ex output[2];
    int count = epoll_wait_ex(epfd, output, 2, 100);
    int saw_old = 0;
    int saw_new = 0;
    if (count != 2) {
        FAIL("expected both reused-fd registrations");
        goto live_reuse_cleanup;
    }
    for (int i = 0; i < count; i++) {
        if (output[i].data.u64 == old_data &&
            output[i].user_ctx == &old_context) {
            saw_old = 1;
        } else if (output[i].data.u64 == new_data &&
                   output[i].user_ctx == &new_context) {
            saw_new = 1;
        } else {
            FAIL("reused-fd event metadata mismatch");
            goto live_reuse_cleanup;
        }
    }
    if (!saw_old || !saw_new ||
        read(keeper, &(char){0}, 1) != 1 ||
        read(new_pair[0], &(char){0}, 1) != 1) {
        FAIL("missing reused-fd event/read");
        goto live_reuse_cleanup;
    }

    /* MOD and DEL must select the current open file description without
     * overwriting or removing the still-live old registration. */
    new_event.data.u64 = modified_data;
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_MOD, new_pair[0], &new_event,
                      &modified_context) != 0 ||
        epoll_fd_count(epfd) != 2 ||
        write(old_pair[1], "p", 1) != 1 ||
        write(new_pair[1], "q", 1) != 1) {
        FAIL("reused-fd MOD/count/write");
        goto live_reuse_cleanup;
    }
    count = epoll_wait_ex(epfd, output, 2, 100);
    saw_old = 0;
    int saw_modified = 0;
    if (count != 2) {
        FAIL("expected both registrations after MOD");
        goto live_reuse_cleanup;
    }
    for (int i = 0; i < count; i++) {
        if (output[i].data.u64 == old_data &&
            output[i].user_ctx == &old_context) {
            saw_old = 1;
        } else if (output[i].data.u64 == modified_data &&
                   output[i].user_ctx == &modified_context) {
            saw_modified = 1;
        } else {
            FAIL("reused-fd MOD metadata mismatch");
            goto live_reuse_cleanup;
        }
    }
    if (!saw_old || !saw_modified ||
        read(keeper, &(char){0}, 1) != 1 ||
        read(new_pair[0], &(char){0}, 1) != 1 ||
        epoll_ctl_ctx(epfd, EPOLL_CTL_DEL, new_pair[0], NULL, NULL) != 0 ||
        epoll_fd_count(epfd) != 1 ||
        write(old_pair[1], "r", 1) != 1) {
        FAIL("reused-fd MOD read/DEL/count/write");
        goto live_reuse_cleanup;
    }
    count = epoll_wait_ex(epfd, output, 1, 100);
    if (count != 1 || output[0].data.u64 != old_data ||
        output[0].user_ctx != &old_context ||
        read(keeper, &(char){0}, 1) != 1) {
        FAIL("DEL removed the wrong reused-fd registration");
        goto live_reuse_cleanup;
    }

    PASS();

live_reuse_cleanup:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (old_pair[0] >= 0) close(old_pair[0]);
    if (old_pair[1] >= 0) close(old_pair[1]);
    if (new_pair[0] >= 0) close(new_pair[0]);
    if (new_pair[1] >= 0) close(new_pair[1]);
    if (keeper >= 0) close(keeper);
}

static void test_ambiguous_fd_identity_is_rejected(void)
{
    TEST("ambiguous reused-fd identity rejects MOD and DEL");

    int old_fd = -1;
    int new_fd = -1;
    int keeper = -1;
    int epfd = -1;
    old_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (old_fd < 0) {
        FAIL("old eventfd");
        return;
    }
    epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        FAIL("epoll_create_ex");
        goto ambiguous_cleanup;
    }
    keeper = dup(old_fd);
    if (keeper < 0) {
        FAIL("dup old eventfd");
        goto ambiguous_cleanup;
    }

    int reused_fd = old_fd;
    int old_context;
    int new_context;
    int modified_context;
    const uint64_t old_data = UINT64_C(0x5151515151515151);
    const uint64_t new_data = UINT64_C(0x5252525252525252);
    struct epoll_event old_event = {
        .events = EPOLLIN,
        .data.u64 = old_data
    };
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, old_fd, &old_event,
                      &old_context) != 0) {
        FAIL("old eventfd ADD");
        goto ambiguous_cleanup;
    }

    close(old_fd);
    old_fd = -1;
    new_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (new_fd < 0) {
        FAIL("new eventfd");
        goto ambiguous_cleanup;
    }
    if (new_fd != reused_fd) {
        if (dup2(new_fd, reused_fd) != reused_fd) {
            FAIL("dup2 new eventfd");
            goto ambiguous_cleanup;
        }
        close(new_fd);
        new_fd = reused_fd;
    }

    struct epoll_event new_event = {
        .events = EPOLLIN,
        .data.u64 = new_data
    };
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_ADD, new_fd, &new_event,
                      &new_context) != 0 ||
        epoll_fd_count(epfd) != 2) {
        FAIL("new eventfd ADD/count");
        goto ambiguous_cleanup;
    }

    struct epoll_event modified_event = {
        .events = EPOLLIN | EPOLLONESHOT,
        .data.u64 = UINT64_C(0x5353535353535353)
    };
    errno = 0;
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_MOD, new_fd, &modified_event,
                      &modified_context) != -1 ||
        errno != EOPNOTSUPP || epoll_fd_count(epfd) != 2) {
        FAIL("ambiguous MOD should return EOPNOTSUPP");
        goto ambiguous_cleanup;
    }
    errno = 0;
    if (epoll_ctl_ctx(epfd, EPOLL_CTL_DEL, new_fd, NULL, NULL) != -1 ||
        errno != EOPNOTSUPP || epoll_fd_count(epfd) != 2) {
        FAIL("ambiguous DEL should return EOPNOTSUPP");
        goto ambiguous_cleanup;
    }

    uint64_t one = 1;
    if (write(keeper, &one, sizeof(one)) != (ssize_t)sizeof(one) ||
        write(new_fd, &one, sizeof(one)) != (ssize_t)sizeof(one)) {
        FAIL("eventfd write");
        goto ambiguous_cleanup;
    }
    epoll_event_ex output[2];
    int count = epoll_wait_ex(epfd, output, 2, 100);
    int saw_old = 0;
    int saw_new = 0;
    if (count != 2) {
        FAIL("failed ambiguous controls changed native state");
        goto ambiguous_cleanup;
    }
    for (int i = 0; i < count; i++) {
        if (output[i].data.u64 == old_data &&
            output[i].user_ctx == &old_context) {
            saw_old = 1;
        } else if (output[i].data.u64 == new_data &&
                   output[i].user_ctx == &new_context) {
            saw_new = 1;
        } else {
            FAIL("ambiguous event metadata mismatch");
            goto ambiguous_cleanup;
        }
    }
    if (!saw_old || !saw_new) {
        FAIL("missing event after ambiguous controls");
        goto ambiguous_cleanup;
    }
    PASS();

ambiguous_cleanup:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (old_fd >= 0) close(old_fd);
    if (new_fd >= 0) close(new_fd);
    if (keeper >= 0) close(keeper);
}

/* --------------------------------------------------------------------- */
/* Closing stale metadata must never close an unrelated reused fd.       */
/* --------------------------------------------------------------------- */

typedef struct stale_close_fixture {
    int pipe_fds[2];
    int replacement;
} stale_close_fixture_t;

static void stale_close_fixture_dispose(stale_close_fixture_t *fixture)
{
    if (fixture->replacement >= 0) (void)close(fixture->replacement);
    if (fixture->pipe_fds[0] >= 0) (void)close(fixture->pipe_fds[0]);
    if (fixture->pipe_fds[1] >= 0) (void)close(fixture->pipe_fds[1]);
    fixture->replacement = -1;
    fixture->pipe_fds[0] = -1;
    fixture->pipe_fds[1] = -1;
}

static int stale_close_fixture_open(stale_close_fixture_t *fixture)
{
    fixture->pipe_fds[0] = -1;
    fixture->pipe_fds[1] = -1;
    fixture->replacement = -1;
    if (pipe(fixture->pipe_fds) != 0) return -1;

    int epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        int saved_errno = errno;
        stale_close_fixture_dispose(fixture);
        errno = saved_errno;
        return -1;
    }
    int stale_epfd = epfd;
    if (close(epfd) != 0) {
        int saved_errno = errno;
        (void)wepoll_close(epfd);
        stale_close_fixture_dispose(fixture);
        errno = saved_errno;
        return -1;
    }

    if (dup2(fixture->pipe_fds[0], stale_epfd) != stale_epfd) {
        int saved_errno = errno;
        (void)wepoll_close(stale_epfd);
        stale_close_fixture_dispose(fixture);
        errno = saved_errno;
        return -1;
    }
    fixture->replacement = stale_epfd;
    return 0;
}

static int stale_close_fixture_check(stale_close_fixture_t *fixture,
                                     char sent)
{
    if (fcntl(fixture->replacement, F_GETFD) < 0) return -1;

    char received = 0;
    if (write(fixture->pipe_fds[1], &sent, 1) != 1 ||
        read(fixture->replacement, &received, 1) != 1) {
        return -1;
    }
    if (received != sent) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static void test_stale_close_preserves_reused_fd(void)
{
    stale_close_fixture_t fixture;

    TEST("stale wepoll_close preserves reused descriptor");
    if (stale_close_fixture_open(&fixture) != 0) {
        FAIL("fixture setup");
        return;
    }
    errno = 0;
    int result = wepoll_close(fixture.replacement);
    int close_errno = errno;
    if (result != -1 || close_errno != EBADF) {
        errno = close_errno;
        FAIL("stale close should return EBADF");
        stale_close_fixture_dispose(&fixture);
        return;
    }
    if (stale_close_fixture_check(&fixture, 'x') != 0) {
        FAIL("replacement pipe is unusable");
        stale_close_fixture_dispose(&fixture);
        return;
    }
    stale_close_fixture_dispose(&fixture);
    PASS();

    TEST("stale probe preserves reused descriptor");
    if (stale_close_fixture_open(&fixture) != 0) {
        FAIL("fixture setup");
        return;
    }

    /* Exercise the path where another extension call notices the stale
     * generation before wepoll_close() is called. */
    errno = 0;
    if (epoll_fd_count(fixture.replacement) != -1 || errno != EINVAL) {
        FAIL("fd_count should reject the replacement");
        stale_close_fixture_dispose(&fixture);
        return;
    }

    errno = 0;
    result = wepoll_close(fixture.replacement);
    close_errno = errno;
    if (result != -1 || close_errno != EBADF) {
        errno = close_errno;
        FAIL("stale close after probe should return EBADF");
        stale_close_fixture_dispose(&fixture);
        return;
    }
    if (stale_close_fixture_check(&fixture, 'y') != 0) {
        FAIL("replacement pipe is unusable");
        stale_close_fixture_dispose(&fixture);
        return;
    }
    stale_close_fixture_dispose(&fixture);
    PASS();
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
    test_wait_maxevents_bounds();
    test_basic_event();
    test_ready_set_round_robin();
    test_double_add();
    test_del_noent();
    test_edge_triggered();
    test_inert_event_bits();
    test_extension_api();
    test_user_ctx();
    test_create_ex_and_timeout_validation();
    test_sigmask_semantics();
    test_ptr_and_u64_context();
    test_duplicate_data_context();
    test_mod_updates_duplicate_data_index();
    test_mod_context_and_data();
    test_mod_adopts_native_registration();
    test_rearm_preserves_registration();
    test_drain_and_batch();
    test_invalid_and_closed_descriptors();
    test_same_epfd_et_wakes_one_waiter();
    test_cancel_blocking_extended_wait();
    test_close_wakes_blocking_extended_wait();
    test_context_change_during_wait_is_not_stale();
    test_concurrent_close_and_reuse();
    test_native_close_and_reuse();
    test_live_registration_fd_reuse();
    test_ambiguous_fd_identity_is_rejected();
    test_stale_close_preserves_reused_fd();

    printf("\n");
    printf("Summary: %d passed, %d skipped, %d failed\n",
           tests_passed, tests_skipped, tests_failed);
    return tests_failed ? 1 : 0;
}
