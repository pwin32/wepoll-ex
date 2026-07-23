/*
 * test_windows_compat.c -- provider, UDP, and concurrent-control regressions.
 *
 * The provider-chain test uses the internal callback form of
 * ep_socket_get_base_with_ioctl so the fallback order can be tested without
 * installing a machine-wide Winsock layered service provider.  The remaining
 * modes exercise the public API against native loopback sockets.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SIO_BSP_HANDLE
#define SIO_BSP_HANDLE 0x4800001B
#endif
#ifndef SIO_BSP_HANDLE_SELECT
#define SIO_BSP_HANDLE_SELECT 0x4800001C
#endif
#ifndef SIO_BSP_HANDLE_POLL
#define SIO_BSP_HANDLE_POLL 0x4800001D
#endif
#ifndef SIO_BASE_HANDLE
#define SIO_BASE_HANDLE 0x48000022
#endif

#ifdef _WIN32

enum setup_result {
    SETUP_FAILED = -1,
    SETUP_OK = 0,
    SETUP_UNAVAILABLE = 1
};

typedef struct udp_fixture {
    SOCKET receiver;
    SOCKET sender;
    struct sockaddr_storage address;
    int address_length;
    int family;
} udp_fixture_t;

static void udp_fixture_init(udp_fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->receiver = INVALID_SOCKET;
    fixture->sender = INVALID_SOCKET;
    fixture->address_length = 0;
    fixture->family = AF_UNSPEC;
}

static void udp_fixture_close(udp_fixture_t *fixture)
{
    if (fixture->sender != INVALID_SOCKET) {
        (void)closesocket(fixture->sender);
        fixture->sender = INVALID_SOCKET;
    }
    if (fixture->receiver != INVALID_SOCKET) {
        (void)closesocket(fixture->receiver);
        fixture->receiver = INVALID_SOCKET;
    }
}

static int ipv6_unavailable_error(int error)
{
    return error == WSAEAFNOSUPPORT ||
           error == WSAEPROTONOSUPPORT ||
           error == WSAEADDRNOTAVAIL ||
           error == WSAEINVAL;
}

static int make_udp_fixture(udp_fixture_t *fixture, int family)
{
    int address_length;
    u_long nonblocking = 1;

    udp_fixture_init(fixture);
    fixture->family = family;
    fixture->receiver = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fixture->receiver == INVALID_SOCKET) {
        return family == AF_INET6 && ipv6_unavailable_error(WSAGetLastError())
            ? SETUP_UNAVAILABLE : SETUP_FAILED;
    }

    if (family == AF_INET) {
        struct sockaddr_in *address =
            (struct sockaddr_in *)&fixture->address;
        memset(address, 0, sizeof(*address));
        address->sin_family = AF_INET;
        address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address->sin_port = htons(0);
        address_length = (int)sizeof(*address);
    } else {
        struct sockaddr_in6 *address =
            (struct sockaddr_in6 *)&fixture->address;
        memset(address, 0, sizeof(*address));
        address->sin6_family = AF_INET6;
        address->sin6_addr = in6addr_loopback;
        address->sin6_port = htons(0);
        address_length = (int)sizeof(*address);
    }

    if (bind(fixture->receiver,
             (const struct sockaddr *)&fixture->address,
             address_length) == SOCKET_ERROR ||
        getsockname(fixture->receiver,
                    (struct sockaddr *)&fixture->address,
                    &address_length) == SOCKET_ERROR ||
        ioctlsocket(fixture->receiver, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        udp_fixture_close(fixture);
        return family == AF_INET6 && ipv6_unavailable_error(error)
            ? SETUP_UNAVAILABLE : SETUP_FAILED;
    }
    fixture->address_length = address_length;

    fixture->sender = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fixture->sender == INVALID_SOCKET) {
        int error = WSAGetLastError();
        udp_fixture_close(fixture);
        return family == AF_INET6 && ipv6_unavailable_error(error)
            ? SETUP_UNAVAILABLE : SETUP_FAILED;
    }
    return SETUP_OK;
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

static int wait_for_mask(int epfd, uint64_t data, uint32_t mask,
                         int timeout_ms, struct epoll_event *output)
{
    memset(output, 0, sizeof(*output));
    int count = epoll_wait(epfd, output, 1, timeout_ms);

    if (count != 1 || output->data.u64 != data ||
        (output->events & mask) != mask) {
        fprintf(stderr,
                "wait: count=%d errno=%d WSA=%d expected_data=0x%llx "
                "expected_mask=0x%08lx data=0x%llx events=0x%08lx\n",
                count, errno, WSAGetLastError(),
                (unsigned long long)data, (unsigned long)mask,
                (unsigned long long)output->data.u64,
                (unsigned long)output->events);
        return -1;
    }
    return 0;
}

static int wait_for_data(int epfd, uint64_t data, int timeout_ms,
                         struct epoll_event *output)
{
    return wait_for_mask(epfd, data, EPOLLIN, timeout_ms, output);
}

static int send_udp_byte(const udp_fixture_t *fixture, char byte)
{
    return sendto(fixture->sender, &byte, 1, 0,
                  (const struct sockaddr *)&fixture->address,
                  fixture->address_length) == 1 ? 0 : -1;
}

static int recv_udp_byte(const udp_fixture_t *fixture, char expected)
{
    char byte = 0;
    int result = recv(fixture->receiver, &byte, 1, 0);

    return result == 1 && byte == expected ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* Provider-chain resolver tests.                                            */
