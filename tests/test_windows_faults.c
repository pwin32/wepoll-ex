/* Deterministic fail-at-N regressions for internal Windows fault points. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SIO_BASE_HANDLE
#define SIO_BASE_HANDLE 0x48000022
#endif

static int test_framework(void)
{
    ep_fault_reset();

    errno = 0;
    if (ep_fault_configure(EP_FAULT_POINT_COUNT, 1, EIO) != -1 ||
        errno != EINVAL) {
        return -1;
    }
    errno = 0;
    if (ep_fault_configure(EP_FAULT_PROVIDER_BASE, 0, EIO) != -1 ||
        errno != EINVAL) {
        return -1;
    }
    if (ep_fault_configure(EP_FAULT_PROVIDER_BASE, 2, EIO) != 0)
        return -1;

    errno = 0;
    if (ep_fault_hit(EP_FAULT_PROVIDER_BASE) != 0 || errno != 0)
        return -1;
    if (ep_fault_hit(EP_FAULT_PROVIDER_BASE) != -1 || errno != EIO)
        return -1;
    errno = 0;
    if (ep_fault_hit(EP_FAULT_PROVIDER_BASE) != 0 || errno != 0 ||
        ep_fault_hits(EP_FAULT_PROVIDER_BASE) != 3) {
        return -1;
    }

    ep_fault_reset();
    return ep_fault_hits(EP_FAULT_PROVIDER_BASE) == 0 ? 0 : -1;
}

static int test_pool_init_alloc(void)
{
    ep_afd_pool_t pool;
    int result = -1;

    memset(&pool, 0, sizeof(pool));
    ep_fault_reset();
    if (ep_fault_configure(EP_FAULT_POOL_INIT_ALLOC, 3, ENOMEM) != 0)
        return -1;

    errno = 0;
    if (ep_afd_pool_init(&pool, 64, 4) != -1 || errno != ENOMEM ||
        ep_fault_hits(EP_FAULT_POOL_INIT_ALLOC) != 3 ||
        pool.initialized || pool.stack != NULL || pool.all_entries != NULL ||
        pool.allocated != 0 ||
        atomic_load_explicit(&pool.in_use, memory_order_relaxed) != 0) {
        return -1;
    }

    ep_fault_reset();
    if (ep_afd_pool_init(&pool, 64, 4) != 0)
        return -1;
    if (pool.initialized && pool.allocated == 4 &&
        atomic_load_explicit(&pool.in_use, memory_order_relaxed) == 0) {
        result = 0;
    }
    ep_afd_pool_destroy(&pool);
    return result == 0 && !pool.initialized ? 0 : -1;
}

static int test_pool_growth(void)
{
    ep_afd_pool_t pool;
    void *buffers[4] = { NULL, NULL, NULL, NULL };
    int initialized = 0;
    int result = -1;

    memset(&pool, 0, sizeof(pool));
    ep_fault_reset();
    if (ep_afd_pool_init(&pool, 64, 2) != 0)
        goto cleanup;
    initialized = 1;
    buffers[0] = ep_afd_pool_take(&pool);
    buffers[1] = ep_afd_pool_take(&pool);
    if (buffers[0] == NULL || buffers[1] == NULL)
        goto cleanup;
    if (ep_fault_configure(EP_FAULT_POOL_GROW, 2, ENOMEM) != 0)
        goto cleanup;

    buffers[2] = ep_afd_pool_take(&pool);
    errno = 0;
    buffers[3] = ep_afd_pool_take(&pool);
    if (buffers[2] == NULL || buffers[3] != NULL || errno != ENOMEM ||
        ep_fault_hits(EP_FAULT_POOL_GROW) != 2 || pool.allocated != 3 ||
        atomic_load_explicit(&pool.in_use, memory_order_relaxed) != 3 ||
        atomic_load_explicit(&pool.peak, memory_order_relaxed) != 3) {
        goto cleanup;
    }

    ep_fault_reset();
    buffers[3] = ep_afd_pool_take(&pool);
    if (buffers[3] == NULL || pool.allocated != 4 ||
        atomic_load_explicit(&pool.in_use, memory_order_relaxed) != 4 ||
        atomic_load_explicit(&pool.peak, memory_order_relaxed) != 4) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (initialized) {
        for (size_t i = 0; i < 4; i++) {
            if (buffers[i] != NULL)
                ep_afd_pool_give(&pool, buffers[i]);
        }
        if (atomic_load_explicit(&pool.in_use, memory_order_relaxed) != 0)
            result = -1;
        ep_afd_pool_destroy(&pool);
        if (pool.initialized)
            result = -1;
    }
    ep_fault_reset();
    return result;
}

typedef struct provider_state {
    int calls;
} provider_state_t;

static SOCKET provider_ioctl_stub(SOCKET socket_fd, DWORD ioctl,
                                  int *error_out, void *context)
{
    provider_state_t *state = (provider_state_t *)context;

    state->calls++;
    if (error_out != NULL)
        *error_out = 0;
    if (ioctl == SIO_BASE_HANDLE)
        return socket_fd + 1;
    if (error_out != NULL)
        *error_out = WSAEINVAL;
    return INVALID_SOCKET;
}

static int test_provider_base(void)
{
    provider_state_t state;
    SOCKET result;

    memset(&state, 0, sizeof(state));
    ep_fault_reset();
    if (ep_fault_configure(EP_FAULT_PROVIDER_BASE, 2, EACCES) != 0)
        return -1;

    result = ep_socket_get_base_with_ioctl((SOCKET)100,
                                           provider_ioctl_stub, &state);
    if (result != (SOCKET)101 || state.calls != 1)
        return -1;

    errno = 0;
    result = ep_socket_get_base_with_ioctl((SOCKET)100,
                                           provider_ioctl_stub, &state);
    if (result != INVALID_SOCKET || errno != EACCES || state.calls != 1 ||
        ep_fault_hits(EP_FAULT_PROVIDER_BASE) != 2) {
        return -1;
    }

    ep_fault_reset();
    result = ep_socket_get_base_with_ioctl((SOCKET)100,
                                           provider_ioctl_stub, &state);
    return result == (SOCKET)101 && state.calls == 2 &&
           ep_fault_hits(EP_FAULT_PROVIDER_BASE) == 0 ? 0 : -1;
}

static int test_afd_open(void)
{
    HANDLE iocp = NULL;
    HANDLE afd = NULL;
    HANDLE injected_out = (HANDLE)(uintptr_t)UINT64_C(0x1234);
    int result = -1;

    ep_fault_reset();
    if (ep_global_init() != 0)
        return -1;
    iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (iocp == NULL ||
        ep_fault_configure(EP_FAULT_AFD_OPEN, 2, EACCES) != 0) {
        goto cleanup;
    }

    if (ep_afd_open(iocp, &afd) != 0 || afd == NULL)
        goto cleanup;
    (void)CloseHandle(afd);
    afd = NULL;

    errno = 0;
    if (ep_afd_open(iocp, &injected_out) != -1 || errno != EACCES ||
        injected_out != (HANDLE)(uintptr_t)UINT64_C(0x1234) ||
        ep_fault_hits(EP_FAULT_AFD_OPEN) != 2) {
        goto cleanup;
    }

    ep_fault_reset();
    if (ep_afd_open(iocp, &afd) != 0 || afd == NULL ||
        ep_fault_hits(EP_FAULT_AFD_OPEN) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (afd != NULL)
        (void)CloseHandle(afd);
    if (iocp != NULL)
        (void)CloseHandle(iocp);
    ep_fault_reset();
    return result;
}

static int g_submit_calls;

static NTSTATUS NTAPI submit_stub(
    HANDLE file_handle, HANDLE event, PIO_APC_ROUTINE apc_routine,
    PVOID apc_context, PIO_STATUS_BLOCK io_status_block, ULONG ioctl,
    PVOID input_buffer, ULONG input_length, PVOID output_buffer,
    ULONG output_length)
{
    (void)file_handle;
    (void)event;
    (void)apc_routine;
    (void)apc_context;
    (void)io_status_block;
    (void)ioctl;
    (void)input_buffer;
    (void)input_length;
    (void)output_buffer;
    (void)output_length;
    g_submit_calls++;
    return STATUS_PENDING;
}

static void submit_sock_init(ep_sock_t *sock, ep_port_t *port,
                             SOCKET socket_fd, uint32_t submitted)
{
    memset(sock, 0, sizeof(*sock));
    sock->fd = socket_fd;
#ifdef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    sock->base_socket = socket_fd;
#else
    sock->base_socket = INVALID_SOCKET;
#endif
    sock->port = port;
    sock->submitted_afd_events = submitted;
    atomic_init(&sock->poll_status, EP_POLL_IDLE);
}

static int test_afd_submit(void)
{
    ep_port_t port;
    ep_sock_t first;
    ep_sock_t second;
    SOCKET first_fd = INVALID_SOCKET;
    SOCKET second_fd = INVALID_SOCKET;
    PNtDeviceIoControlFile original_submit = NULL;
    int stub_installed = 0;
    int result = -1;
    const uint32_t old_mask = UINT32_C(0x5a5a);

    memset(&port, 0, sizeof(port));
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    port.afd = (HANDLE)(uintptr_t)1;
    ep_fault_reset();
    if (ep_global_init() != 0)
        goto cleanup;
    first_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    second_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (first_fd == INVALID_SOCKET || second_fd == INVALID_SOCKET)
        goto cleanup;
    submit_sock_init(&first, &port, first_fd, 0);
    submit_sock_init(&second, &port, second_fd, old_mask);

    original_submit = g_ntdll.NtDeviceIoControlFile;
    g_ntdll.NtDeviceIoControlFile = submit_stub;
    stub_installed = 1;
    g_submit_calls = 0;
    if (ep_fault_configure(EP_FAULT_AFD_SUBMIT, 2, EAGAIN) != 0)
        goto cleanup;

    if (ep_afd_poll_submit(&first, AFD_POLL_RECEIVE, NULL) != 0 ||
        atomic_load_explicit(&first.poll_status,
                             memory_order_relaxed) != EP_POLL_PENDING ||
        g_submit_calls != 1) {
        goto cleanup;
    }

    errno = 0;
    if (ep_afd_poll_submit(&second, AFD_POLL_SEND, NULL) != -1 ||
        errno != EAGAIN ||
        atomic_load_explicit(&second.poll_status,
                             memory_order_relaxed) != EP_POLL_IDLE ||
        second.submitted_afd_events != old_mask || g_submit_calls != 1 ||
        ep_fault_hits(EP_FAULT_AFD_SUBMIT) != 2) {
        goto cleanup;
    }

    ep_fault_reset();
    if (ep_afd_poll_submit(&second, AFD_POLL_SEND, NULL) != 0 ||
        atomic_load_explicit(&second.poll_status,
                             memory_order_relaxed) != EP_POLL_PENDING ||
        second.submitted_afd_events != AFD_POLL_SEND ||
        g_submit_calls != 2 ||
        ep_fault_hits(EP_FAULT_AFD_SUBMIT) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (stub_installed)
        g_ntdll.NtDeviceIoControlFile = original_submit;
    free(first.afd_info);
    free(second.afd_info);
    if (first_fd != INVALID_SOCKET)
        (void)closesocket(first_fd);
    if (second_fd != INVALID_SOCKET)
        (void)closesocket(second_fd);
    ep_fault_reset();
    return result;
}

static int g_cancel_calls;

static NTSTATUS NTAPI cancel_stub(HANDLE file_handle,
                                  PIO_STATUS_BLOCK request,
                                  PIO_STATUS_BLOCK status)
{
    (void)file_handle;
    (void)request;
    (void)status;
    g_cancel_calls++;
    return STATUS_SUCCESS;
}

static void cancel_sock_init(ep_sock_t *sock, ep_port_t *port)
{
    memset(sock, 0, sizeof(*sock));
    sock->port = port;
    sock->io_status_block.Status = STATUS_PENDING;
    atomic_init(&sock->poll_status, EP_POLL_PENDING);
}

static int test_afd_cancel(void)
{
    ep_port_t port;
    ep_sock_t first;
    ep_sock_t second;
    PNtCancelIoFileEx original_cancel;
    int result = -1;

    memset(&port, 0, sizeof(port));
    port.afd = (HANDLE)(uintptr_t)1;
    cancel_sock_init(&first, &port);
    cancel_sock_init(&second, &port);
    ep_fault_reset();
    if (ep_global_init() != 0)
        return -1;

    original_cancel = g_ntdll.NtCancelIoFileEx;
    g_ntdll.NtCancelIoFileEx = cancel_stub;
    g_cancel_calls = 0;
    if (ep_fault_configure(EP_FAULT_AFD_CANCEL, 2, EBUSY) != 0)
        goto cleanup;

    if (ep_afd_cancel(&first) != 0 ||
        atomic_load_explicit(&first.poll_status,
                             memory_order_relaxed) != EP_POLL_CANCELLED ||
        g_cancel_calls != 1) {
        goto cleanup;
    }

    errno = 0;
    if (ep_afd_cancel(&second) != -1 || errno != EBUSY ||
        atomic_load_explicit(&second.poll_status,
                             memory_order_relaxed) != EP_POLL_PENDING ||
        g_cancel_calls != 1 ||
        ep_fault_hits(EP_FAULT_AFD_CANCEL) != 2) {
        goto cleanup;
    }

    ep_fault_reset();
    if (ep_afd_cancel(&second) != 0 ||
        atomic_load_explicit(&second.poll_status,
                             memory_order_relaxed) != EP_POLL_CANCELLED ||
        g_cancel_calls != 2 ||
        ep_fault_hits(EP_FAULT_AFD_CANCEL) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    g_ntdll.NtCancelIoFileEx = original_cancel;
    ep_fault_reset();
    return result;
}

static int test_endpoint_identity(void)
{
#ifdef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    puts("endpoint-identity: skipped by synchronized lifetime contract");
    return 0;
#else
    SOCKET socket_fd = INVALID_SOCKET;
    uint64_t endpoint_id = 0;
    int query_result;
    int result = -1;

    ep_fault_reset();
    if (ep_global_init() != 0)
        return -1;
    socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd == INVALID_SOCKET ||
        ep_fault_configure(EP_FAULT_ENDPOINT_IDENTITY, 2, EIO) != 0) {
        goto cleanup;
    }

    query_result = ep_socket_get_endpoint_id(socket_fd, &endpoint_id);
    if (query_result < 0)
        goto cleanup;

    endpoint_id = UINT64_C(0x123456789abcdef0);
    errno = 0;
    if (ep_socket_get_endpoint_id(socket_fd, &endpoint_id) != -1 ||
        errno != EIO || endpoint_id != UINT64_C(0x123456789abcdef0) ||
        ep_fault_hits(EP_FAULT_ENDPOINT_IDENTITY) != 2) {
        goto cleanup;
    }

    ep_fault_reset();
    query_result = ep_socket_get_endpoint_id(socket_fd, &endpoint_id);
    if (query_result >= 0 &&
        ep_fault_hits(EP_FAULT_ENDPOINT_IDENTITY) == 0) {
        result = 0;
    }

cleanup:
    if (socket_fd != INVALID_SOCKET)
        (void)closesocket(socket_fd);
    ep_fault_reset();
    return result;
#endif
}

static int make_udp_receiver(SOCKET *receiver_out,
                             struct sockaddr_in *address_out)
{
    SOCKET receiver = INVALID_SOCKET;
    int address_length = (int)sizeof(*address_out);

    memset(address_out, 0, sizeof(*address_out));
    address_out->sin_family = AF_INET;
    address_out->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address_out->sin_port = htons(0);
    receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (receiver == INVALID_SOCKET ||
        bind(receiver, (const struct sockaddr *)address_out,
             (int)sizeof(*address_out)) == SOCKET_ERROR ||
        getsockname(receiver, (struct sockaddr *)address_out,
                    &address_length) == SOCKET_ERROR) {
        if (receiver != INVALID_SOCKET) closesocket(receiver);
        return -1;
    }
    *receiver_out = receiver;
    return 0;
}

static int test_endpoint_policy(void)
{
#ifdef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    puts("endpoint-policy: skipped by synchronized lifetime contract");
    return 0;
#else
    struct sockaddr_in address;
    struct epoll_event event;
    wepoll_ex_stats stats;
    SOCKET receiver = INVALID_SOCKET;
    int epfd = -1;
    int result = -1;

    ep_fault_reset();
    if (ep_global_init() != 0 ||
        make_udp_receiver(&receiver, &address) != 0 ||
        (epfd = epoll_create1(0)) < 0 ||
        ep_fault_configure(EP_FAULT_ENDPOINT_UNAVAILABLE, 1,
                           EOPNOTSUPP) != 0) {
        goto cleanup;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = UINT64_C(0x1234);
    errno = 0;
#ifdef WEPOLL_EX_STRICT_SOCKET_IDENTITY
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, receiver, &event) != -1 ||
        errno != EOPNOTSUPP || epoll_fd_count(epfd) != 0 ||
        wepoll_ex_get_stats(epfd, &stats, sizeof(stats)) != 0 ||
        stats.socket_lifetime_policy != WEPOLL_EX_SOCKET_LIFETIME_STRICT ||
        stats.identity_failures == 0) {
        goto cleanup;
    }
#else
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, receiver, &event) != 0 ||
        epoll_fd_count(epfd) != 1 ||
        wepoll_ex_get_stats(epfd, &stats, sizeof(stats)) != 0 ||
        stats.socket_lifetime_policy !=
            WEPOLL_EX_SOCKET_LIFETIME_BEST_EFFORT ||
        epoll_ctl(epfd, EPOLL_CTL_DEL, receiver, NULL) != 0) {
        goto cleanup;
    }
#endif
    if (ep_fault_hits(EP_FAULT_ENDPOINT_UNAVAILABLE) != 1) goto cleanup;
    result = 0;

cleanup:
    ep_fault_reset();
    if (epfd >= 0 && wepoll_close(epfd) != 0) result = -1;
    if (receiver != INVALID_SOCKET) closesocket(receiver);
    return result;
#endif
}

static int test_iocp_create(void)
{
    ep_port_t *first = NULL;
    ep_port_t *second = NULL;
    int result = -1;

    ep_fault_reset();
    if (ep_fault_configure(EP_FAULT_IOCP_CREATE, 2, EMFILE) != 0 ||
        ep_port_create(0, 0, &first) != 0) {
        goto cleanup;
    }
    errno = 0;
    if (ep_port_create(0, 0, &second) != -1 || errno != EMFILE ||
        second != NULL || ep_fault_hits(EP_FAULT_IOCP_CREATE) != 2) {
        goto cleanup;
    }
    ep_fault_reset();
    if (ep_port_create(0, 0, &second) != 0 || second == NULL) goto cleanup;
    result = 0;

cleanup:
    ep_fault_reset();
    if (first != NULL && ep_port_destroy(first) != 0) result = -1;
    if (second != NULL && ep_port_destroy(second) != 0) result = -1;
    return result;
}

static int test_iocp_post(void)
{
    ep_port_t *first = NULL;
    ep_port_t *second = NULL;
    int result = -1;

    ep_fault_reset();
    if (ep_port_create(0, 0, &first) != 0 ||
        ep_port_create(0, 0, &second) != 0 ||
        ep_fault_configure(EP_FAULT_IOCP_POST, 2, EIO) != 0) {
        goto cleanup;
    }
    ep_port_begin_close(first);
    if (atomic_load_explicit(&first->iocp_closed,
                             memory_order_relaxed) != 0) {
        goto cleanup;
    }
    ep_port_begin_close(second);
    if (atomic_load_explicit(&second->iocp_closed,
                             memory_order_relaxed) == 0 ||
        ep_fault_hits(EP_FAULT_IOCP_POST) != 2) {
        goto cleanup;
    }
    result = 0;

cleanup:
    ep_fault_reset();
    if (first != NULL && ep_port_destroy(first) != 0) result = -1;
    if (second != NULL && ep_port_destroy(second) != 0) result = -1;
    return result;
}

static int test_iocp_dequeue(void)
{
    ep_port_t *port = NULL;
    epoll_event_ex event;
    int result = -1;

    ep_fault_reset();
    if (ep_port_create(0, 0, &port) != 0 ||
        ep_fault_configure(EP_FAULT_IOCP_DEQUEUE, 2, EIO) != 0 ||
        ep_port_wait(port, &event, 1, 0, NULL) != 0) {
        goto cleanup;
    }
    errno = 0;
    if (ep_port_wait(port, &event, 1, 0, NULL) != -1 || errno != EIO ||
        ep_fault_hits(EP_FAULT_IOCP_DEQUEUE) != 2) {
        goto cleanup;
    }
    ep_fault_reset();
    if (ep_port_wait(port, &event, 1, 0, NULL) != 0) goto cleanup;
    result = 0;

cleanup:
    ep_fault_reset();
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    return result;
}

static int test_ready_node_alloc(void)
{
    static const char first_byte = 'a';
    static const char second_byte = 'b';
    struct sockaddr_in address;
    struct epoll_event event;
    wepoll_ex_stats stats;
    SOCKET receiver = INVALID_SOCKET;
    SOCKET sender = INVALID_SOCKET;
    int epfd = -1;
    int result = -1;
    char received;

    ep_fault_reset();
    if (ep_global_init() != 0 ||
        make_udp_receiver(&receiver, &address) != 0 ||
        (sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET ||
        (epfd = epoll_create1(0)) < 0) {
        goto cleanup;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = UINT64_C(0xabcdef);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, receiver, &event) != 0 ||
        ep_fault_configure(EP_FAULT_READY_NODE_ALLOC, 2, ENOMEM) != 0 ||
        sendto(sender, &first_byte, 1, 0, (const struct sockaddr *)&address,
               (int)sizeof(address)) != 1 ||
        epoll_wait(epfd, &event, 1, 2000) != 1 ||
        recv(receiver, &received, 1, 0) != 1 || received != first_byte ||
        sendto(sender, &second_byte, 1, 0, (const struct sockaddr *)&address,
               (int)sizeof(address)) != 1) {
        goto cleanup;
    }
    errno = 0;
    if (epoll_wait(epfd, &event, 1, 2000) != -1 || errno != ENOMEM ||
        ep_fault_hits(EP_FAULT_READY_NODE_ALLOC) != 2 ||
        wepoll_ex_get_stats(epfd, &stats, sizeof(stats)) != 0 ||
        stats.asynchronous_errors == 0 || stats.rearm_queue_depth == 0) {
        goto cleanup;
    }
    ep_fault_reset();
    if (epoll_wait(epfd, &event, 1, 2000) != 1 ||
        event.data.u64 != UINT64_C(0xabcdef) ||
        recv(receiver, &received, 1, 0) != 1 || received != second_byte) {
        goto cleanup;
    }
    result = 0;

cleanup:
    ep_fault_reset();
    if (epfd >= 0 && wepoll_close(epfd) != 0) result = -1;
    if (sender != INVALID_SOCKET) closesocket(sender);
    if (receiver != INVALID_SOCKET) closesocket(receiver);
    return result;
}

static int test_aux_waitable_disarm(void)
{
    HANDLE event_handle = NULL;
    struct epoll_event event;
    wepoll_ex_stats stats;
    int epfd = -1;
    int result = -1;

    ep_fault_reset();
    event_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    epfd = epoll_create1(0);
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (event_handle == NULL || epfd < 0 ||
        epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)event_handle, &event) != 0 ||
        epoll_wait(epfd, &event, 1, 0) != 0 ||
        ep_fault_configure(EP_FAULT_AUX_DISARM, 1, EBUSY) != 0 ||
        !SetEvent(event_handle)) {
        goto cleanup;
    }

    errno = 0;
    if (epoll_wait(epfd, &event, 1, 1000) != -1 || errno != EBUSY ||
        ep_fault_hits(EP_FAULT_AUX_DISARM) != 1 ||
        wepoll_ex_get_stats(epfd, &stats, sizeof(stats)) != 0 ||
        stats.active_registrations != 1 || stats.pending_polls != 1 ||
        stats.rearm_queue_depth != 1 || stats.asynchronous_errors == 0) {
        goto cleanup;
    }

    if (epoll_wait(epfd, &event, 1, 1000) != 1 ||
        (event.events & EPOLLIN) == 0 ||
        ep_fault_hits(EP_FAULT_AUX_DISARM) != 2) {
        goto cleanup;
    }
    result = 0;

cleanup:
    ep_fault_reset();
    if (epfd >= 0 && wepoll_close(epfd) != 0) result = -1;
    if (event_handle != NULL) CloseHandle(event_handle);
    return result;
}

static int test_aux_waitable_disarm_repeat(void)
{
    HANDLE event_handle = NULL;
    struct epoll_event event;
    wepoll_ex_stats stats;
    int epfd = -1;
    int result = -1;

    ep_fault_reset();
    event_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    epfd = epoll_create1(0);
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (event_handle == NULL || epfd < 0 ||
        epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)event_handle, &event) != 0 ||
        epoll_wait(epfd, &event, 1, 0) != 0 ||
        ep_fault_configure(EP_FAULT_AUX_DISARM, 1, EBUSY) != 0 ||
        !SetEvent(event_handle)) {
        goto cleanup;
    }
    errno = 0;
    if (epoll_wait(epfd, &event, 1, 1000) != -1 || errno != EBUSY ||
        ep_fault_configure(EP_FAULT_AUX_DISARM, 1, EBUSY) != 0) {
        goto cleanup;
    }

    errno = 0;
    if (epoll_wait(epfd, &event, 1, 1000) != -1 || errno != EBUSY ||
        ep_fault_hits(EP_FAULT_AUX_DISARM) != 1 ||
        epoll_fd_count(epfd) != 0 ||
        wepoll_ex_get_stats(epfd, &stats, sizeof(stats)) != 0 ||
        stats.active_registrations != 0 || stats.pending_polls != 1 ||
        stats.rearm_queue_depth != 0) {
        goto cleanup;
    }

    ep_fault_reset();
    if (wepoll_close(epfd) != 0)
        goto cleanup;
    epfd = -1;
    result = 0;

cleanup:
    ep_fault_reset();
    if (epfd >= 0 && wepoll_close(epfd) != 0) result = -1;
    if (event_handle != NULL) CloseHandle(event_handle);
    return result;
}

static int test_aux_pipe_disarm(void)
{
    HANDLE read_handle = NULL;
    HANDLE write_handle = NULL;
    struct epoll_event event;
    wepoll_ex_stats stats;
    DWORD transferred = 0;
    char byte = 'p';
    int epfd = -1;
    int result = -1;

    ep_fault_reset();
    epfd = epoll_create1(0);
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (epfd < 0 || !CreatePipe(&read_handle, &write_handle, NULL, 0) ||
        epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)read_handle, &event) != 0 ||
        epoll_wait(epfd, &event, 1, 0) != 0 ||
        ep_fault_configure(EP_FAULT_AUX_DISARM, 1, EBUSY) != 0 ||
        !WriteFile(write_handle, &byte, 1, &transferred, NULL) ||
        transferred != 1) {
        goto cleanup;
    }

    errno = 0;
    if (epoll_wait(epfd, &event, 1, 1000) != -1 || errno != EBUSY ||
        ep_fault_hits(EP_FAULT_AUX_DISARM) != 1 ||
        wepoll_ex_get_stats(epfd, &stats, sizeof(stats)) != 0 ||
        stats.active_registrations != 1 || stats.pending_polls != 1 ||
        stats.rearm_queue_depth != 1) {
        goto cleanup;
    }
    if (epoll_wait(epfd, &event, 1, 1000) != 1 ||
        (event.events & EPOLLIN) == 0 ||
        ep_fault_hits(EP_FAULT_AUX_DISARM) != 2 ||
        !ReadFile(read_handle, &byte, 1, &transferred, NULL) ||
        transferred != 1) {
        goto cleanup;
    }
    result = 0;

cleanup:
    ep_fault_reset();
    if (epfd >= 0 && wepoll_close(epfd) != 0) result = -1;
    if (write_handle != NULL) CloseHandle(write_handle);
    if (read_handle != NULL) CloseHandle(read_handle);
    return result;
}

typedef int (*fault_test_fn)(void);

typedef struct fault_test_case {
    const char *name;
    fault_test_fn run;
} fault_test_case_t;

static const fault_test_case_t g_tests[] = {
    { "framework", test_framework },
    { "pool-init", test_pool_init_alloc },
    { "pool-grow", test_pool_growth },
    { "provider-base", test_provider_base },
    { "afd-open", test_afd_open },
    { "afd-submit", test_afd_submit },
    { "afd-cancel", test_afd_cancel },
    { "endpoint-identity", test_endpoint_identity },
    { "endpoint-policy", test_endpoint_policy },
    { "iocp-create", test_iocp_create },
    { "iocp-post", test_iocp_post },
    { "iocp-dequeue", test_iocp_dequeue },
    { "ready-node-alloc", test_ready_node_alloc },
    { "aux-waitable-disarm", test_aux_waitable_disarm },
    { "aux-waitable-disarm-repeat", test_aux_waitable_disarm_repeat },
    { "aux-pipe-disarm", test_aux_pipe_disarm }
};

static int run_test(const fault_test_case_t *test)
{
    int result = test->run();

    if (result == 0) {
        printf("%s: OK\n", test->name);
        return 0;
    }
    fprintf(stderr, "%s: FAILED (errno=%d WSA=%d)\n",
            test->name, errno, WSAGetLastError());
    return -1;
}

int main(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "all") == 0)) {
        for (size_t i = 0; i < sizeof(g_tests) / sizeof(g_tests[0]); i++) {
            if (run_test(&g_tests[i]) != 0)
                return 1;
        }
        return 0;
    }

    if (argc == 2) {
        for (size_t i = 0; i < sizeof(g_tests) / sizeof(g_tests[0]); i++) {
            if (strcmp(argv[1], g_tests[i].name) == 0)
                return run_test(&g_tests[i]) == 0 ? 0 : 1;
        }
    }

    fprintf(stderr,
            "usage: %s [all|framework|pool-init|pool-grow|provider-base|"
            "afd-open|afd-submit|afd-cancel|endpoint-identity|"
            "endpoint-policy|iocp-create|iocp-post|iocp-dequeue|"
            "ready-node-alloc|aux-waitable-disarm|"
            "aux-waitable-disarm-repeat|aux-pipe-disarm]\n",
            argv[0]);
    return 2;
}
