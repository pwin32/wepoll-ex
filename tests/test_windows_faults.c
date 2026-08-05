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

#define TEST_SKIP 77

#ifndef SIO_BASE_HANDLE
#define SIO_BASE_HANDLE 0x48000022
#endif
#ifndef SIO_QUERY_WFP_ALE_ENDPOINT_HANDLE
#define SIO_QUERY_WFP_ALE_ENDPOINT_HANDLE 0x580000CD
#endif

typedef struct fault_wait_context {
    int epfd;
    HANDLE started;
    int result;
    int error;
} fault_wait_context_t;

static DWORD WINAPI fault_wait_thread(void *parameter)
{
    fault_wait_context_t *context = (fault_wait_context_t *)parameter;
    struct epoll_event event;

    memset(&event, 0, sizeof(event));
    (void)SetEvent(context->started);
    errno = 0;
    context->result = epoll_wait(context->epfd, &event, 1, -1);
    context->error = errno;
    return 0;
}

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

static int test_error_info_native(void)
{
    wepoll_ex_error_info info;

    ep_set_winsock_error(WSAECONNRESET);
    if (wepoll_ex_get_last_error_info(&info, sizeof(info)) != 0 ||
        errno != ECONNRESET || info.portable_error != ECONNRESET ||
        info.native_domain != WEPOLL_EX_NATIVE_ERROR_WINSOCK ||
        info.native_code != WSAECONNRESET ||
        info.winsock_error != WSAECONNRESET ||
        (info.flags & (WEPOLL_EX_ERROR_NATIVE_EXACT |
                       WEPOLL_EX_ERROR_WINSOCK_EQUIVALENT)) !=
            (WEPOLL_EX_ERROR_NATIVE_EXACT |
             WEPOLL_EX_ERROR_WINSOCK_EQUIVALENT)) {
        return -1;
    }

    ep_set_win32_error(ERROR_ACCESS_DENIED);
    if (wepoll_ex_get_last_error_info(&info, sizeof(info)) != 0 ||
        errno != EACCES || info.portable_error != EACCES ||
        info.native_domain != WEPOLL_EX_NATIVE_ERROR_WIN32 ||
        info.native_code != ERROR_ACCESS_DENIED ||
        info.winsock_error != WSAEACCES ||
        (info.flags & (WEPOLL_EX_ERROR_NATIVE_EXACT |
                       WEPOLL_EX_ERROR_WINSOCK_EQUIVALENT)) !=
            (WEPOLL_EX_ERROR_NATIVE_EXACT |
             WEPOLL_EX_ERROR_WINSOCK_EQUIVALENT)) {
        return -1;
    }

    ep_set_ntstatus_error(STATUS_CANCELLED);
    if (wepoll_ex_get_last_error_info(&info, sizeof(info)) != 0 ||
        info.portable_error != ep_status_to_errno(STATUS_CANCELLED) ||
        info.native_domain != WEPOLL_EX_NATIVE_ERROR_NTSTATUS ||
        info.native_code != (uint32_t)STATUS_CANCELLED ||
        (info.flags & WEPOLL_EX_ERROR_NATIVE_EXACT) == 0) {
        return -1;
    }
    return 0;
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
static NTSTATUS g_submit_status = STATUS_PENDING;

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
    return g_submit_status;
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
    ep_afd_poll_key_release(&first);
    ep_afd_poll_key_release(&second);
    free(first.afd_info);
    free(second.afd_info);
    if (first_fd != INVALID_SOCKET)
        (void)closesocket(first_fd);
    if (second_fd != INVALID_SOCKET)
        (void)closesocket(second_fd);
    ep_fault_reset();
    return result;
}

#define AFD_KEY_UNRESERVED_OWNER_COUNT 9
#define AFD_KEY_OWNER_COUNT (AFD_KEY_UNRESERVED_OWNER_COUNT + 1)
#define AFD_KEY_SOCK_COUNT (AFD_KEY_OWNER_COUNT + 1)

static HANDLE g_afd_key_captured[AFD_KEY_SOCK_COUNT];
static int g_afd_key_capture_count;
static int g_afd_key_capture_invalid;

static NTSTATUS NTAPI afd_key_submit_stub(
    HANDLE file_handle, HANDLE event, PIO_APC_ROUTINE apc_routine,
    PVOID apc_context, PIO_STATUS_BLOCK io_status_block, ULONG ioctl,
    PVOID input_buffer, ULONG input_length, PVOID output_buffer,
    ULONG output_length)
{
    AFD_POLL_INFO *info = (AFD_POLL_INFO *)input_buffer;

    (void)file_handle;
    (void)event;
    (void)apc_routine;
    (void)apc_context;
    (void)io_status_block;
    (void)output_buffer;
    (void)output_length;

    if (ioctl != IOCTL_AFD_POLL || info == NULL ||
        input_length < sizeof(*info) || info->NumberOfHandles != 1 ||
        g_afd_key_capture_count >= AFD_KEY_SOCK_COUNT) {
        g_afd_key_capture_invalid = 1;
    } else {
        g_afd_key_captured[g_afd_key_capture_count] =
            info->Handles[0].Handle;
    }
    g_afd_key_capture_count++;

    /* The optional reservation bookkeeping must preserve the status/error
     * state left by the actual submit call, including when its fault hook
     * suppresses CreateEventW. */
    ep_set_errno(ERANGE);
    SetLastError(ERROR_BUSY);
    return STATUS_PENDING;
}

static int afd_key_owner_valid(const ep_sock_t *sock, HANDLE key)
{
    return sock->afd_poll_key_owned != 0 &&
           sock->afd_poll_target == key &&
           sock->afd_poll_key_reservation == NULL &&
           atomic_load_explicit(&sock->poll_status,
                                memory_order_relaxed) == EP_POLL_PENDING;
}

static int afd_key_released(const ep_sock_t *sock)
{
    return sock->afd_poll_key_owned == 0 &&
           sock->afd_poll_target == NULL &&
           sock->afd_poll_key_reservation == NULL &&
           sock->afd_poll_key_next == NULL;
}