/* ------------------------------------------------------------------------- */

typedef struct fake_ioctl_state {
    int base_calls;
    int select_calls;
    int poll_calls;
    int generic_calls;
    int mode;
} fake_ioctl_state_t;

enum fake_provider_mode {
    FAKE_PROVIDER_POLL = 1,
    FAKE_PROVIDER_NONE,
    FAKE_PROVIDER_CYCLE,
    FAKE_PROVIDER_VISITED_THEN_POLL,
    FAKE_PROVIDER_SELECT,
    FAKE_PROVIDER_GENERIC,
    FAKE_PROVIDER_NOTSOCK,
    FAKE_PROVIDER_ACCESS_DENIED
};

static SOCKET fake_provider_ioctl(SOCKET socket_fd, DWORD ioctl,
                                  int *error_out, void *opaque)
{
    fake_ioctl_state_t *state = (fake_ioctl_state_t *)opaque;

    if (error_out != NULL)
        *error_out = WSAEINVAL;

    if (ioctl == SIO_BASE_HANDLE) {
        state->base_calls++;
        if (state->mode == FAKE_PROVIDER_NOTSOCK) {
            if (error_out != NULL)
                *error_out = WSAENOTSOCK;
            return INVALID_SOCKET;
        }
        if (state->mode == FAKE_PROVIDER_ACCESS_DENIED) {
            if (error_out != NULL)
                *error_out = WSAEACCES;
            return INVALID_SOCKET;
        }
        if ((state->mode == FAKE_PROVIDER_POLL ||
             state->mode == FAKE_PROVIDER_SELECT ||
             state->mode == FAKE_PROVIDER_GENERIC) &&
            socket_fd == (SOCKET)200) {
            if (error_out != NULL)
                *error_out = 0;
            return (SOCKET)300;
        }
        if (state->mode == FAKE_PROVIDER_VISITED_THEN_POLL &&
            socket_fd == (SOCKET)300) {
            if (error_out != NULL)
                *error_out = 0;
            return (SOCKET)400;
        }
        return INVALID_SOCKET;
    }
    if (ioctl == SIO_BSP_HANDLE_SELECT) {
        state->select_calls++;
        if ((state->mode == FAKE_PROVIDER_POLL ||
             state->mode == FAKE_PROVIDER_NONE ||
             state->mode == FAKE_PROVIDER_CYCLE) &&
            socket_fd == (SOCKET)100)
            return socket_fd;
        if (state->mode == FAKE_PROVIDER_VISITED_THEN_POLL) {
            if (socket_fd == (SOCKET)100)
                return (SOCKET)200;
            if (socket_fd == (SOCKET)200)
                return (SOCKET)100;
        }
        if (state->mode == FAKE_PROVIDER_SELECT &&
            socket_fd == (SOCKET)100)
            return (SOCKET)200;
        return INVALID_SOCKET;
    }
    if (ioctl == SIO_BSP_HANDLE_POLL) {
        state->poll_calls++;
        if (state->mode == FAKE_PROVIDER_POLL &&
            socket_fd == (SOCKET)100)
            return (SOCKET)200;
        if (state->mode == FAKE_PROVIDER_NONE &&
            socket_fd == (SOCKET)100)
            return socket_fd;
        if (state->mode == FAKE_PROVIDER_CYCLE &&
            socket_fd == (SOCKET)100)
            return (SOCKET)200;
        if (state->mode == FAKE_PROVIDER_CYCLE &&
            socket_fd == (SOCKET)200)
            return (SOCKET)100;
        if (state->mode == FAKE_PROVIDER_VISITED_THEN_POLL &&
            socket_fd == (SOCKET)200)
            return (SOCKET)300;
        return INVALID_SOCKET;
    }
    if (ioctl == SIO_BSP_HANDLE) {
        state->generic_calls++;
        if (state->mode == FAKE_PROVIDER_GENERIC &&
            socket_fd == (SOCKET)100)
            return (SOCKET)200;
        return INVALID_SOCKET;
    }
    return INVALID_SOCKET;
}

