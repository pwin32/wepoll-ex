#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32

static int make_loopback_pair(SOCKET *listener_out, SOCKET *client_out,
                              SOCKET *accepted_out)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct sockaddr_in addr;
    int addrlen = (int)sizeof(addr);
    u_long nonblocking = 1;

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET || client == INVALID_SOCKET)
        goto fail;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR ||
        getsockname(listener, (struct sockaddr *)&addr, &addrlen) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR ||
        ioctlsocket(client, FIONBIO, &nonblocking) == SOCKET_ERROR ||
        connect(client, (const struct sockaddr *)&addr, addrlen) == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK)
            goto fail;
    }
    accepted = accept(listener, NULL, NULL);
    if (accepted == INVALID_SOCKET)
        goto fail;
    nonblocking = 1;
    if (ioctlsocket(accepted, FIONBIO, &nonblocking) == SOCKET_ERROR)
        goto fail;

    *listener_out = listener;
    *client_out = client;
    *accepted_out = accepted;
    return 0;

fail:
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return -1;
}

static int wait_one(int epfd, int timeout_ms, uint32_t *events_out,
                    uint32_t *flags_out)
{
    struct epoll_event_ex events[1];
    int n = epoll_wait_ex(epfd, events, 1, timeout_ms);
    if (n < 0)
        return -1;
    if (n == 0)
        return 0;
    if (events_out)
        *events_out = events[0].events;
    if (flags_out)
        *flags_out = events[0].flags;
    return 1;
}