static int test_afd_key_fallback(void)
{
    ep_port_t port;
    ep_sock_t socks[AFD_KEY_SOCK_COUNT];
    SOCKET socket_fd = INVALID_SOCKET;
    PNtDeviceIoControlFile original_submit = NULL;
    DWORD baseline_handles = 0;
    DWORD handles_after = 0;
    const char *failure_stage = "initialization";
    size_t failure_index = 0;
    int stub_installed = 0;
    int result = -1;
    const uint32_t failed_old_mask = UINT32_C(0x5a5a);

    memset(&port, 0, sizeof(port));
    memset(socks, 0, sizeof(socks));
    memset(g_afd_key_captured, 0, sizeof(g_afd_key_captured));
    port.afd = (HANDLE)(uintptr_t)1;
    ep_fault_reset();
    failure_stage = "global-init";
    if (ep_global_init() != 0)
        goto cleanup;

    failure_stage = "socket-create";
    socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd == INVALID_SOCKET)
        goto cleanup;
    for (size_t i = 0; i < AFD_KEY_SOCK_COUNT; i++)
        submit_sock_init(&socks[i], &port, socket_fd, 0);
    socks[AFD_KEY_OWNER_COUNT].submitted_afd_events = failed_old_mask;

    original_submit = g_ntdll.NtDeviceIoControlFile;
    g_ntdll.NtDeviceIoControlFile = afd_key_submit_stub;
    stub_installed = 1;
    g_afd_key_capture_count = 0;
    g_afd_key_capture_invalid = 0;

    failure_stage = "first-submit";
    if (ep_afd_poll_submit(&socks[0], AFD_POLL_RECEIVE, NULL) != 0 ||
        g_afd_key_capture_count != 1 || g_afd_key_capture_invalid ||
        !afd_key_owner_valid(&socks[0], g_afd_key_captured[0])) {
        goto cleanup;
    }
    failure_stage = "baseline-handle-count";
    if (!GetProcessHandleCount(GetCurrentProcess(), &baseline_handles))
        goto cleanup;

    /* Leave nine duplicate target slots deliberately unreserved.  Force the
     * first i duplicate candidates through the collision path so this covers
     * fallback traversal and both scratch-array capacities independently of
     * Windows handle-allocation policy.  The submit stub's errno proves the
     * reservation fault remains optional bookkeeping rather than leaking its
     * injected error. */
    for (size_t i = 1; i < AFD_KEY_OWNER_COUNT; i++) {
        failure_stage = "owner-submit";
        failure_index = i;
        ep_fault_reset();
        if (ep_fault_configure(EP_FAULT_AFD_KEY_FORCE_COLLISION,
                               i, EEXIST) != 0 ||
            ep_fault_configure(EP_FAULT_AFD_KEY_RESERVATION,
                               1, EACCES) != 0) {
            goto cleanup;
        }
        errno = 0;
        if (ep_afd_poll_submit(&socks[i], AFD_POLL_RECEIVE, NULL) != 0 ||
            errno != ERANGE ||
            ep_fault_hits(EP_FAULT_AFD_KEY_RESERVATION) != 1 ||
            g_afd_key_capture_count != (int)i + 1 ||
            g_afd_key_capture_invalid ||
            !afd_key_owner_valid(&socks[i], g_afd_key_captured[i])) {
            goto cleanup;
        }
        for (size_t j = 0; j < i; j++) {
            if (g_afd_key_captured[j] == g_afd_key_captured[i])
                goto cleanup;
        }
    }

    failure_stage = "owner-handle-count";
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles_after) ||
        handles_after != baseline_handles) {
        goto cleanup;
    }

    /* Force nine collisions so the scratch array must grow from eight to
     * sixteen.  Fail that second growth: the current duplicate and all eight
     * retained duplicates must close, the candidate must remain wholly
     * unowned, and the established owners must remain unchanged. */
    failure_stage = "growth-failure";
    failure_index = AFD_KEY_OWNER_COUNT;
    ep_fault_reset();
    if (ep_fault_configure(EP_FAULT_AFD_KEY_FORCE_COLLISION,
                           9, EEXIST) != 0 ||
        ep_fault_configure(EP_FAULT_AFD_KEY_COLLISION_GROW,
                           2, ENOSPC) != 0) {
        goto cleanup;
    }
    errno = 0;
    if (ep_afd_poll_submit(&socks[AFD_KEY_OWNER_COUNT],
                           AFD_POLL_SEND, NULL) != -1 ||
        errno != ENOSPC ||
        ep_fault_hits(EP_FAULT_AFD_KEY_FORCE_COLLISION) != 9 ||
        ep_fault_hits(EP_FAULT_AFD_KEY_COLLISION_GROW) != 2 ||
        g_afd_key_capture_count != AFD_KEY_OWNER_COUNT ||
        atomic_load_explicit(&socks[AFD_KEY_OWNER_COUNT].poll_status,
                             memory_order_relaxed) != EP_POLL_IDLE ||
        socks[AFD_KEY_OWNER_COUNT].submitted_afd_events != failed_old_mask ||
        !afd_key_released(&socks[AFD_KEY_OWNER_COUNT])) {
        goto cleanup;
    }
    failure_stage = "growth-failure-handle-count";
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles_after) ||
        handles_after != baseline_handles) {
        goto cleanup;
    }
    failure_stage = "growth-failure-owner-validation";
    for (size_t i = 0; i < AFD_KEY_OWNER_COUNT; i++) {
        failure_index = i;
        if (!afd_key_owner_valid(&socks[i], g_afd_key_captured[i]))
            goto cleanup;
    }

    /* A clean retry must walk the still-owned keys and capture one more
     * distinct target, demonstrating that failure inserted no dangling key
     * and removed none of the preceding owners. */
    failure_stage = "retry-submit";
    failure_index = AFD_KEY_OWNER_COUNT;
    ep_fault_reset();
    if (ep_fault_configure(EP_FAULT_AFD_KEY_RESERVATION,
                           1, EACCES) != 0) {
        goto cleanup;
    }
    errno = 0;
    if (ep_afd_poll_submit(&socks[AFD_KEY_OWNER_COUNT],
                           AFD_POLL_SEND, NULL) != 0 ||
        errno != ERANGE ||
        ep_fault_hits(EP_FAULT_AFD_KEY_RESERVATION) != 1 ||
        g_afd_key_capture_count != AFD_KEY_SOCK_COUNT ||
        g_afd_key_capture_invalid ||
        !afd_key_owner_valid(&socks[AFD_KEY_OWNER_COUNT],
                             g_afd_key_captured[AFD_KEY_OWNER_COUNT])) {
        goto cleanup;
    }
    failure_stage = "retry-uniqueness";
    for (size_t i = 0; i < AFD_KEY_OWNER_COUNT; i++) {
        failure_index = i;
        if (g_afd_key_captured[i] ==
            g_afd_key_captured[AFD_KEY_OWNER_COUNT]) {
            goto cleanup;
        }
    }
    failure_stage = "retry-handle-count";
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles_after) ||
        handles_after != baseline_handles) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (result != 0) {
        fprintf(stderr,
                "afd-key-fallback stage=%s index=%zu captured=%d invalid=%d "
                "baseline=%lu after=%lu candidate-status=%d "
                "candidate-events=%lu candidate-owned=%u fault-hits=%zu\n",
                failure_stage,
                failure_index,
                g_afd_key_capture_count,
                g_afd_key_capture_invalid,
                (unsigned long)baseline_handles,
                (unsigned long)handles_after,
                (int)atomic_load_explicit(
                    &socks[AFD_KEY_OWNER_COUNT].poll_status,
                    memory_order_relaxed),
                (unsigned long)socks[AFD_KEY_OWNER_COUNT]
                    .submitted_afd_events,
                (unsigned int)socks[AFD_KEY_OWNER_COUNT]
                    .afd_poll_key_owned,
                (size_t)ep_fault_hits(
                    EP_FAULT_AFD_KEY_COLLISION_GROW));
    }
    if (stub_installed)
        g_ntdll.NtDeviceIoControlFile = original_submit;
    ep_fault_reset();
    for (size_t i = 0; i < AFD_KEY_SOCK_COUNT; i++) {
        ep_afd_poll_key_release(&socks[i]);
        if (!afd_key_released(&socks[i]))
            result = -1;
        free(socks[i].afd_info);
    }
    if (socket_fd != INVALID_SOCKET) {
        if (baseline_handles != 0 &&
            (!GetProcessHandleCount(GetCurrentProcess(), &handles_after) ||
             handles_after != baseline_handles)) {
            result = -1;
        }
        (void)closesocket(socket_fd);
    }
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

#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
typedef struct endpoint_ioctl_state {
    int ioctl_result;
    int wsa_error;
    uint64_t result;
    DWORD bytes;
    int calls;
    SOCKET seen_socket;
    DWORD seen_ioctl;
    DWORD seen_result_size;
} endpoint_ioctl_state_t;

static int endpoint_ioctl_stub(
    SOCKET socket_fd, DWORD ioctl, uint64_t *result_out, DWORD result_size,
    DWORD *bytes_out, int *error_out, void *context)
{
    endpoint_ioctl_state_t *state = (endpoint_ioctl_state_t *)context;

    state->calls++;
    state->seen_socket = socket_fd;
    state->seen_ioctl = ioctl;
    state->seen_result_size = result_size;
    if (result_out != NULL)
        *result_out = state->result;
    if (bytes_out != NULL)
        *bytes_out = state->bytes;
    if (error_out != NULL)
        *error_out = state->wsa_error;
    return state->ioctl_result;
}