static int test_provider_fallback(void)
{
    fake_ioctl_state_t state;
    SOCKET result;

    memset(&state, 0, sizeof(state));
    state.mode = FAKE_PROVIDER_POLL;
    result = ep_socket_get_base_with_ioctl((SOCKET)100,
                                           fake_provider_ioctl, &state);
    if (result != (SOCKET)300 || state.base_calls != 2 ||
        state.select_calls != 1 || state.poll_calls != 1 ||
        state.generic_calls != 0) {
        fprintf(stderr, "provider fallback sequence failed: result=%llu "
                "base=%d select=%d poll=%d generic=%d\n",
                (unsigned long long)result, state.base_calls,
                state.select_calls, state.poll_calls, state.generic_calls);
        return -1;
    }

    memset(&state, 0, sizeof(state));
    state.mode = FAKE_PROVIDER_SELECT;
    result = ep_socket_get_base_with_ioctl((SOCKET)100,
                                           fake_provider_ioctl, &state);
    if (result != (SOCKET)300 || state.base_calls != 2 ||
        state.select_calls != 1 || state.poll_calls != 0 ||
        state.generic_calls != 0) {
        fprintf(stderr, "provider SELECT fallback failed: result=%llu "
                "base=%d select=%d poll=%d generic=%d\n",
                (unsigned long long)result, state.base_calls,
                state.select_calls, state.poll_calls, state.generic_calls);
        return -1;
    }

    memset(&state, 0, sizeof(state));
    state.mode = FAKE_PROVIDER_GENERIC;
    result = ep_socket_get_base_with_ioctl((SOCKET)100,
                                           fake_provider_ioctl, &state);
    if (result != (SOCKET)300 || state.base_calls != 2 ||
        state.select_calls != 1 || state.poll_calls != 1 ||
        state.generic_calls != 1) {
        fprintf(stderr, "provider generic fallback failed: result=%llu "
                "base=%d select=%d poll=%d generic=%d\n",
                (unsigned long long)result, state.base_calls,
                state.select_calls, state.poll_calls, state.generic_calls);
        return -1;
    }

    memset(&state, 0, sizeof(state));
    state.mode = FAKE_PROVIDER_VISITED_THEN_POLL;
    result = ep_socket_get_base_with_ioctl((SOCKET)100,
                                           fake_provider_ioctl, &state);
    if (result != (SOCKET)400 || state.base_calls != 3 ||
        state.select_calls != 2 || state.poll_calls != 1 ||
        state.generic_calls != 0) {
        fprintf(stderr, "provider fallback after visited candidate failed: "
                "result=%llu base=%d select=%d poll=%d generic=%d\n",
                (unsigned long long)result, state.base_calls,
                state.select_calls, state.poll_calls, state.generic_calls);
        return -1;
    }

    memset(&state, 0, sizeof(state));
    state.mode = FAKE_PROVIDER_NONE;
    errno = 0;
    result = ep_socket_get_base_with_ioctl((SOCKET)100,
                                           fake_provider_ioctl, &state);
    if (result != INVALID_SOCKET || errno != EINVAL ||
        state.generic_calls != 1) {
        fprintf(stderr, "provider fallback rejection failed: result=%llu "
                "errno=%d generic=%d\n", (unsigned long long)result,
                errno, state.generic_calls);
        return -1;
    }

    memset(&state, 0, sizeof(state));
    state.mode = FAKE_PROVIDER_CYCLE;
    errno = 0;
    result = ep_socket_get_base_with_ioctl((SOCKET)100,
                                           fake_provider_ioctl, &state);
    if (result != INVALID_SOCKET || errno != ELOOP) {
        fprintf(stderr, "provider cycle detection failed: result=%llu "
                "errno=%d\n", (unsigned long long)result, errno);
        return -1;
    }

    memset(&state, 0, sizeof(state));
    state.mode = FAKE_PROVIDER_NOTSOCK;
    errno = 0;
    result = ep_socket_get_base_with_ioctl((SOCKET)100,
                                           fake_provider_ioctl, &state);
    if (result != INVALID_SOCKET || errno != ENOTSOCK ||
        state.base_calls != 1 || state.select_calls != 0 ||
        state.poll_calls != 0 || state.generic_calls != 0) {
        fprintf(stderr, "provider ENOTSOCK handling failed: result=%llu "
                "errno=%d base=%d select=%d poll=%d generic=%d\n",
                (unsigned long long)result, errno, state.base_calls,
                state.select_calls, state.poll_calls, state.generic_calls);
        return -1;
    }

    memset(&state, 0, sizeof(state));
    state.mode = FAKE_PROVIDER_ACCESS_DENIED;
    errno = 0;
    result = ep_socket_get_base_with_ioctl((SOCKET)100,
                                           fake_provider_ioctl, &state);
    if (result != INVALID_SOCKET || errno != EACCES ||
        state.base_calls != 1 || state.select_calls != 1 ||
        state.poll_calls != 1 || state.generic_calls != 1) {
        fprintf(stderr, "provider base error preservation failed: "
                "result=%llu errno=%d base=%d select=%d poll=%d "
                "generic=%d\n", (unsigned long long)result, errno,
                state.base_calls, state.select_calls, state.poll_calls,
                state.generic_calls);
        return -1;
    }

    errno = 0;
    result = ep_socket_get_base_with_ioctl(INVALID_SOCKET,
                                           fake_provider_ioctl, &state);
    if (result != INVALID_SOCKET || errno != ENOTSOCK) {
        fprintf(stderr, "invalid provider socket check failed: result=%llu "
                "errno=%d\n", (unsigned long long)result, errno);
        return -1;
    }

    errno = 0;
    result = ep_socket_get_base_with_ioctl((SOCKET)100, NULL, &state);
    if (result != INVALID_SOCKET || errno != EFAULT) {
        fprintf(stderr, "NULL provider callback check failed: result=%llu "
                "errno=%d\n", (unsigned long long)result, errno);
        return -1;
    }
    return 0;
}