static int test_edge_readable(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    char buffer[16];
    uint32_t events = 0;
    uint32_t flags = 0;
    int epfd = -1;
    int n;

    if (make_loopback_pair(&listener, &client, &accepted) != 0) {
        fputs("edge: pair setup failed\n", stderr);
        return 1;
    }

    epfd = epoll_create1(0);
    if (epfd < 0) {
        fputs("edge: epoll_create1 failed\n", stderr);
        goto fail;
    }

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET;
    event.data.u64 = 1;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0) {
        fprintf(stderr, "edge: ADD EPOLLET failed errno=%d\n", errno);
        goto fail;
    }

    if (send(client, "abc", 3, 0) != 3) {
        fputs("edge: send failed\n", stderr);
        goto fail;
    }

    n = wait_one(epfd, 1000, &events, &flags);
    if (n != 1 || (events & EPOLLIN) == 0 ||
        (flags & WEPOLL_FLAG_ET_DELIVERED) == 0) {
        fprintf(stderr, "edge: first wait n=%d events=%u flags=%u errno=%d\n",
                n, events, flags, errno);
        goto fail;
    }

    /* Still unread: edge-triggered must not redeliver the same level. */
    n = wait_one(epfd, 50, &events, &flags);
    if (n != 0) {
        fprintf(stderr, "edge: unexpected redelivery n=%d events=%u\n",
                n, events);
        goto fail;
    }

    if (recv(accepted, buffer, sizeof(buffer), 0) != 3) {
        fputs("edge: recv failed\n", stderr);
        goto fail;
    }

    /* Observe the drained (EAGAIN) condition before the next write. */
    n = wait_one(epfd, 0, &events, &flags);
    if (n != 0) {
        fprintf(stderr, "edge: post-drain wait n=%d events=%u\n", n, events);
        goto fail;
    }

    if (send(client, "de", 2, 0) != 2) {
        fputs("edge: second send failed\n", stderr);
        goto fail;
    }

    n = wait_one(epfd, 1000, &events, &flags);
    if (n != 1 || (events & EPOLLIN) == 0) {
        fprintf(stderr, "edge: second edge missing n=%d events=%u\n",
                n, events);
        goto fail;
    }

    (void)wepoll_close(epfd);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("edge-readable: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_edge_writable_no_spin(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    uint32_t events = 0;
    int epfd = -1;
    int n;
    int i;

    if (make_loopback_pair(&listener, &client, &accepted) != 0) {
        fputs("edge-out: pair setup failed\n", stderr);
        return 1;
    }

    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLOUT | EPOLLET;
    event.data.u64 = 2;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0) {
        fprintf(stderr, "edge-out: ADD failed errno=%d\n", errno);
        goto fail;
    }

    n = wait_one(epfd, 1000, &events, NULL);
    if (n != 1 || (events & EPOLLOUT) == 0) {
        fprintf(stderr, "edge-out: first OUT missing n=%d events=%u\n",
                n, events);
        goto fail;
    }

    for (i = 0; i < 8; i++) {
        n = wait_one(epfd, 20, &events, NULL);
        if (n != 0) {
            fprintf(stderr, "edge-out: spin redelivery i=%d n=%d events=%u\n",
                    i, n, events);
            goto fail;
        }
    }

    (void)wepoll_close(epfd);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("edge-writable: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_exclusive_mod_rejected(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    int epfd = -1;

    if (make_loopback_pair(&listener, &client, &accepted) != 0)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLEXCLUSIVE;
    event.data.u64 = 3;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0)
        goto fail;

    /* Linux rejects every MOD of an exclusive registration, even when the
     * MOD mask does not repeat EPOLLEXCLUSIVE. */
    event.events = EPOLLIN;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, accepted, &event) != -1 ||
        errno != EINVAL) {
        fprintf(stderr, "exclusive-mod: expected EINVAL, got errno=%d\n",
                errno);
        goto fail;
    }

    (void)wepoll_close(epfd);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("exclusive-mod: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int wait_for_exclusive_recipient(int epfd_a, int epfd_b,
                                        int *winner_out, const char *label)
{
    struct epoll_event events[1];
    int winner = 0;
    int loser_checks = 0;
    int i;

    for (i = 0; i < 40 && loser_checks < 6; i++) {
        int n;

        if (winner != 1) {
            n = epoll_wait(epfd_a, events, 1, 25);
            if (n < 0)
                return -1;
            if (n == 1 && (events[0].events & EPOLLIN) != 0) {
                if (winner == 2) {
                    fprintf(stderr, "%s: both instances woke\n", label);
                    return -1;
                }
                winner = 1;
            }
        }
        if (winner != 2) {
            n = epoll_wait(epfd_b, events, 1, 25);
            if (n < 0)
                return -1;
            if (n == 1 && (events[0].events & EPOLLIN) != 0) {
                if (winner == 1) {
                    fprintf(stderr, "%s: both instances woke\n", label);
                    return -1;
                }
                winner = 2;
            }
        }
        if (winner != 0)
            loser_checks++;
    }
    if (winner == 0) {
        fprintf(stderr, "%s: neither instance woke\n", label);
        return -1;
    }
    *winner_out = winner;
    return 0;
}

static int arm_exclusive_after_drain(int epfd, const char *label)
{
    struct epoll_event events[1];
    int n = epoll_wait(epfd, events, 1, 0);

    /* A queued peer completion from the prior level must be re-sampled and
     * discarded after the application has synchronously drained the socket. */
    if (n < 0) {
        fprintf(stderr, "%s: rearm failed errno=%d\n", label, errno);
        return -1;
    }
    if (n != 0) {
        fprintf(stderr, "%s: stale post-drain events=%u\n",
                label, events[0].events);
        return -1;
    }
    return 0;
}