static int test_endpoint_ioctl_contract(void)
{
    static const int unsupported_errors[] = {
        WSAEOPNOTSUPP,
        WSAENOPROTOOPT,
        WSAEINVAL
    };
    static const struct {
        int wsa_error;
        int expected_errno;
    } hard_errors[] = {
        { WSAENOTSOCK, ENOTSOCK },
        { WSAEACCES, EACCES }
    };
    static const DWORD malformed_sizes[] = {
        (DWORD)sizeof(uint64_t) - 1,
        (DWORD)sizeof(uint64_t) + 1
    };
    const SOCKET fake_socket = (SOCKET)100;
    const uint64_t sentinel = UINT64_C(0x1122334455667788);
    const uint64_t token = UINT64_C(0xfedcba9876543210);
    endpoint_ioctl_state_t state;
    uint64_t endpoint_id;
    int result = -1;

    ep_fault_reset();

    memset(&state, 0, sizeof(state));
    state.result = token;
    state.bytes = (DWORD)sizeof(state.result);
    endpoint_id = sentinel;
    if (ep_socket_get_endpoint_id_with_ioctl(
            fake_socket, &endpoint_id, endpoint_ioctl_stub, &state) != 1 ||
        endpoint_id != token || state.calls != 1 ||
        state.seen_socket != fake_socket ||
        state.seen_ioctl != SIO_QUERY_WFP_ALE_ENDPOINT_HANDLE ||
        state.seen_result_size != (DWORD)sizeof(uint64_t)) {
        goto cleanup;
    }

    for (size_t i = 0;
         i < sizeof(unsupported_errors) / sizeof(unsupported_errors[0]); i++) {
        memset(&state, 0, sizeof(state));
        state.ioctl_result = SOCKET_ERROR;
        state.wsa_error = unsupported_errors[i];
        state.result = token;
        state.bytes = (DWORD)sizeof(state.result);
        endpoint_id = sentinel;
        errno = EINTR;
        if (ep_socket_get_endpoint_id_with_ioctl(
                fake_socket, &endpoint_id, endpoint_ioctl_stub, &state) != 0 ||
            endpoint_id != sentinel || state.calls != 1) {
            goto cleanup;
        }
    }

    for (size_t i = 0;
         i < sizeof(hard_errors) / sizeof(hard_errors[0]); i++) {
        memset(&state, 0, sizeof(state));
        state.ioctl_result = SOCKET_ERROR;
        state.wsa_error = hard_errors[i].wsa_error;
        state.result = token;
        state.bytes = (DWORD)sizeof(state.result);
        endpoint_id = sentinel;
        errno = 0;
        if (ep_socket_get_endpoint_id_with_ioctl(
                fake_socket, &endpoint_id, endpoint_ioctl_stub, &state) != -1 ||
            errno != hard_errors[i].expected_errno ||
            endpoint_id != sentinel || state.calls != 1) {
            goto cleanup;
        }
    }

    for (size_t i = 0;
         i < sizeof(malformed_sizes) / sizeof(malformed_sizes[0]); i++) {
        memset(&state, 0, sizeof(state));
        state.result = token;
        state.bytes = malformed_sizes[i];
        endpoint_id = sentinel;
        errno = 0;
        if (ep_socket_get_endpoint_id_with_ioctl(
                fake_socket, &endpoint_id, endpoint_ioctl_stub, &state) != -1 ||
            errno != EIO || endpoint_id != sentinel || state.calls != 1) {
            goto cleanup;
        }
    }

    memset(&state, 0, sizeof(state));
    endpoint_id = sentinel;
    errno = 0;
    if (ep_socket_get_endpoint_id_with_ioctl(
            INVALID_SOCKET, &endpoint_id, endpoint_ioctl_stub, &state) != -1 ||
        errno != ENOTSOCK || endpoint_id != sentinel || state.calls != 0) {
        goto cleanup;
    }
    errno = 0;
    if (ep_socket_get_endpoint_id_with_ioctl(
            fake_socket, NULL, endpoint_ioctl_stub, &state) != -1 ||
        errno != EFAULT || state.calls != 0) {
        goto cleanup;
    }
    errno = 0;
    if (ep_socket_get_endpoint_id_with_ioctl(
            fake_socket, &endpoint_id, NULL, &state) != -1 ||
        errno != EFAULT || endpoint_id != sentinel || state.calls != 0) {
        goto cleanup;
    }

    memset(&state, 0, sizeof(state));
    state.result = token;
    state.bytes = (DWORD)sizeof(state.result);
    if (ep_fault_configure(EP_FAULT_ENDPOINT_UNAVAILABLE, 1,
                           EOPNOTSUPP) != 0) {
        goto cleanup;
    }
    endpoint_id = sentinel;
    errno = EINTR;
    if (ep_socket_get_endpoint_id_with_ioctl(
            fake_socket, &endpoint_id, endpoint_ioctl_stub, &state) != 0 ||
        errno != 0 || endpoint_id != sentinel || state.calls != 0 ||
        ep_fault_hits(EP_FAULT_ENDPOINT_UNAVAILABLE) != 1) {
        goto cleanup;
    }

    ep_fault_reset();
    if (ep_fault_configure(EP_FAULT_ENDPOINT_IDENTITY, 1, EIO) != 0)
        goto cleanup;
    endpoint_id = sentinel;
    errno = 0;
    if (ep_socket_get_endpoint_id_with_ioctl(
            fake_socket, &endpoint_id, endpoint_ioctl_stub, &state) != -1 ||
        errno != EIO || endpoint_id != sentinel || state.calls != 0 ||
        ep_fault_hits(EP_FAULT_ENDPOINT_IDENTITY) != 1) {
        goto cleanup;
    }

    result = 0;

cleanup:
    ep_fault_reset();
    return result;
}
#endif

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

    if (test_endpoint_ioctl_contract() != 0)
        return -1;

    ep_fault_reset();
    if (ep_global_init() != 0)
        return -1;
    socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd == INVALID_SOCKET) {
        goto cleanup;
    }

    query_result = ep_socket_get_endpoint_id(socket_fd, &endpoint_id);
    if (query_result != 0 && query_result != 1)
        goto cleanup;
    result = 0;

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

static int make_tcp_pair(SOCKET *listener_out, SOCKET *client_out,
                         SOCKET *server_out)
{
    struct sockaddr_in address;
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET server = INVALID_SOCKET;
    int address_length = (int)sizeof(address);

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET ||
        bind(listener, (const struct sockaddr *)&address,
             (int)sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR ||
        getsockname(listener, (struct sockaddr *)&address,
                    &address_length) == SOCKET_ERROR) {
        goto fail;
    }
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET ||
        connect(client, (const struct sockaddr *)&address,
                address_length) == SOCKET_ERROR) {
        goto fail;
    }
    server = accept(listener, NULL, NULL);
    if (server == INVALID_SOCKET) goto fail;

    *listener_out = listener;
    *client_out = client;
    *server_out = server;
    return 0;

fail:
    if (server != INVALID_SOCKET) closesocket(server);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return -1;
}

static int wait_for_empty_public_port(int epfd)
{
    ULONGLONG deadline = GetTickCount64() + 2000;
    struct epoll_event event;
    wepoll_ex_stats stats;

    for (;;) {
        memset(&stats, 0, sizeof(stats));
        if (epoll_fd_count(epfd) != 0 ||
            wepoll_ex_get_stats(epfd, &stats, sizeof(stats)) != 0) {
            return -1;
        }
        if (stats.active_registrations == 0 && stats.pending_polls == 0 &&
            stats.rearm_queue_depth == 0 &&
            stats.oneshot_probe_queue_depth == 0 &&
            stats.ready_queue_depth == 0 && stats.afd_pool_in_use == 0 &&
            stats.ready_pool_in_use == 0) {
            return 0;
        }
        if (GetTickCount64() >= deadline)
            return -1;

        memset(&event, 0, sizeof(event));
        if (epoll_wait(epfd, &event, 1, 10) != 0)
            return -1;
    }
}