static int test_real_base_resolution(void)
{
    SOCKET socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET base;

    if (socket_fd == INVALID_SOCKET)
        return -1;
    errno = 0;
    base = ep_socket_get_base(socket_fd);
    (void)closesocket(socket_fd);
    if (base == INVALID_SOCKET || errno != 0) {
        fprintf(stderr, "real base resolution failed: base=%llu errno=%d "
                "WSA=%d\n", (unsigned long long)base, errno,
                WSAGetLastError());
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* UDP readiness and error tests.                                            */
/* ------------------------------------------------------------------------- */

static int test_udp_readiness(int family)
{
    udp_fixture_t fixture;
    struct epoll_event output;
    int epfd = -1;
    int setup;
    int result = -1;
    const uint64_t data = family == AF_INET
        ? UINT64_C(0x55445034) : UINT64_C(0x55445036);
    const char byte = family == AF_INET ? '4' : '6';

    setup = make_udp_fixture(&fixture, family);
    if (setup != SETUP_OK) {
        if (setup == SETUP_UNAVAILABLE) {
            printf("UDP IPv%d readiness: SKIP (address family unavailable)\n",
                   family == AF_INET ? 4 : 6);
            return 1;
        }
        return -1;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 || add_socket(epfd, fixture.receiver, EPOLLIN | EPOLLERR,
                               data) != 0 ||
        epoll_wait(epfd, &output, 1, 0) != 0 ||
        add_socket(epfd, fixture.sender, EPOLLOUT,
                   data + UINT64_C(1)) != 0 ||
        wait_for_mask(epfd, data + UINT64_C(1), EPOLLOUT, 2000,
                      &output) != 0 ||
        epoll_ctl(epfd, EPOLL_CTL_DEL, fixture.sender, NULL) != 0 ||
        send_udp_byte(&fixture, byte) != 0 ||
        wait_for_data(epfd, data, 2000, &output) != 0 ||
        recv_udp_byte(&fixture, byte) != 0 ||
        epoll_ctl(epfd, EPOLL_CTL_DEL, fixture.receiver, NULL) != 0) {
        fprintf(stderr, "UDP IPv%d readiness failed: errno=%d WSA=%d\n",
                family == AF_INET ? 4 : 6, errno, WSAGetLastError());
        goto cleanup;
    }
    result = 0;

cleanup:
    if (epfd >= 0)
        (void)wepoll_close(epfd);
    udp_fixture_close(&fixture);
    return result;
}

static int test_udp_error(void)
{
    udp_fixture_t probe;
    udp_fixture_t fixture;
    struct epoll_event output;
    int epfd = -1;
    int setup;
    int result = -1;
    int address_length;
    int wait_count;

    /* Reserve and release a loopback port, then connect a UDP socket to it.
     * Windows normally reports the resulting ICMP port-unreachable as an
     * asynchronous WSAECONNRESET/AFD abort; firewall policy may suppress it,
     * in which case this mode is explicitly skipped. */
    setup = make_udp_fixture(&probe, AF_INET);
    if (setup != SETUP_OK)
        return -1;
    address_length = probe.address_length;
    udp_fixture_close(&probe);

    udp_fixture_init(&fixture);
    fixture.family = AF_INET;
    fixture.sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fixture.sender == INVALID_SOCKET)
        goto cleanup;
    if (connect(fixture.sender, (const struct sockaddr *)&probe.address,
                address_length) == SOCKET_ERROR ||
        send(fixture.sender, "e", 1, 0) != 1) {
        goto cleanup;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 || add_socket(epfd, fixture.sender,
                               EPOLLERR | EPOLLIN, UINT64_C(0x55445045)) != 0)
        goto cleanup;

    wait_count = epoll_wait(epfd, &output, 1, 1500);
    if (wait_count == 0) {
        printf("UDP error readiness: SKIP (ICMP error suppressed)\n");
        result = 1;
        goto cleanup;
    }
    if (wait_count != 1 || output.data.u64 != UINT64_C(0x55445045) ||
        (output.events & (EPOLLERR | EPOLLHUP)) == 0) {
        fprintf(stderr, "UDP error event mismatch: count=%d errno=%d "
                "WSA=%d events=0x%08lx\n", wait_count, errno,
                WSAGetLastError(), (unsigned long)output.events);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (epfd >= 0)
        (void)wepoll_close(epfd);
    udp_fixture_close(&fixture);
    return result;
}

/* ------------------------------------------------------------------------- */
/* Registration lifecycle tests.                                             */
/* ------------------------------------------------------------------------- */

static int test_del_add_same_socket(void)
{
    udp_fixture_t fixture;
    struct epoll_event output;
    int epfd = -1;
    int result = -1;

    if (make_udp_fixture(&fixture, AF_INET) != SETUP_OK)
        return -1;
    epfd = epoll_create1(0);
    if (epfd < 0 || add_socket(epfd, fixture.receiver, EPOLLIN,
                               UINT64_C(0x44454c4f)) != 0 ||
        send_udp_byte(&fixture, 'a') != 0 ||
        epoll_ctl(epfd, EPOLL_CTL_DEL, fixture.receiver, NULL) != 0 ||
        add_socket(epfd, fixture.receiver, EPOLLIN,
                   UINT64_C(0x44454c4e)) != 0 ||
        wait_for_data(epfd, UINT64_C(0x44454c4e), 2000, &output) != 0 ||
        recv_udp_byte(&fixture, 'a') != 0 ||
        send_udp_byte(&fixture, 'b') != 0 ||
        wait_for_data(epfd, UINT64_C(0x44454c4e), 2000, &output) != 0 ||
        recv_udp_byte(&fixture, 'b') != 0) {
        fprintf(stderr, "DEL->ADD same socket failed: errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }
    result = 0;

cleanup:
    if (epfd >= 0)
        (void)wepoll_close(epfd);
    udp_fixture_close(&fixture);
    return result;
}

static int test_mod_before_first_wait(void)
{
    udp_fixture_t fixture;
    struct epoll_event output;
    struct epoll_event event;
    int epfd = -1;
    int result = -1;

    if (make_udp_fixture(&fixture, AF_INET) != SETUP_OK)
        return -1;
    epfd = epoll_create1(0);
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = UINT64_C(0x4d4f4441);
    if (epfd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, fixture.receiver,
                              &event) != 0) {
        goto cleanup;
    }
    event.data.u64 = UINT64_C(0x4d4f4442);
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fixture.receiver, &event) != 0 ||
        send_udp_byte(&fixture, 'm') != 0 ||
        wait_for_data(epfd, UINT64_C(0x4d4f4442), 2000, &output) != 0 ||
        recv_udp_byte(&fixture, 'm') != 0) {
        fprintf(stderr, "MOD-before-WAIT failed: errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }
    result = 0;

cleanup:
    if (epfd >= 0)
        (void)wepoll_close(epfd);
    udp_fixture_close(&fixture);
    return result;
}

static int test_oneshot_rearm_before_wait(void)
{
    udp_fixture_t fixture;
    struct epoll_event event;
    int epfd = -1;
    int result = -1;

    if (make_udp_fixture(&fixture, AF_INET) != SETUP_OK)
        return -1;
    epfd = epoll_create1(0);
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLONESHOT;
    event.data.u64 = UINT64_C(0x52415231);
    if (epfd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, fixture.receiver,
                              &event) != 0 ||
        send_udp_byte(&fixture, '1') != 0 ||
        epoll_wait(epfd, &event, 1, 2000) != 1 ||
        event.data.u64 != UINT64_C(0x52415231) ||
        (event.events & EPOLLIN) == 0 ||
        recv_udp_byte(&fixture, '1') != 0 ||
        epoll_rearm(epfd, fixture.receiver) != 0 ||
        send_udp_byte(&fixture, '2') != 0 ||
        epoll_wait(epfd, &event, 1, 2000) != 1 ||
        event.data.u64 != UINT64_C(0x52415231) ||
        (event.events & EPOLLIN) == 0 ||
        recv_udp_byte(&fixture, '2') != 0) {
        fprintf(stderr, "oneshot rearm-before-WAIT failed: errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }
    result = 0;

cleanup:
    if (epfd >= 0)
        (void)wepoll_close(epfd);
    udp_fixture_close(&fixture);
    return result;
}

typedef struct wait_thread_context {
    int epfd;
    HANDLE started;
    int result;
    int error;
    struct epoll_event event;
} wait_thread_context_t;

static DWORD WINAPI wait_thread_proc(void *opaque)
{
    wait_thread_context_t *context = (wait_thread_context_t *)opaque;
    struct epoll_event probe;

    memset(&probe, 0, sizeof(probe));
    errno = 0;
    context->result = epoll_wait(context->epfd, &probe, 1, 0);
    context->error = errno;
    SetEvent(context->started);
    if (context->result != 0)
        return 0;

    errno = 0;
    context->result = epoll_wait(context->epfd, &context->event, 1, 4000);
    context->error = errno;
    return 0;
}

static volatile LONG idle_dequeue_calls;

static BOOL WINAPI counting_get_queued_completion_status_ex(
    HANDLE completion_port,
    OVERLAPPED_ENTRY *entries,
    ULONG count,
    PULONG removed,
    DWORD timeout_ms,
    BOOL alertable)
{
    InterlockedIncrement(&idle_dequeue_calls);
    return GetQueuedCompletionStatusEx(completion_port, entries, count,
                                       removed, timeout_ms, alertable);
}

typedef struct port_wait_context {
    ep_port_t *port;
    HANDLE started;
    int result;
    int error;
    ULONGLONG elapsed;
    epoll_event_ex event;
} port_wait_context_t;

static DWORD WINAPI port_wait_thread_proc(void *opaque)
{
    port_wait_context_t *context = (port_wait_context_t *)opaque;
    ULONGLONG started = GetTickCount64();

    SetEvent(context->started);
    context->result = ep_port_wait(context->port, &context->event,
                                   1, 250, NULL);
    context->elapsed = GetTickCount64() - started;
    context->error = errno;
    return 0;
}

static int test_idle_same_socket_two_ports(void)
{
    udp_fixture_t fixture;
    ep_port_t *first_port = NULL;
    ep_port_t *second_port = NULL;
    port_wait_context_t first_context;
    port_wait_context_t second_context;
    HANDLE first_thread = NULL;
    HANDLE second_thread = NULL;
    epoll_data_t data;
    LONG dequeue_calls = 0;
    int result = -1;

    memset(&first_context, 0, sizeof(first_context));
    memset(&second_context, 0, sizeof(second_context));
    memset(&data, 0, sizeof(data));
    if (make_udp_fixture(&fixture, AF_INET) != SETUP_OK)
        return -1;
    if (ep_port_create(0, 0, &first_port) != 0 ||
        ep_port_create(0, 0, &second_port) != 0) {
        goto cleanup;
    }
    data.u64 = UINT64_C(0x49444c31);
    if (ep_port_register(first_port, fixture.receiver, EPOLLIN, 0,
                         data, NULL) != 0) {
        goto cleanup;
    }
    data.u64 = UINT64_C(0x49444c32);
    if (ep_port_register(second_port, fixture.receiver, EPOLLIN, 0,
                         data, NULL) != 0) {
        goto cleanup;
    }

    InterlockedExchange(&idle_dequeue_calls, 0);
    first_port->get_queued_completion_status_ex =
        counting_get_queued_completion_status_ex;
    second_port->get_queued_completion_status_ex =
        counting_get_queued_completion_status_ex;
    first_context.port = first_port;
    second_context.port = second_port;
    first_context.started = CreateEventA(NULL, TRUE, FALSE, NULL);
    second_context.started = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (first_context.started == NULL || second_context.started == NULL)
        goto cleanup;

    first_thread = CreateThread(NULL, 0, port_wait_thread_proc,
                                &first_context, 0, NULL);
    second_thread = CreateThread(NULL, 0, port_wait_thread_proc,
                                 &second_context, 0, NULL);
    if (first_thread == NULL || second_thread == NULL ||
        WaitForSingleObject(first_context.started, 2000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(second_context.started, 2000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(first_thread, 2000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(second_thread, 2000) != WAIT_OBJECT_0) {
        goto cleanup;
    }

    dequeue_calls = InterlockedCompareExchange(&idle_dequeue_calls, 0, 0);
    if (first_context.result != 0 || second_context.result != 0 ||
        first_context.elapsed < 100 || second_context.elapsed < 100 ||
        first_context.elapsed > 1500 || second_context.elapsed > 1500 ||
        dequeue_calls > 8) {
        fprintf(stderr, "idle two-port wait failed: first=%d/%d/%llu "
                "second=%d/%d/%llu dequeues=%ld\n",
                first_context.result, first_context.error,
                (unsigned long long)first_context.elapsed,
                second_context.result, second_context.error,
                (unsigned long long)second_context.elapsed,
                (long)dequeue_calls);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (first_thread != NULL &&
        WaitForSingleObject(first_thread, 0) != WAIT_OBJECT_0) {
        ep_port_begin_close(first_port);
    }
    if (second_thread != NULL &&
        WaitForSingleObject(second_thread, 0) != WAIT_OBJECT_0) {
        ep_port_begin_close(second_port);
    }
    if (first_thread != NULL) {
        if (WaitForSingleObject(first_thread, 5000) != WAIT_OBJECT_0)
            TerminateThread(first_thread, 1);
        CloseHandle(first_thread);
    }
    if (second_thread != NULL) {
        if (WaitForSingleObject(second_thread, 5000) != WAIT_OBJECT_0)
            TerminateThread(second_thread, 1);
        CloseHandle(second_thread);
    }
    if (first_context.started != NULL)
        CloseHandle(first_context.started);
    if (second_context.started != NULL)
        CloseHandle(second_context.started);
    if (first_port != NULL)
        (void)ep_port_destroy(first_port);
    if (second_port != NULL)
        (void)ep_port_destroy(second_port);
    udp_fixture_close(&fixture);
    return result;
}

static int test_same_socket_two_epfds(void)
{
    udp_fixture_t fixture;
    wait_thread_context_t first_context;
    wait_thread_context_t second_context;
    HANDLE first_thread = NULL;
    HANDLE second_thread = NULL;
    int first_epfd = -1;
    int second_epfd = -1;
    int result = -1;

    memset(&first_context, 0, sizeof(first_context));
    memset(&second_context, 0, sizeof(second_context));
    if (make_udp_fixture(&fixture, AF_INET) != SETUP_OK)
        return -1;
    first_epfd = epoll_create1(0);
    second_epfd = epoll_create1(0);
    first_context.epfd = first_epfd;
    second_context.epfd = second_epfd;
    first_context.started = CreateEventA(NULL, TRUE, FALSE, NULL);
    second_context.started = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (first_epfd < 0 || second_epfd < 0 ||
        first_context.started == NULL || second_context.started == NULL ||
        add_socket(first_epfd, fixture.receiver, EPOLLIN,
                   UINT64_C(0x45504631)) != 0 ||
        add_socket(second_epfd, fixture.receiver, EPOLLIN,
                   UINT64_C(0x45504632)) != 0) {
        fprintf(stderr, "same socket in two epfds failed: errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }
    first_thread = CreateThread(NULL, 0, wait_thread_proc,
                                &first_context, 0, NULL);
    second_thread = CreateThread(NULL, 0, wait_thread_proc,
                                 &second_context, 0, NULL);
    if (first_thread == NULL || second_thread == NULL ||
        WaitForSingleObject(first_context.started, 2000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(second_context.started, 2000) != WAIT_OBJECT_0 ||
        send_udp_byte(&fixture, 'p') != 0 ||
        WaitForSingleObject(first_thread, 5000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(second_thread, 5000) != WAIT_OBJECT_0 ||
        first_context.result != 1 || second_context.result != 1 ||
        first_context.event.data.u64 != UINT64_C(0x45504631) ||
        second_context.event.data.u64 != UINT64_C(0x45504632) ||
        (first_context.event.events & EPOLLIN) == 0 ||
        (second_context.event.events & EPOLLIN) == 0 ||
        recv_udp_byte(&fixture, 'p') != 0) {
        fprintf(stderr, "same socket in two epfds result failed: "
                "first=%d/%d second=%d/%d errno=%d WSA=%d\n",
                first_context.result, first_context.error,
                second_context.result, second_context.error,
                errno, WSAGetLastError());
        goto cleanup;
    }
    result = 0;

cleanup:
    if (first_epfd >= 0)
        (void)wepoll_close(first_epfd);
    if (second_epfd >= 0)
        (void)wepoll_close(second_epfd);
    if (first_thread != NULL) {
        if (WaitForSingleObject(first_thread, 5000) != WAIT_OBJECT_0) {
            /* Last resort if a broken close path failed to wake the waiter. */
            TerminateThread(first_thread, 1);
        }
        CloseHandle(first_thread);
    }
    if (second_thread != NULL) {
        if (WaitForSingleObject(second_thread, 5000) != WAIT_OBJECT_0) {
            /* Last resort if a broken close path failed to wake the waiter. */
            TerminateThread(second_thread, 1);
        }
        CloseHandle(second_thread);
    }
    if (first_context.started != NULL)
        CloseHandle(first_context.started);
    if (second_context.started != NULL)
        CloseHandle(second_context.started);
    udp_fixture_close(&fixture);
    return result;
}

static int test_concurrent_control(void)
{
    udp_fixture_t fixture;
    wait_thread_context_t context;
    HANDLE thread = NULL;
    int epfd = -1;
    int result = -1;
    struct epoll_event event;

    memset(&context, 0, sizeof(context));
    if (make_udp_fixture(&fixture, AF_INET) != SETUP_OK)
        return -1;
    epfd = epoll_create1(0);
    context.epfd = epfd;
    context.started = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (epfd < 0 || context.started == NULL ||
        add_socket(epfd, fixture.receiver, EPOLLIN,
                   UINT64_C(0x434f4e30)) != 0) {
        fprintf(stderr, "concurrent setup failed: errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }
    thread = CreateThread(NULL, 0, wait_thread_proc, &context, 0, NULL);
    if (thread == NULL ||
        WaitForSingleObject(context.started, 2000) != WAIT_OBJECT_0) {
        fprintf(stderr, "concurrent waiter start failed: errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }
    Sleep(50);

    for (unsigned int i = 0; i < 32; i++) {
        memset(&event, 0, sizeof(event));
        event.events = EPOLLIN;
        event.data.u64 = UINT64_C(0x434f4e10) + i;
        if (epoll_ctl(epfd, EPOLL_CTL_MOD, fixture.receiver, &event) != 0)
            goto cleanup;
        if ((i % 3U) == 0) {
            if (epoll_ctl(epfd, EPOLL_CTL_DEL, fixture.receiver, NULL) != 0 ||
                add_socket(epfd, fixture.receiver, EPOLLIN,
                           UINT64_C(0x434f4e40) + i) != 0) {
                goto cleanup;
            }
        }
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = UINT64_C(0x434f4eff);
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fixture.receiver, &event) != 0 ||
        send_udp_byte(&fixture, 'c') != 0 ||
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 ||
        context.result != 1 || context.event.data.u64 != event.data.u64 ||
        (context.event.events & EPOLLIN) == 0 ||
        recv_udp_byte(&fixture, 'c') != 0) {
        fprintf(stderr, "concurrent control result failed: result=%d "
                "error=%d data=0x%llx events=0x%08lx errno=%d WSA=%d\n",
                context.result, context.error,
                (unsigned long long)context.event.data.u64,
                (unsigned long)context.event.events, errno,
                WSAGetLastError());
        goto cleanup;
    }
    result = 0;

cleanup:
    if (epfd >= 0)
        (void)wepoll_close(epfd);
    if (thread != NULL) {
        if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) {
            /* Last resort if a broken close path failed to wake the waiter. */
            TerminateThread(thread, 1);
        }
        CloseHandle(thread);
    }
    if (context.started != NULL)
        CloseHandle(context.started);
    udp_fixture_close(&fixture);
    return result;
}

static int run_mode(const char *mode)
{
    if (strcmp(mode, "provider") == 0) {
        return test_provider_fallback() == 0 &&
               test_real_base_resolution() == 0 ? 0 : -1;
    }
    if (strcmp(mode, "udp-v4") == 0)
        return test_udp_readiness(AF_INET);
    if (strcmp(mode, "udp-v6") == 0)
        return test_udp_readiness(AF_INET6);
    if (strcmp(mode, "udp-error") == 0)
        return test_udp_error();
    if (strcmp(mode, "del-add") == 0)
        return test_del_add_same_socket();
    if (strcmp(mode, "mod-before-wait") == 0)
        return test_mod_before_first_wait();
    if (strcmp(mode, "oneshot-rearm-before-wait") == 0)
        return test_oneshot_rearm_before_wait();
    if (strcmp(mode, "two-epfds") == 0)
        return test_same_socket_two_epfds();
    if (strcmp(mode, "idle-two-epfds") == 0)
        return test_idle_same_socket_two_ports();
    if (strcmp(mode, "concurrent-ctl") == 0)
        return test_concurrent_control();
    fprintf(stderr, "unknown mode: %s\n", mode);
    return 2;
}

int main(int argc, char **argv)
{
    WSADATA wsa_data;
    int result;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 2;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s provider|udp-v4|udp-v6|udp-error|"
                "del-add|mod-before-wait|oneshot-rearm-before-wait|"
                "two-epfds|idle-two-epfds|concurrent-ctl\n", argv[0]);
        WSACleanup();
        return 2;
    }
    result = run_mode(argv[1]);
    WSACleanup();
    return result == SETUP_UNAVAILABLE ? 77 : result;
}

#else

int main(void)
{
    return 0;
}

#endif