static int test_exclusive_two_cycles(int edge_triggered)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    struct epoll_event events[1];
    int epfd_a = -1;
    int epfd_b = -1;
    int first_winner = 0;
    int second_winner = 0;
    char byte;
    uint32_t flags = EPOLLIN | EPOLLEXCLUSIVE;

    if (edge_triggered)
        flags |= EPOLLET;

    if (make_loopback_pair(&listener, &client, &accepted) != 0)
        return 1;

    epfd_a = epoll_create1(0);
    epfd_b = epoll_create1(0);
    if (epfd_a < 0 || epfd_b < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = flags;
    event.data.u64 = 11;
    if (epoll_ctl(epfd_a, EPOLL_CTL_ADD, accepted, &event) != 0) {
        fprintf(stderr, "exclusive: ADD A failed errno=%d\n", errno);
        goto fail;
    }

    event.events = flags;
    event.data.u64 = 22;
    if (epoll_ctl(epfd_b, EPOLL_CTL_ADD, accepted, &event) != 0) {
        fprintf(stderr, "exclusive: ADD B failed errno=%d\n", errno);
        goto fail;
    }

    /* Arm both instances by entering wait briefly. */
    (void)epoll_wait(epfd_a, events, 1, 0);
    (void)epoll_wait(epfd_b, events, 1, 0);

    if (send(client, "x", 1, 0) != 1)
        goto fail;

    if (wait_for_exclusive_recipient(epfd_a, epfd_b, &first_winner,
                                     edge_triggered ? "exclusive-et first" :
                                                      "exclusive first") != 0) {
        goto fail;
    }
    if (recv(accepted, &byte, 1, 0) != 1) {
        fprintf(stderr, "exclusive: first recv failed WSA=%d\n",
                WSAGetLastError());
        goto fail;
    }

    /* A pending re-submit after the application drains the byte is the
     * quiescent transition that releases the process-wide exclusive claim. */
    if (arm_exclusive_after_drain(epfd_a, "exclusive rearm A") != 0 ||
        arm_exclusive_after_drain(epfd_b, "exclusive rearm B") != 0) {
        goto fail;
    }

    if (send(client, "y", 1, 0) != 1)
        goto fail;
    if (wait_for_exclusive_recipient(epfd_a, epfd_b, &second_winner,
                                     edge_triggered ? "exclusive-et second" :
                                                      "exclusive second") != 0) {
        goto fail;
    }
    if (recv(accepted, &byte, 1, 0) != 1) {
        fprintf(stderr, "exclusive: second recv failed WSA=%d\n",
                WSAGetLastError());
        goto fail;
    }

    (void)wepoll_close(epfd_a);
    (void)wepoll_close(epfd_b);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    printf("%s: first=%c second=%c OK\n",
           edge_triggered ? "exclusive-et-wake" : "exclusive-wake",
           first_winner == 1 ? 'A' : 'B',
           second_winner == 1 ? 'A' : 'B');
    return 0;

fail:
    if (epfd_a >= 0) (void)wepoll_close(epfd_a);
    if (epfd_b >= 0) (void)wepoll_close(epfd_b);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_exclusive_disjoint_classes(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    struct epoll_event output;
    int epfd_read = -1;
    int epfd_read_second = -1;
    int epfd_write = -1;
    char byte;

    if (make_loopback_pair(&listener, &client, &accepted) != 0)
        return 1;

    epfd_read = epoll_create1(0);
    epfd_write = epoll_create1(0);
    if (epfd_read < 0 || epfd_write < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLOUT | EPOLLEXCLUSIVE;
    event.data.u64 = 32;
    if (epoll_ctl(epfd_write, EPOLL_CTL_ADD, accepted, &event) != 0 ||
        epoll_wait(epfd_write, &output, 1, 1000) != 1 ||
        (output.events & EPOLLOUT) == 0 || output.data.u64 != 32) {
        fprintf(stderr,
                "exclusive-disjoint: initial OUT missing events=%u errno=%d\n",
                output.events, errno);
        goto fail;
    }

    event.events = EPOLLIN | EPOLLOUT | EPOLLEXCLUSIVE;
    event.data.u64 = 31;
    if (epoll_ctl(epfd_read, EPOLL_CTL_ADD, accepted, &event) != 0 ||
        epoll_wait(epfd_read, &output, 1, 0) != 0) {
        fprintf(stderr, "exclusive-disjoint: mixed arm failed errno=%d\n",
                errno);
        goto fail;
    }

    /* The writable registration now owns a continuously active OUT class.
     * The mixed registration must filter that class without suppressing its
     * independent IN class on the same base socket. */
    if (send(client, "z", 1, 0) != 1 ||
        epoll_wait(epfd_read, &output, 1, 1000) != 1 ||
        (output.events & EPOLLIN) == 0 || (output.events & EPOLLOUT) != 0 ||
        output.data.u64 != 31 ||
        recv(accepted, &byte, 1, 0) != 1) {
        fprintf(stderr,
                "exclusive-disjoint: IN blocked by OUT owner events=%u "
                "errno=%d WSA=%d\n",
                output.events, errno, WSAGetLastError());
        goto fail;
    }

    /* OUT remains true, so the mixed registration's next AFD request cannot
     * become wholly pending.  Its read-side select sample must still release
     * the now-inactive READ claim, allowing another IN-only owner to win. */
    if (epoll_wait(epfd_read, &output, 1, 20) != 0) {
        fprintf(stderr,
                "exclusive-disjoint: mixed post-drain wait returned events=%u\n",
                output.events);
        goto fail;
    }
    epfd_read_second = epoll_create1(0);
    event.events = EPOLLIN | EPOLLEXCLUSIVE;
    event.data.u64 = 33;
    if (epfd_read_second < 0 ||
        epoll_ctl(epfd_read_second, EPOLL_CTL_ADD, accepted, &event) != 0 ||
        epoll_wait(epfd_read_second, &output, 1, 0) != 0 ||
        send(client, "q", 1, 0) != 1 ||
        epoll_wait(epfd_read_second, &output, 1, 1000) != 1 ||
        (output.events & EPOLLIN) == 0 || output.data.u64 != 33 ||
        recv(accepted, &byte, 1, 0) != 1) {
        fprintf(stderr,
                "exclusive-disjoint: inactive READ claim was retained "
                "events=%u errno=%d WSA=%d\n",
                output.events, errno, WSAGetLastError());
        goto fail;
    }

    (void)wepoll_close(epfd_read);
    (void)wepoll_close(epfd_read_second);
    (void)wepoll_close(epfd_write);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("exclusive-disjoint: OK");
    return 0;

fail:
    if (epfd_read >= 0) (void)wepoll_close(epfd_read);
    if (epfd_read_second >= 0) (void)wepoll_close(epfd_read_second);
    if (epfd_write >= 0) (void)wepoll_close(epfd_write);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_exclusive_invalid_combos(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    int epfd = -1;

    if (make_loopback_pair(&listener, &client, &accepted) != 0)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLEXCLUSIVE | EPOLLONESHOT;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != -1 ||
        errno != EINVAL) {
        fprintf(stderr, "exclusive+oneshot: expected EINVAL errno=%d\n",
                errno);
        goto fail;
    }

    event.events = EPOLLIN | EPOLLRDHUP | EPOLLEXCLUSIVE;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != -1 ||
        errno != EINVAL) {
        fprintf(stderr, "exclusive+rdhup: expected EINVAL errno=%d\n", errno);
        goto fail;
    }

    event.events = EPOLLPRI | EPOLLEXCLUSIVE;
    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != -1 ||
        errno != EINVAL) {
        fprintf(stderr, "exclusive+pri: expected EINVAL errno=%d\n", errno);
        goto fail;
    }

    event.events = EPOLLIN | EPOLLEXCLUSIVE | EPOLLET;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0) {
        fprintf(stderr, "exclusive+et: ADD failed errno=%d\n", errno);
        goto fail;
    }
    if (epoll_fd_count(epfd) != 1 ||
        epoll_ctl(epfd, EPOLL_CTL_DEL, accepted, NULL) != 0 ||
        epoll_fd_count(epfd) != 0) {
        fputs("exclusive+et: registration lifecycle failed\n", stderr);
        goto fail;
    }

    (void)wepoll_close(epfd);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("exclusive-invalid: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_pwait_nonnull_sigmask(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    struct epoll_event events[1];
    struct epoll_event_ex events_ex[1];
    struct timespec zero;
    int dummy_mask = 0x5a5a5a5a;
    int epfd = -1;
    int n;

    if (make_loopback_pair(&listener, &client, &accepted) != 0)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = 99;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0)
        goto fail;
    if (send(client, "z", 1, 0) != 1)
        goto fail;

    /* Opaque non-null mask must be accepted and ignored on Windows. */
    n = epoll_pwait(epfd, events, 1, 1000,
                    (const wepoll_sigset_t *)&dummy_mask);
    if (n != 1 || (events[0].events & EPOLLIN) == 0) {
        fprintf(stderr, "pwait non-null mask failed n=%d errno=%d\n",
                n, errno);
        goto fail;
    }

    if (send(client, "y", 1, 0) != 1)
        goto fail;
    zero.tv_sec = 0;
    zero.tv_nsec = 0;
    n = epoll_pwait2_ex(epfd, events_ex, 1, &zero,
                        (const wepoll_sigset_t *)&dummy_mask);
    if (n != 1 || (events_ex[0].events & EPOLLIN) == 0) {
        fprintf(stderr, "pwait2_ex non-null mask failed n=%d errno=%d\n",
                n, errno);
        goto fail;
    }

    (void)wepoll_close(epfd);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("pwait-sigmask: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int test_wakeup_flag_accepted(void)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct epoll_event event;
    int epfd = -1;

    if (make_loopback_pair(&listener, &client, &accepted) != 0)
        return 1;
    epfd = epoll_create1(0);
    if (epfd < 0)
        goto fail;

    memset(&event, 0, sizeof(event));
    /* EPOLLWAKEUP has no freezable-task equivalent on Windows; accept and
     * ignore it so portable event masks do not fail registration. */
    event.events = EPOLLIN | EPOLLWAKEUP;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, accepted, &event) != 0 ||
        epoll_fd_count(epfd) != 1) {
        fprintf(stderr, "EPOLLWAKEUP ADD failed errno=%d\n", errno);
        goto fail;
    }

    (void)wepoll_close(epfd);
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    puts("wakeup-flag: OK");
    return 0;