static int test_afd_submit_batch(void)
{
    struct sockaddr_in addresses[2];
    struct epoll_event events[2];
    wepoll_ex_stats stats;
    SOCKET receivers[2] = { INVALID_SOCKET, INVALID_SOCKET };
    epoll_fd_t fds[2];
    int add_ops[2] = { EPOLL_CTL_ADD, EPOLL_CTL_ADD };
    int del_ops[2] = { EPOLL_CTL_DEL, EPOLL_CTL_DEL };
    int epfd = -1;
    int result = -1;

    ep_fault_reset();
    if (ep_global_init() != 0 ||
        make_udp_receiver(&receivers[0], &addresses[0]) != 0 ||
        make_udp_receiver(&receivers[1], &addresses[1]) != 0 ||
        (epfd = epoll_create1(0)) < 0) {
        goto cleanup;
    }

    memset(events, 0, sizeof(events));
    events[0].events = EPOLLIN;
    events[0].data.u64 = UINT64_C(0x1111222233334444);
    events[1].events = EPOLLIN;
    events[1].data.u64 = UINT64_C(0x5555666677778888);
    fds[0] = (epoll_fd_t)receivers[0];
    fds[1] = (epoll_fd_t)receivers[1];

    if (ep_fault_configure(EP_FAULT_AFD_SUBMIT, 2, EAGAIN) != 0)
        goto cleanup;
    errno = 0;
    if (epoll_ctl_batch(epfd, add_ops, fds, events, 2) != -1 ||
        errno != EAGAIN || ep_fault_hits(EP_FAULT_AFD_SUBMIT) != 2 ||
        epoll_fd_count(epfd) != 0 ||
        wait_for_empty_public_port(epfd) != 0) {
        goto cleanup;
    }

    /* A stale logical AFD key from either rolled-back ADD would force its
     * retry through the duplicate-target reservation path.  Neither clean
     * retry should touch that optional hook. */
    ep_fault_reset();
    if (ep_fault_configure(EP_FAULT_AFD_KEY_RESERVATION, 1, EBUSY) != 0 ||
        epoll_ctl_batch(epfd, add_ops, fds, events, 2) != 0 ||
        ep_fault_hits(EP_FAULT_AFD_SUBMIT) != 0 ||
        ep_fault_hits(EP_FAULT_AFD_KEY_RESERVATION) != 0 ||
        epoll_fd_count(epfd) != 2 ||
        wepoll_ex_get_stats(epfd, &stats, sizeof(stats)) != 0 ||
        stats.active_registrations != 2 || stats.pending_polls != 2 ||
        stats.rearm_queue_depth != 0 ||
        stats.oneshot_probe_queue_depth != 0 ||
        stats.ready_queue_depth != 0 || stats.afd_pool_in_use != 2 ||
        stats.ready_pool_in_use != 0) {
        goto cleanup;
    }

    ep_fault_reset();
    if (epoll_ctl_batch(epfd, del_ops, fds, NULL, 2) != 0 ||
        epoll_fd_count(epfd) != 0 ||
        wait_for_empty_public_port(epfd) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    ep_fault_reset();
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = -1;
    for (size_t i = 0; i < 2; i++) {
        if (receivers[i] != INVALID_SOCKET)
            (void)closesocket(receivers[i]);
    }
    return result;
}

static int test_afd_refresh_submit(void)
{
    static const uint64_t value = UINT64_C(0x7265667265736831);
    struct sockaddr_in address;
    ep_port_t *port = NULL;
    ep_sock_t *sock = NULL;
    epoll_data_t data;
    epoll_event_ex event;
    PNtDeviceIoControlFile original_submit = NULL;
    SOCKET receiver = INVALID_SOCKET;
    SOCKET sender = INVALID_SOCKET;
    int context = 1;
    int stub_installed = 0;
    int synthetic_pending = 0;
    int completion_posted = 0;
    int registered = 0;
    int state_ok;
    int result = -1;

    ep_fault_reset();
    memset(&data, 0, sizeof(data));
    memset(&event, 0, sizeof(event));
    data.u64 = value;
    if (ep_global_init() != 0 ||
        make_udp_receiver(&receiver, &address) != 0 ||
        (sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) ==
            INVALID_SOCKET ||
        ep_port_create(0, 0, &port) != 0) {
        goto cleanup;
    }

    original_submit = g_ntdll.NtDeviceIoControlFile;
    g_submit_calls = 0;
    g_ntdll.NtDeviceIoControlFile = submit_stub;
    stub_installed = 1;
    if (ep_port_register(port, receiver, EPOLLIN | EPOLLONESHOT,
                         EPOLLONESHOT, data, &context) != 0) {
        goto cleanup;
    }
    registered = 1;
    synthetic_pending = 1;
    g_ntdll.NtDeviceIoControlFile = original_submit;
    stub_installed = 0;

    pthread_mutex_lock(&port->fd_table_lock);
    sock = port->sock_list_head;
    state_ok = sock != NULL && sock->next == NULL &&
        g_submit_calls == 1 && sock->submitted_wait_epoch == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING &&
        port->pending_poll_count == 1;
    if (state_ok) {
        sock->afd_info->NumberOfHandles = 1;
        sock->afd_info->Handles[0].Events = AFD_POLL_RECEIVE;
        sock->afd_info->Handles[0].Status = STATUS_SUCCESS;
        sock->io_status_block.Status = STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok ||
        !PostQueuedCompletionStatus(
            port->iocp, 0, 0,
            (OVERLAPPED *)&sock->io_status_block)) {
        goto cleanup;
    }
    completion_posted = 1;
    synthetic_pending = 0;

    if (ep_fault_configure(EP_FAULT_AFD_SUBMIT, 1, EAGAIN) != 0) {
        goto cleanup;
    }
    errno = 0;
    if (ep_port_wait(port, &event, 1, 1000, NULL) != -1 ||
        errno != EAGAIN || ep_fault_hits(EP_FAULT_AFD_SUBMIT) != 1) {
        goto cleanup;
    }
    completion_posted = 0;

    pthread_mutex_lock(&port->fd_table_lock);
    state_ok = port->fd_table_count == 1 &&
        port->sock_list_head == sock && sock->next == NULL &&
        port->pending_poll_count == 0 && port->needs_rearm_count == 1 &&
        port->rearm_head == sock && port->rearm_tail == sock &&
        port->oneshot_fired_count == 0 &&
        port->oneshot_head == NULL && port->oneshot_tail == NULL &&
        port->async_error == 0 && port->asynchronous_errors == 1 &&
        sock->submitted_wait_epoch == 0 && sock->needs_rearm &&
        !sock->oneshot_fired && sock->pending_events == 0 &&
        atomic_load_explicit(&sock->state,
                             memory_order_relaxed) == EP_SOCK_REGISTERED &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&port->ready_queue.queued,
                             memory_order_relaxed) == 0 &&
        sock->afd_poll_key_owned == 0 && sock->afd_poll_target == NULL &&
        sock->afd_poll_key_reservation == NULL &&
        atomic_load_explicit(&port->active_wait_epoch,
                             memory_order_relaxed) == 0 &&
        ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok) {
        goto cleanup;
    }

    ep_fault_reset();
    if (sendto(sender, "r", 1, 0,
               (const struct sockaddr *)&address,
               (int)sizeof(address)) != 1) {
        goto cleanup;
    }
    memset(&event, 0, sizeof(event));
    if (ep_port_wait(port, &event, 1, 2000, NULL) != 1 ||
        event.events != EPOLLIN || event.data.u64 != value ||
        event.user_ctx != &context ||
        event.flags != WEPOLL_FLAG_ONESHOT_FIRED ||
        event.timestamp == 0 || g_submit_calls != 1) {
        goto cleanup;
    }
    {
        char byte = 0;

        if (recv(receiver, &byte, 1, 0) != 1 || byte != 'r') {
            goto cleanup;
        }
    }
    if (ep_port_unregister(port, receiver) != 0) {
        goto cleanup;
    }
    registered = 0;
    result = 0;

cleanup:
    ep_fault_reset();
    if (stub_installed) {
        g_ntdll.NtDeviceIoControlFile = original_submit;
    }
    if (port != NULL && sock != NULL && synthetic_pending) {
        sock->io_status_block.Status = STATUS_CANCELLED;
        if (PostQueuedCompletionStatus(
                port->iocp, 0, 0,
                (OVERLAPPED *)&sock->io_status_block)) {
            completion_posted = 1;
            synthetic_pending = 0;
        }
    }
    (void)completion_posted;
    if (registered && port != NULL) {
        (void)ep_port_unregister(port, receiver);
    }
    if (port != NULL && ep_port_destroy(port) != 0) {
        result = -1;
    }
    if (sender != INVALID_SOCKET) closesocket(sender);
    if (receiver != INVALID_SOCKET) closesocket(receiver);
    return result;
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

#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
static int run_endpoint_closed_identity_fault(ep_fault_point_t point)
{
    struct sockaddr_in address;
    struct epoll_event event;
    SOCKET closed_receiver = INVALID_SOCKET;
    SOCKET receiver = INVALID_SOCKET;
    int ctl_error;
    int ctl_result;
    int epfd = -1;
    int result = -1;

    ep_fault_reset();
    if (ep_global_init() != 0 ||
        make_udp_receiver(&receiver, &address) != 0 ||
        (epfd = epoll_create1(0)) < 0) {
        goto cleanup;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, receiver, &event) != 0 ||
        ep_fault_configure(point, 1, EIO) != 0) {
        goto cleanup;
    }
    closed_receiver = receiver;
    if (closesocket(receiver) == SOCKET_ERROR) goto cleanup;
    receiver = INVALID_SOCKET;

    errno = 0;
    ctl_result = epoll_ctl(epfd, EPOLL_CTL_MOD, closed_receiver, &event);
    ctl_error = errno;
    if (ctl_result != -1 || ctl_error != EBADF ||
        epoll_fd_count(epfd) != 0 ||
        ep_fault_hits(point) != 1) {
        goto cleanup;
    }
    result = 0;

cleanup:
    ep_fault_reset();
    if (epfd >= 0 && wepoll_close(epfd) != 0) result = -1;
    if (receiver != INVALID_SOCKET) closesocket(receiver);
    return result;
}
#endif

static int test_endpoint_closed_after_token_loss(void)
{
#ifdef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    puts("endpoint-closed-token-loss: skipped by synchronized lifetime contract");
    return 0;
#else
    return run_endpoint_closed_identity_fault(
               EP_FAULT_ENDPOINT_UNAVAILABLE) == 0 &&
           run_endpoint_closed_identity_fault(
               EP_FAULT_ENDPOINT_IDENTITY) == 0 ? 0 : -1;
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

static int test_wake_post(void)
{
    ep_port_t *port = NULL;
    struct epoll_event event;
    wepoll_ex_stats stats;
    int result = -1;

    ep_fault_reset();
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = UINT64_C(0x57414b45504f5354);
    if (ep_port_create(0, 0, &port) != 0 ||
        ep_fault_configure(EP_FAULT_IOCP_POST, 1, EIO) != 0) {
        goto cleanup;
    }

    errno = 0;
    if (ep_port_wake_event(port, &event) != -1 || errno != EIO ||
        ep_fault_hits(EP_FAULT_IOCP_POST) != 1 ||
        atomic_load_explicit(&port->wake_pending,
                             memory_order_acquire) != 0 ||
        atomic_load_explicit(&port->closing,
                             memory_order_acquire) == 0 ||
        atomic_load_explicit(&port->iocp_closed,
                             memory_order_acquire) == 0 ||
        ep_port_get_stats(port, &stats) != 0 ||
        stats.wake_requests != 1 || stats.wake_coalesced != 0 ||
        stats.wake_returns != 0 || stats.asynchronous_errors == 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    ep_fault_reset();
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    return result;
}

/* A synchronous AFD success is converted into an internal IOCP packet only
 * when a waiter is active.  Exercise the packet-post failure rollback so an
 * ADD does not strand a pending count or an AFD key after the port is
 * force-closed. */
static int test_iocp_post_immediate_socket(void)
{
    struct sockaddr_in address;
    ep_port_t *port = NULL;
    PNtDeviceIoControlFile original_submit = NULL;
    SOCKET receiver = INVALID_SOCKET;
    epoll_data_t data;
    int context = 1;
    int result = -1;

    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0x696d6d706f737466);
    ep_fault_reset();
    g_submit_status = STATUS_SUCCESS;
    g_submit_calls = 0;
    if (ep_global_init() != 0 ||
        make_udp_receiver(&receiver, &address) != 0 ||
        ep_port_create(0, 0, &port) != 0 ||
        ep_fault_configure(EP_FAULT_IOCP_POST, 1, EIO) != 0) {
        goto cleanup;
    }

    original_submit = g_ntdll.NtDeviceIoControlFile;
    g_ntdll.NtDeviceIoControlFile = submit_stub;
    atomic_store_explicit(&port->active_wait_epoch, 1,
                          memory_order_release);
    atomic_store_explicit(&port->waiter_active, 1, memory_order_release);
    errno = 0;
    if (ep_port_register(port, receiver, EPOLLIN, 0, data, &context) != -1 ||
        errno != EIO || g_submit_calls != 1 ||
        ep_fault_hits(EP_FAULT_IOCP_POST) != 1) {
        goto cleanup;
    }
    atomic_store_explicit(&port->waiter_active, 0, memory_order_release);

    pthread_mutex_lock(&port->fd_table_lock);
    if (port->fd_table_count != 0 || port->pending_poll_count != 0 ||
        port->needs_rearm_count != 0 || port->rearm_head != NULL ||
        port->rearm_tail != NULL ||
        atomic_load_explicit(&port->iocp_closed, memory_order_acquire) == 0 ||
        atomic_load_explicit(&port->closing, memory_order_acquire) == 0 ||
        atomic_load_explicit(&port->iocp_post_error, memory_order_acquire) ==
            0 ||
        ep_port_worklists_valid_locked(port) == 0) {
        pthread_mutex_unlock(&port->fd_table_lock);
        goto cleanup;
    }
    pthread_mutex_unlock(&port->fd_table_lock);
    result = 0;

cleanup:
    if (port != NULL) {
        atomic_store_explicit(&port->waiter_active, 0,
                              memory_order_release);
        atomic_store_explicit(&port->active_wait_epoch, 0,
                              memory_order_release);
    }
    if (original_submit != NULL)
        g_ntdll.NtDeviceIoControlFile = original_submit;
    g_submit_status = STATUS_PENDING;
    ep_fault_reset();
    if (port != NULL && ep_port_destroy(port) != 0)
        result = -1;
    if (receiver != INVALID_SOCKET)
        (void)closesocket(receiver);
    return result;
}

static int test_iocp_dequeue(void)
{
    ep_port_t *port = NULL;
    epoll_event_ex *large_events = NULL;
    epoll_event_ex event;
    epoll_data_t data;
    HANDLE event_handle = NULL;
    SOCKET fd;
    int context = 1;
    int result = -1;

    ep_fault_reset();
    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0x494f4350434f414c);
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

    large_events = (epoll_event_ex *)calloc(4097, sizeof(*large_events));
    event_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (large_events == NULL || event_handle == NULL) goto cleanup;
    fd = (SOCKET)(uintptr_t)event_handle;
    if (ep_port_register(port, fd, EPOLLIN, 0, data, &context) != 0 ||
        ep_fault_configure(EP_FAULT_IOCP_DEQUEUE, 2, EIO) != 0 ||
        !SetEvent(event_handle)) {
        goto cleanup;
    }

    memset(large_events, 0, 4097 * sizeof(*large_events));
    errno = 0;
    if (ep_port_wait(port, large_events, 4097, 2000, NULL) != 1 ||
        large_events[0].events != EPOLLIN ||
        large_events[0].data.u64 != data.u64 ||
        large_events[0].user_ctx != &context ||
        ep_fault_hits(EP_FAULT_IOCP_DEQUEUE) != 2) {
        goto cleanup;
    }

    errno = 0;
    if (ep_port_wait(port, &event, 1, 0, NULL) != -1 || errno != EIO ||
        ep_fault_hits(EP_FAULT_IOCP_DEQUEUE) != 2) {
        goto cleanup;
    }

    ep_fault_reset();
    memset(&event, 0, sizeof(event));
    if (ep_port_wait(port, &event, 1, 2000, NULL) != 1 ||
        event.events != EPOLLIN || event.data.u64 != data.u64 ||
        event.user_ctx != &context ||
        ep_fault_hits(EP_FAULT_IOCP_DEQUEUE) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    ep_fault_reset();
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    if (event_handle != NULL) CloseHandle(event_handle);
    free(large_events);
    return result;
}

static int test_aux_closed_iocp_retire(void)
{
    HANDLE event_handle = NULL;
    struct epoll_event event;
    wepoll_ex_global_stats before = {0};
    wepoll_ex_global_stats after = {0};
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
        wepoll_ex_get_global_stats(&before, sizeof(before)) != 0 ||
        ep_fault_configure(EP_FAULT_IOCP_POST, 1, EIO) != 0) {
        goto cleanup;
    }

    if (wepoll_close(epfd) != 0) {
        epfd = -1;
        goto cleanup;
    }
    epfd = -1;
    if (ep_fault_hits(EP_FAULT_IOCP_POST) != 1 ||
        wepoll_ex_get_global_stats(&after, sizeof(after)) != 0 ||
        after.quarantined_ports != before.quarantined_ports ||
        after.active_quarantines != before.active_quarantines ||
        after.irrecoverable_ports != before.irrecoverable_ports) {
        goto cleanup;
    }
    result = 0;

cleanup:
    ep_fault_reset();
    if (epfd >= 0 && wepoll_close(epfd) != 0) result = -1;
    if (event_handle != NULL) CloseHandle(event_handle);
    return result;
}