fail:
    if (epfd >= 0) (void)wepoll_close(epfd);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return 1;
}

static int run_mode(const char *mode)
{
    if (strcmp(mode, "readable") == 0)
        return test_edge_readable();
    if (strcmp(mode, "writable") == 0)
        return test_edge_writable_no_spin();
    if (strcmp(mode, "exclusive-mod") == 0)
        return test_exclusive_mod_rejected();
    if (strcmp(mode, "exclusive-wake") == 0)
        return test_exclusive_two_cycles(0);
    if (strcmp(mode, "exclusive-et") == 0)
        return test_exclusive_two_cycles(1);
    if (strcmp(mode, "exclusive-disjoint") == 0)
        return test_exclusive_disjoint_classes();
    if (strcmp(mode, "exclusive-invalid") == 0)
        return test_exclusive_invalid_combos();
    if (strcmp(mode, "pwait-sigmask") == 0)
        return test_pwait_nonnull_sigmask();
    if (strcmp(mode, "wakeup-flag") == 0)
        return test_wakeup_flag_accepted();
    fprintf(stderr, "unknown edge mode: %s\n", mode);
    return 2;
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    int failures = 0;
    const char *modes[] = {
        "readable", "writable", "exclusive-mod", "exclusive-wake",
        "exclusive-et", "exclusive-disjoint", "exclusive-invalid",
        "pwait-sigmask", "wakeup-flag"
    };
    size_t i;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fputs("WSAStartup failed\n", stderr);
        return 2;
    }

    if (argc > 1) {
        int rc = run_mode(argv[1]);
        WSACleanup();
        return rc;
    }

    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        int rc = run_mode(modes[i]);
        if (rc != 0)
            failures++;
    }
    WSACleanup();
    return failures == 0 ? 0 : 1;
}

#else
int main(void)
{
    return 0;
}
#endif