static int test_aux_post_failure(void)
{
    fault_wait_context_t context;
    HANDLE event_handle = NULL;
    HANDLE thread = NULL;
    struct epoll_event event;
    wepoll_ex_stats stats = {0};
    ULONGLONG deadline;
    int epfd = -1;
    int result = -1;

    memset(&context, 0, sizeof(context));
    context.epfd = -1;
    context.result = 1;
    context.started = CreateEventW(NULL, TRUE, FALSE, NULL);
    event_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    epfd = epoll_create1(0);
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (context.started == NULL || event_handle == NULL || epfd < 0 ||
        epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)event_handle, &event) != 0 ||
        ep_fault_configure(EP_FAULT_AUX_POST, 1, EIO) != 0) {
        goto cleanup;
    }

    context.epfd = epfd;
    thread = CreateThread(NULL, 0, fault_wait_thread, &context, 0, NULL);
    if (thread == NULL ||
        WaitForSingleObject(context.started, 2000) != WAIT_OBJECT_0) {
        goto cleanup;
    }
    deadline = GetTickCount64() + 2000;
    do {
        if (wepoll_ex_get_stats(epfd, &stats, sizeof(stats)) != 0) {
            goto cleanup;
        }
        if (stats.pending_polls == 1) break;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    if (stats.pending_polls != 1 || !SetEvent(event_handle) ||
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 ||
        context.result != -1 || context.error != EIO ||
        ep_fault_hits(EP_FAULT_AUX_POST) != 1) {
        goto cleanup;
    }

    ep_fault_reset();
    if (wepoll_close(epfd) != 0) {
        epfd = -1;
        goto cleanup;
    }
    epfd = -1;
    result = 0;

cleanup:
    ep_fault_reset();
    if (epfd >= 0) {
        if (wepoll_close(epfd) != 0) result = -1;
        epfd = -1;
    }
    if (thread != NULL &&
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) {
        (void)TerminateThread(thread, 1);
        result = -1;
    }
    if (thread != NULL) CloseHandle(thread);
    if (event_handle != NULL) CloseHandle(event_handle);
    if (context.started != NULL) CloseHandle(context.started);
    return result;
}

static int test_aux_post_immediate_failure(void)
{
    fault_wait_context_t context;
    HANDLE pending_event = NULL;
    HANDLE signaled_event = NULL;
    HANDLE thread = NULL;
    struct epoll_event event;
    wepoll_ex_stats stats = {0};
    ULONGLONG deadline;
    int epfd = -1;
    int result = -1;

    memset(&context, 0, sizeof(context));
    context.epfd = -1;
    context.result = 1;
    ep_fault_reset();
    context.started = CreateEventW(NULL, TRUE, FALSE, NULL);
    pending_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    signaled_event = CreateEventW(NULL, TRUE, TRUE, NULL);
    epfd = epoll_create1(0);
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (context.started == NULL || pending_event == NULL ||
        signaled_event == NULL || epfd < 0 ||
        epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)pending_event, &event) != 0) {
        goto cleanup;
    }

    context.epfd = epfd;
    thread = CreateThread(NULL, 0, fault_wait_thread, &context, 0, NULL);
    if (thread == NULL ||
        WaitForSingleObject(context.started, 2000) != WAIT_OBJECT_0) {
        goto cleanup;
    }
    deadline = GetTickCount64() + 2000;
    do {
        if (wepoll_ex_get_stats(epfd, &stats, sizeof(stats)) != 0) {
            goto cleanup;
        }
        if (stats.pending_polls == 1) break;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    if (stats.pending_polls != 1 ||
        ep_fault_configure(EP_FAULT_AUX_POST, 1, EIO) != 0) {
        goto cleanup;
    }

    errno = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD,
                  (epoll_fd_t)signaled_event, &event) != -1 ||
        errno != EIO || ep_fault_hits(EP_FAULT_AUX_POST) != 1 ||
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 ||
        context.result != -1 || context.error != EIO) {
        goto cleanup;
    }
    ep_fault_reset();
    if (wepoll_close(epfd) != 0) {
        epfd = -1;
        goto cleanup;
    }
    epfd = -1;
    result = 0;

cleanup:
    ep_fault_reset();
    if (epfd >= 0) {
        if (wepoll_close(epfd) != 0) result = -1;
        epfd = -1;
    }
    if (thread != NULL &&
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) {
        (void)TerminateThread(thread, 1);
        result = -1;
    }
    if (thread != NULL) CloseHandle(thread);
    if (signaled_event != NULL) CloseHandle(signaled_event);
    if (pending_event != NULL) CloseHandle(pending_event);
    if (context.started != NULL) CloseHandle(context.started);
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

static int test_shutdown_ready_node_alloc(void)
{
    static const char byte = 's';
    ep_port_t *port = NULL;
    ep_sock_t *sock = NULL;
    epoll_data_t data;
    epoll_event_ex output;
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET server = INVALID_SOCKET;
    int context = 1;
    int registered = 0;
    int state_ok;
    int result = -1;
    char received = 0;

    ep_fault_reset();
    memset(&data, 0, sizeof(data));
    memset(&output, 0, sizeof(output));
    data.u64 = UINT64_C(0x73687574616c6c6f);
    if (ep_global_init() != 0 ||
        make_tcp_pair(&listener, &client, &server) != 0 ||
        ep_port_create(0, 0, &port) != 0 ||
        ep_port_register(port, server, EPOLLOUT, 0,
                         data, &context) != 0) {
        goto cleanup;
    }
    registered = 1;
    if (ep_port_wait(port, &output, 1, 2000, NULL) != 1 ||
        output.events != EPOLLOUT || output.data.u64 != data.u64 ||
        output.user_ctx != &context ||
        ep_port_modify(port, server, EPOLLIN | EPOLLRDHUP, 0,
                       data, &context) != 0 ||
        ep_fault_configure(EP_FAULT_READY_NODE_ALLOC, 1, ENOMEM) != 0) {
        goto cleanup;
    }

    errno = 0;
    if (ep_port_shutdown_socket(port, server, SD_RECEIVE) != -1 ||
        errno != ENOMEM ||
        ep_fault_hits(EP_FAULT_READY_NODE_ALLOC) != 1) {
        goto cleanup;
    }
    pthread_mutex_lock(&port->fd_table_lock);
    sock = port->sock_list_head;
    state_ok = sock != NULL && sock->next == NULL &&
        sock->local_shutdown == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        sock->needs_rearm && ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok || send(client, &byte, 1, 0) != 1 ||
        recv(server, &received, 1, 0) != 1 || received != byte) {
        goto cleanup;
    }

    ep_fault_reset();
    memset(&output, 0, sizeof(output));
    if (ep_port_shutdown_socket(port, server, SD_RECEIVE) != 0 ||
        ep_port_wait(port, &output, 1, 2000, NULL) != 1 ||
        output.events != (EPOLLIN | EPOLLRDHUP) ||
        output.data.u64 != data.u64 || output.user_ctx != &context) {
        goto cleanup;
    }
    result = 0;

cleanup:
    ep_fault_reset();
    if (registered && port != NULL &&
        ep_port_unregister(port, server) != 0) {
        result = -1;
    }
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    if (server != INVALID_SOCKET) closesocket(server);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return result;
}

static int test_shutdown_cancel_retry(void)
{
    ep_port_t *port = NULL;
    ep_sock_t *sock = NULL;
    epoll_data_t data;
    epoll_event_ex output;
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET server = INVALID_SOCKET;
    int context = 1;
    int registered = 0;
    int state_ok;
    int result = -1;
    char byte;

    ep_fault_reset();
    memset(&data, 0, sizeof(data));
    memset(&output, 0, sizeof(output));
    data.u64 = UINT64_C(0x7368757463616e63);
    if (ep_global_init() != 0 ||
        make_tcp_pair(&listener, &client, &server) != 0 ||
        ep_port_create(0, 0, &port) != 0 ||
        ep_port_register(port, server, EPOLLIN | EPOLLRDHUP, 0,
                         data, &context) != 0 ||
        ep_fault_configure(EP_FAULT_AFD_CANCEL, 1, EBUSY) != 0) {
        goto cleanup;
    }
    registered = 1;

    errno = 0;
    if (ep_port_shutdown_socket(port, server, SD_RECEIVE) != -1 ||
        errno != EBUSY || ep_fault_hits(EP_FAULT_AFD_CANCEL) != 1 ||
        recv(server, &byte, 1, 0) != SOCKET_ERROR ||
        WSAGetLastError() != WSAESHUTDOWN) {
        goto cleanup;
    }
    pthread_mutex_lock(&port->fd_table_lock);
    sock = port->sock_list_head;
    state_ok = sock != NULL && sock->next == NULL &&
        sock->local_shutdown == EP_LOCAL_SHUTDOWN_READ &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING &&
        sock->needs_rearm && ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok) goto cleanup;

    ep_fault_reset();
    if (ep_port_shutdown_socket(port, server, SD_RECEIVE) != 0 ||
        ep_port_wait(port, &output, 1, 2000, NULL) != 1 ||
        output.events != (EPOLLIN | EPOLLRDHUP) ||
        output.data.u64 != data.u64 || output.user_ctx != &context) {
        goto cleanup;
    }
    result = 0;

cleanup:
    ep_fault_reset();
    if (registered && port != NULL &&
        ep_port_unregister(port, server) != 0) {
        result = -1;
    }
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    if (server != INVALID_SOCKET) closesocket(server);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    return result;
}

static int test_waitable_zero_disarm(void)
{
    ep_port_t *port = NULL;
    ep_sock_t *sock = NULL;
    HANDLE event_handle = NULL;
    HANDLE old_wait_registration = NULL;
    epoll_data_t old_data;
    epoll_data_t new_data;
    epoll_event_ex event;
    SOCKET fd;
    uint64_t old_generation = 0;
    int old_context = 1;
    int new_context = 2;
    int state_ok;
    int result = -1;

    ep_fault_reset();
    memset(&old_data, 0, sizeof(old_data));
    memset(&new_data, 0, sizeof(new_data));
    memset(&event, 0, sizeof(event));
    old_data.u64 = UINT64_C(0x1020304050607080);
    new_data.u64 = UINT64_C(0x8070605040302010);
    event_handle = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (event_handle == NULL || ep_port_create(0, 0, &port) != 0) {
        goto cleanup;
    }
    fd = (SOCKET)(uintptr_t)event_handle;
    if (ep_port_register(port, fd, EPOLLIN, 0,
                         old_data, &old_context) != 0 ||
        ep_port_wait(port, &event, 1, 0, NULL) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    sock = port->sock_list_head;
    state_ok = sock != NULL && sock->next == NULL &&
        sock->kind == EP_REG_WAITABLE &&
        sock->waitable_semantics == EP_WAITABLE_CONSUMPTIVE &&
        sock->user_events == EPOLLIN && sock->user_flags == 0 &&
        sock->user_data.u64 == old_data.u64 &&
        sock->user_ctx == &old_context &&
        atomic_load_explicit(&sock->state,
                             memory_order_relaxed) == EP_SOCK_POLLING &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->completion_posted,
                             memory_order_acquire) == 0 &&
        atomic_load_explicit(&sock->waitable_notification_owned,
                             memory_order_acquire) == 0 &&
        sock->wait_registration != NULL && !sock->needs_rearm &&
        !sock->et_holdoff && port->pending_poll_count == 1 &&
        port->needs_rearm_count == 0;
    if (sock != NULL) {
        old_generation = sock->generation;
        old_wait_registration = sock->wait_registration;
    }
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok ||
        ep_fault_configure(EP_FAULT_AUX_DISARM, 1, EBUSY) != 0) {
        goto cleanup;
    }

    errno = 0;
    if (ep_port_modify(port, fd, 0, 0,
                       new_data, &new_context) != -1 ||
        errno != EBUSY || ep_fault_hits(EP_FAULT_AUX_DISARM) != 1) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    state_ok = port->fd_table_count == 1 &&
        port->sock_list_head == sock && sock->next == NULL &&
        sock->user_events == EPOLLIN && sock->user_flags == 0 &&
        sock->user_data.u64 == old_data.u64 &&
        sock->user_ctx == &old_context &&
        sock->generation == old_generation &&
        sock->wait_registration == old_wait_registration &&
        atomic_load_explicit(&sock->state,
                             memory_order_relaxed) == EP_SOCK_POLLING &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->completion_posted,
                             memory_order_acquire) == 0 &&
        atomic_load_explicit(&sock->waitable_notification_owned,
                             memory_order_acquire) == 0 &&
        !sock->needs_rearm && !sock->et_holdoff &&
        port->pending_poll_count == 1 && port->needs_rearm_count == 0;
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok || !SetEvent(event_handle)) {
        goto cleanup;
    }

    memset(&event, 0, sizeof(event));
    if (ep_port_wait(port, &event, 1, 2000, NULL) != 1 ||
        (event.events & EPOLLIN) == 0 ||
        event.data.u64 != old_data.u64 || event.user_ctx != &old_context ||
        ep_fault_hits(EP_FAULT_AUX_DISARM) != 2 ||
        WaitForSingleObject(event_handle, 0) != WAIT_TIMEOUT ||
        ep_port_wait(port, &event, 1, 50, NULL) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    ep_fault_reset();
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    if (event_handle != NULL) CloseHandle(event_handle);
    return result;
}

static int test_waitable_ready_node_alloc(void)
{
    ep_port_t *port = NULL;
    ep_sock_t *sock = NULL;
    HANDLE event_handle = NULL;
    epoll_data_t data;
    epoll_event_ex event;
    SOCKET fd;
    int context = 1;
    int state_ok;
    int result = -1;

    ep_fault_reset();
    memset(&data, 0, sizeof(data));
    memset(&event, 0, sizeof(event));
    data.u64 = UINT64_C(0xabcdef0123456789);
    event_handle = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (event_handle == NULL || ep_port_create(0, 0, &port) != 0) {
        goto cleanup;
    }
    fd = (SOCKET)(uintptr_t)event_handle;
    if (ep_port_register(port, fd, EPOLLIN, 0, data, &context) != 0 ||
        ep_port_wait(port, &event, 1, 0, NULL) != 0 ||
        ep_fault_configure(EP_FAULT_READY_NODE_ALLOC, 1, ENOMEM) != 0 ||
        !SetEvent(event_handle)) {
        goto cleanup;
    }

    errno = 0;
    if (ep_port_wait(port, &event, 1, 2000, NULL) != -1 ||
        errno != ENOMEM ||
        ep_fault_hits(EP_FAULT_READY_NODE_ALLOC) != 1 ||
        WaitForSingleObject(event_handle, 0) != WAIT_TIMEOUT) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    sock = port->sock_list_head;
    state_ok = sock != NULL && sock->next == NULL &&
        sock->kind == EP_REG_WAITABLE &&
        sock->waitable_semantics == EP_WAITABLE_CONSUMPTIVE &&
        sock->user_events == EPOLLIN && sock->user_flags == 0 &&
        sock->user_data.u64 == data.u64 && sock->user_ctx == &context &&
        sock->wait_registration == NULL &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->completion_posted,
                             memory_order_acquire) == 0 &&
        atomic_load_explicit(&sock->waitable_notification_owned,
                             memory_order_acquire) == 1 &&
        sock->needs_rearm && port->pending_poll_count == 0 &&
        port->needs_rearm_count == 1 && port->rearm_head == sock &&
        port->rearm_tail == sock && port->asynchronous_errors != 0;
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok) {
        goto cleanup;
    }

    ep_fault_reset();
    memset(&event, 0, sizeof(event));
    if (ep_port_wait(port, &event, 1, 2000, NULL) != 1 ||
        (event.events & EPOLLIN) == 0 || event.data.u64 != data.u64 ||
        event.user_ctx != &context ||
        atomic_load_explicit(&sock->waitable_notification_owned,
                             memory_order_acquire) != 0 ||
        ep_port_wait(port, &event, 1, 50, NULL) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    ep_fault_reset();
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    if (event_handle != NULL) CloseHandle(event_handle);
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

static int test_aux_consumptive_disarm(void)
{
    HANDLE event_handle = NULL;
    struct epoll_event event;
    wepoll_ex_stats stats = {0};
    int epfd = -1;
    int result = -1;

    ep_fault_reset();
    event_handle = CreateEventW(NULL, FALSE, FALSE, NULL);
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
        stats.rearm_queue_depth != 1) {
        goto cleanup;
    }
    if (epoll_wait(epfd, &event, 1, 1000) != 1 ||
        (event.events & EPOLLIN) == 0 ||
        ep_fault_hits(EP_FAULT_AUX_DISARM) != 2 ||
        epoll_wait(epfd, &event, 1, 50) != 0) {
        goto cleanup;
    }
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

static PGetQueuedCompletionStatusEx timeout_native_dequeue;
static ep_port_t *timeout_fault_port;

/* The precise timer callback and the rounded-millisecond IOCP timeout share
 * nearly the same deadline.  For the callback-post fault, keep dequeuing in
 * short native slices until the callback reaches the injected post or the
 * optional timer path proves unavailable.  This prevents scheduler order
 * from deciding whether the fault point is covered. */
static BOOL WINAPI timeout_post_dequeue(
    HANDLE completion_port, OVERLAPPED_ENTRY *entries, ULONG count,
    PULONG removed, DWORD milliseconds, BOOL alertable)
{
    ULONGLONG deadline = GetTickCount64() + 2000;

    (void)milliseconds;
    for (;;) {
        BOOL ok = timeout_native_dequeue(
            completion_port, entries, count, removed, 10, alertable);

        if (ok) return TRUE;
        if (GetLastError() != WAIT_TIMEOUT) return FALSE;
        if (ep_fault_hits(EP_FAULT_TIMEOUT_POST) != 0 ||
            (timeout_fault_port != NULL &&
             timeout_fault_port->precise_timeout_capability ==
                 EP_TIMEOUT_CAPABILITY_UNAVAILABLE) ||
            GetTickCount64() >= deadline) {
            *removed = 0;
            SetLastError(WAIT_TIMEOUT);
            return FALSE;
        }
    }
}

static int run_timeout_fault(ep_fault_point_t point)
{
    const struct timespec duration = { 0, 20000000L };
    ep_wait_timeout_t timeout;
    epoll_event_ex event;
    ep_port_t *port = NULL;
    uint64_t post_failures_before = 0;
    PGetQueuedCompletionStatusEx original_dequeue = NULL;
    int result = -1;

    ep_fault_reset();
    if (ep_port_create(0, 0, &port) != 0 ||
        ep_wait_timeout_from_timespec(&duration, &timeout) != 0) {
        goto cleanup;
    }
    if (port->create_waitable_timer_ex_w == NULL ||
        port->query_unbiased_interrupt_time_precise == NULL) {
        /* Older supported Windows releases intentionally take the coarse
         * fallback and cannot reach high-resolution timer fault points. */
        result = TEST_SKIP;
        goto cleanup;
    }
    post_failures_before = atomic_load_explicit(
        &port->precise_timeout_post_failures, memory_order_relaxed);
    if (ep_fault_configure(point, 1, EIO) != 0) {
        goto cleanup;
    }
    if (point == EP_FAULT_TIMEOUT_POST) {
        original_dequeue = port->get_queued_completion_status_ex;
        timeout_native_dequeue = original_dequeue;
        timeout_fault_port = port;
        port->get_queued_completion_status_ex = timeout_post_dequeue;
    }
    if (ep_port_wait_timeout(port, &event, 1, &timeout, NULL) != 0 ||
        atomic_load_explicit(&port->iocp_closed,
                             memory_order_acquire) != 0 ||
        atomic_load_explicit(&port->iocp_post_error,
                             memory_order_acquire) != 0) {
        goto cleanup;
    }
    if (ep_fault_hits(point) != 1) {
        if (point != EP_FAULT_TIMEOUT_INIT &&
            ep_fault_hits(point) == 0 &&
            port->precise_timeout_capability ==
                EP_TIMEOUT_CAPABILITY_UNAVAILABLE) {
            result = TEST_SKIP;
        }
        goto cleanup;
    }
    if (point == EP_FAULT_TIMEOUT_INIT &&
        port->precise_timeout_capability !=
            EP_TIMEOUT_CAPABILITY_UNAVAILABLE) {
        goto cleanup;
    }
    if (point == EP_FAULT_TIMEOUT_POST &&
        atomic_load_explicit(&port->precise_timeout_post_failures,
                             memory_order_relaxed) !=
            post_failures_before + 1) {
        goto cleanup;
    }

    ep_fault_reset();
    if (ep_port_wait(port, &event, 1, 0, NULL) != 0) goto cleanup;
    result = 0;

cleanup:
    if (port != NULL && original_dequeue != NULL) {
        port->get_queued_completion_status_ex = original_dequeue;
    }
    timeout_fault_port = NULL;
    timeout_native_dequeue = NULL;
    ep_fault_reset();
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    return result;
}

static int test_timeout_init(void)
{
    return run_timeout_fault(EP_FAULT_TIMEOUT_INIT);
}

static int test_timeout_arm(void)
{
    return run_timeout_fault(EP_FAULT_TIMEOUT_ARM);
}

static int test_timeout_post(void)
{
    return run_timeout_fault(EP_FAULT_TIMEOUT_POST);
}

typedef int (*fault_test_fn)(void);

typedef struct fault_test_case {
    const char *name;
    fault_test_fn run;
} fault_test_case_t;

static const fault_test_case_t g_tests[] = {
    { "framework", test_framework },
    { "error-info-native", test_error_info_native },
    { "pool-init", test_pool_init_alloc },
    { "pool-grow", test_pool_growth },
    { "provider-base", test_provider_base },
    { "afd-open", test_afd_open },
    { "afd-submit", test_afd_submit },
    { "afd-submit-batch", test_afd_submit_batch },
    { "afd-refresh-submit", test_afd_refresh_submit },
    { "afd-key-fallback", test_afd_key_fallback },
    { "afd-cancel", test_afd_cancel },
    { "endpoint-identity", test_endpoint_identity },
    { "endpoint-policy", test_endpoint_policy },
    { "endpoint-closed-token-loss", test_endpoint_closed_after_token_loss },
    { "iocp-create", test_iocp_create },
    { "iocp-post", test_iocp_post },
    { "wake-post", test_wake_post },
    { "iocp-post-immediate-socket", test_iocp_post_immediate_socket },
    { "iocp-dequeue", test_iocp_dequeue },
    { "aux-closed-iocp", test_aux_closed_iocp_retire },
    { "aux-post", test_aux_post_failure },
    { "aux-post-immediate", test_aux_post_immediate_failure },
    { "ready-node-alloc", test_ready_node_alloc },
    { "shutdown-ready-node-alloc", test_shutdown_ready_node_alloc },
    { "shutdown-cancel-retry", test_shutdown_cancel_retry },
    { "waitable-ready-node-alloc", test_waitable_ready_node_alloc },
    { "waitable-zero-disarm", test_waitable_zero_disarm },
    { "aux-waitable-disarm", test_aux_waitable_disarm },
    { "aux-waitable-disarm-repeat", test_aux_waitable_disarm_repeat },
    { "aux-consumptive-disarm", test_aux_consumptive_disarm },
    { "aux-pipe-disarm", test_aux_pipe_disarm },
    { "timeout-init", test_timeout_init },
    { "timeout-arm", test_timeout_arm },
    { "timeout-post", test_timeout_post }
};

static int run_test(const fault_test_case_t *test)
{
    int result = test->run();

    if (result == 0) {
        printf("%s: OK\n", test->name);
        return 0;
    }
    if (result == TEST_SKIP) {
        printf("%s: SKIPPED (high-resolution timer unavailable)\n",
               test->name);
        return TEST_SKIP;
    }
    fprintf(stderr, "%s: FAILED (errno=%d WSA=%d)\n",
            test->name, errno, WSAGetLastError());
    return -1;
}

int main(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "all") == 0)) {
        for (size_t i = 0; i < sizeof(g_tests) / sizeof(g_tests[0]); i++) {
            int result = run_test(&g_tests[i]);

            if (result != 0 && result != TEST_SKIP)
                return 1;
        }
        return 0;
    }

    if (argc == 2) {
        for (size_t i = 0; i < sizeof(g_tests) / sizeof(g_tests[0]); i++) {
            if (strcmp(argv[1], g_tests[i].name) == 0) {
                int result = run_test(&g_tests[i]);

                if (result == TEST_SKIP) return TEST_SKIP;
                return result == 0 ? 0 : 1;
            }
        }
    }

    fprintf(stderr,
            "usage: %s [all|framework|pool-init|pool-grow|provider-base|"
            "error-info-native|"
            "afd-open|afd-submit|afd-submit-batch|afd-refresh-submit|"
            "afd-key-fallback|"
            "afd-cancel|"
            "endpoint-identity|"
            "endpoint-policy|endpoint-closed-token-loss|iocp-create|"
            "iocp-post|wake-post|iocp-dequeue|"
            "aux-closed-iocp|aux-post|aux-post-immediate|ready-node-alloc|"
            "shutdown-ready-node-alloc|shutdown-cancel-retry|"
            "waitable-ready-node-alloc|waitable-zero-disarm|"
            "aux-waitable-disarm|"
            "aux-waitable-disarm-repeat|aux-consumptive-disarm|"
            "aux-pipe-disarm|timeout-init|timeout-arm|timeout-post]\n",
            argv[0]);
    return 2;
}
