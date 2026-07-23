/* Deterministic internal regressions for Windows registration state changes.
 * Each mode runs independently so failures remain easy to isolate. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex_internal.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STATUS_ACCESS_DENIED
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022L)
#endif

typedef struct state_fixture {
    ep_port_t *port;
    SOCKET listener;
    SOCKET client;
    SOCKET server;
} state_fixture_t;

static void fixture_reset(state_fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->listener = INVALID_SOCKET;
    fixture->client = INVALID_SOCKET;
    fixture->server = INVALID_SOCKET;
}

static void fixture_close(state_fixture_t *fixture)
{
    if (fixture->port != NULL) {
        (void)ep_port_destroy(fixture->port);
        fixture->port = NULL;
    }
    if (fixture->server != INVALID_SOCKET) {
        (void)closesocket(fixture->server);
        fixture->server = INVALID_SOCKET;
    }
    if (fixture->client != INVALID_SOCKET) {
        (void)closesocket(fixture->client);
        fixture->client = INVALID_SOCKET;
    }
    if (fixture->listener != INVALID_SOCKET) {
        (void)closesocket(fixture->listener);
        fixture->listener = INVALID_SOCKET;
    }
}

static int fixture_open(state_fixture_t *fixture)
{
    struct sockaddr_in address;
    int address_length = (int)sizeof(address);

    fixture_reset(fixture);
    fixture->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fixture->listener == INVALID_SOCKET) goto fail;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (bind(fixture->listener, (const struct sockaddr *)&address,
             (int)sizeof(address)) == SOCKET_ERROR ||
        listen(fixture->listener, 1) == SOCKET_ERROR ||
        getsockname(fixture->listener, (struct sockaddr *)&address,
                    &address_length) == SOCKET_ERROR) {
        goto fail;
    }

    fixture->client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fixture->client == INVALID_SOCKET ||
        connect(fixture->client, (const struct sockaddr *)&address,
                (int)sizeof(address)) == SOCKET_ERROR) {
        goto fail;
    }
    fixture->server = accept(fixture->listener, NULL, NULL);
    if (fixture->server == INVALID_SOCKET ||
        ep_port_create(0, 0, &fixture->port) != 0) {
        goto fail;
    }
    return 0;

fail:
    fixture_close(fixture);
    return -1;
}

static int send_byte(SOCKET socket_fd)
{
    static const char byte = 'x';
    return send(socket_fd, &byte, 1, 0) == 1 ? 0 : -1;
}

static ep_sock_t *fixture_sock(state_fixture_t *fixture)
{
    ep_sock_t *sock;

    pthread_mutex_lock(&fixture->port->fd_table_lock);
    sock = fixture->port->sock_list_head;
    if (sock != NULL && sock->next != NULL) sock = NULL;
    pthread_mutex_unlock(&fixture->port->fd_table_lock);
    return sock;
}

/* Consume IOCP packets and run their handlers without draining the ready
 * queue.  This creates the otherwise tiny "completion queued, wait not yet
 * called" window deterministically. */
static int pump_iocp(ep_port_t *port, DWORD timeout_ms)
{
    ULONG removed = 0;
    int handled = 0;
    BOOL ok = GetQueuedCompletionStatusEx(
        port->iocp, port->iocp_entries, port->iocp_batch_size,
        &removed, timeout_ms, FALSE);

    if (!ok) {
        ep_set_errno(ep_winerr_to_errno(GetLastError()));
        return -1;
    }
    for (ULONG i = 0; i < removed; i++) {
        OVERLAPPED *overlapped = port->iocp_entries[i].lpOverlapped;
        IO_STATUS_BLOCK *iosb;
        ep_sock_t *sock;

        if (overlapped == NULL) continue;
        iosb = (IO_STATUS_BLOCK *)overlapped;
        sock = (ep_sock_t *)((unsigned char *)iosb -
                             offsetof(ep_sock_t, io_status_block));
        ep_sock_handle_completion(
            sock, port->iocp_entries[i].dwNumberOfBytesTransferred,
            iosb->Status);
        handled++;
    }
    if (handled == 0) {
        ep_set_errno(EIO);
        return -1;
    }
    return handled;
}

static int register_events(state_fixture_t *fixture, uint32_t events,
                           uint32_t flags, uint64_t value, void *context)
{
    epoll_data_t data;
    epoll_event_ex ignored;
    int result;

    memset(&data, 0, sizeof(data));
    data.u64 = value;
    result = ep_port_register(fixture->port, fixture->server,
                              events, flags, data, context);
    if (result != 0)
        return result;

    /* Public registrations intentionally defer AFD submission until a wait
     * path is active.  These internal state tests need a pending request
     * before they inject or inspect completions, so arm it explicitly. */
    memset(&ignored, 0, sizeof(ignored));
    return ep_port_wait(fixture->port, &ignored, 1, 0, NULL) < 0 ? -1 : 0;
}

static int register_oneshot(state_fixture_t *fixture, uint64_t value,
                            void *context)
{
    return register_events(fixture, EPOLLIN | EPOLLONESHOT,
                           EPOLLONESHOT, value, context);
}

static int modify_events(state_fixture_t *fixture, uint32_t events,
                         uint32_t flags, uint64_t value, void *context)
{
    epoll_data_t data;

    memset(&data, 0, sizeof(data));
    data.u64 = value;
    return ep_port_modify(fixture->port, fixture->server,
                          events, flags, data, context);
}

static int modify_oneshot(state_fixture_t *fixture, uint64_t value,
                          void *context)
{
    return modify_events(fixture, EPOLLIN | EPOLLONESHOT,
                         EPOLLONESHOT, value, context);
}

static PNtCancelIoFileEx counted_cancel_delegate;
static volatile LONG counted_cancel_calls;
static PNtDeviceIoControlFile counted_submit_delegate;
static volatile LONG counted_submit_calls;

static NTSTATUS NTAPI counted_cancel_stub(
    HANDLE file_handle,
    PIO_STATUS_BLOCK io_request_to_cancel,
    PIO_STATUS_BLOCK io_status_block)
{
    InterlockedIncrement(&counted_cancel_calls);
    return counted_cancel_delegate(file_handle, io_request_to_cancel,
                                   io_status_block);
}

static NTSTATUS NTAPI counted_submit_stub(
    HANDLE file_handle,
    HANDLE event,
    PIO_APC_ROUTINE apc_routine,
    PVOID apc_context,
    PIO_STATUS_BLOCK io_status_block,
    ULONG io_control_code,
    PVOID input_buffer,
    ULONG input_buffer_length,
    PVOID output_buffer,
    ULONG output_buffer_length)
{
    InterlockedIncrement(&counted_submit_calls);
    return counted_submit_delegate(file_handle, event, apc_routine,
                                   apc_context, io_status_block,
                                   io_control_code, input_buffer,
                                   input_buffer_length, output_buffer,
                                   output_buffer_length);
}

static NTSTATUS NTAPI submit_failure_stub(
    HANDLE file_handle,
    HANDLE event,
    PIO_APC_ROUTINE apc_routine,
    PVOID apc_context,
    PIO_STATUS_BLOCK io_status_block,
    ULONG io_control_code,
    PVOID input_buffer,
    ULONG input_buffer_length,
    PVOID output_buffer,
    ULONG output_buffer_length)
{
    (void)file_handle;
    (void)event;
    (void)apc_routine;
    (void)apc_context;
    (void)io_status_block;
    (void)io_control_code;
    (void)input_buffer;
    (void)input_buffer_length;
    (void)output_buffer;
    (void)output_buffer_length;
    return STATUS_ACCESS_DENIED;
}

#ifdef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
static SOCKET cached_base_capture;
static int cached_base_capture_calls;

static NTSTATUS NTAPI submit_capture_base_stub(
    HANDLE file_handle,
    HANDLE event,
    PIO_APC_ROUTINE apc_routine,
    PVOID apc_context,
    PIO_STATUS_BLOCK io_status_block,
    ULONG io_control_code,
    PVOID input_buffer,
    ULONG input_buffer_length,
    PVOID output_buffer,
    ULONG output_buffer_length)
{
    AFD_POLL_INFO *info = (AFD_POLL_INFO *)input_buffer;

    (void)file_handle;
    (void)event;
    (void)apc_routine;
    (void)apc_context;
    (void)io_status_block;
    (void)io_control_code;
    (void)input_buffer_length;
    (void)output_buffer;
    (void)output_buffer_length;
    cached_base_capture = info != NULL && info->NumberOfHandles > 0
        ? (SOCKET)info->Handles[0].Handle : INVALID_SOCKET;
    cached_base_capture_calls++;
    return STATUS_PENDING;
}

/* The synchronized-lifetime path must use the base captured at ADD.  An
 * invalid numeric fd makes a second SIO_BASE_HANDLE lookup fail, while the
 * cached path still reaches the AFD submission stub. */
static int test_cached_base_submit(void)
{
    ep_port_t port;
    ep_sock_t sock;
    PNtDeviceIoControlFile original_submit;
    SOCKET expected_base = (SOCKET)123;
    int result = -1;

    memset(&port, 0, sizeof(port));
    memset(&sock, 0, sizeof(sock));
    port.afd = (HANDLE)(uintptr_t)1;
    sock.port = &port;
    sock.fd = INVALID_SOCKET;
    sock.base_socket = expected_base;
    atomic_init(&sock.poll_status, EP_POLL_IDLE);

    original_submit = g_ntdll.NtDeviceIoControlFile;
    g_ntdll.NtDeviceIoControlFile = submit_capture_base_stub;
    cached_base_capture = INVALID_SOCKET;
    cached_base_capture_calls = 0;
    if (ep_afd_poll_submit(&sock, AFD_POLL_RECEIVE) == 0 &&
        cached_base_capture_calls == 1 &&
        cached_base_capture == expected_base) {
        result = 0;
    }
    g_ntdll.NtDeviceIoControlFile = original_submit;
    atomic_store(&sock.poll_status, EP_POLL_IDLE);
    free(sock.afd_info);
    return result;
}
#endif

static int expect_one_oneshot(ep_port_t *port, uint64_t value,
                              void *context)
{
    epoll_event_ex event;
    epoll_event_ex extra;
    int count;

    memset(&event, 0, sizeof(event));
    count = ep_port_wait(port, &event, 1, 2000, NULL);
    if (count != 1 || event.data.u64 != value ||
        event.user_ctx != context || (event.events & EPOLLIN) == 0 ||
        (event.flags & WEPOLL_FLAG_ONESHOT_FIRED) == 0) {
        ep_set_errno(EIO);
        return -1;
    }

    memset(&extra, 0, sizeof(extra));
    if (ep_port_wait(port, &extra, 1, 0, NULL) != 0) {
        ep_set_errno(EIO);
        return -1;
    }
    return 0;
}

static int test_queued_rearm(void)
{
    static const uint64_t value = UINT64_C(0x1111222233334444);
    state_fixture_t fixture;
    ep_sock_t *sock;
    uint64_t queued_generation;
    int context;
    int state_ok;
    int result = -1;

#ifdef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    if (test_cached_base_submit() != 0) return -1;
#endif
    if (fixture_open(&fixture) != 0) return -1;
    if (register_oneshot(&fixture, value, &context) != 0 ||
        send_byte(fixture.client) != 0 ||
        pump_iocp(fixture.port, 2000) < 1) {
        goto cleanup;
    }

    sock = fixture_sock(&fixture);
    if (sock == NULL) goto cleanup;
    pthread_mutex_lock(&fixture.port->fd_table_lock);
    queued_generation = sock->generation;
    state_ok = sock->oneshot_fired != 0 &&
        fixture.port->oneshot_fired_count == 1 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) != 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        fixture.port->needs_rearm_count == 0 &&
        atomic_load_explicit(&fixture.port->ready_queue.queued,
                             memory_order_relaxed) == 1;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok || ep_port_rearm(fixture.port, fixture.server) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = sock->generation != queued_generation &&
        sock->oneshot_fired == 0 &&
        fixture.port->oneshot_fired_count == 0 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        fixture.port->needs_rearm_count == 1;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok || expect_one_oneshot(fixture.port, value, &context) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    fixture_close(&fixture);
    return result;
}

static int test_queued_mod(void)
{
    static const uint64_t old_value = UINT64_C(0x2122232425262728);
    static const uint64_t new_value = UINT64_C(0x3132333435363738);
    state_fixture_t fixture;
    PNtCancelIoFileEx original_cancel;
    ep_sock_t *sock;
    uint64_t queued_generation;
    int old_context;
    int new_context;
    int state_ok;
    int result = -1;

    if (fixture_open(&fixture) != 0) return -1;
    if (register_oneshot(&fixture, old_value, &old_context) != 0 ||
        send_byte(fixture.client) != 0 ||
        pump_iocp(fixture.port, 2000) < 1) {
        goto cleanup;
    }

    sock = fixture_sock(&fixture);
    if (sock == NULL) goto cleanup;
    pthread_mutex_lock(&fixture.port->fd_table_lock);
    queued_generation = sock->generation;
    state_ok = sock->oneshot_fired != 0 &&
        fixture.port->oneshot_fired_count == 1 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) != 0;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok) {
        goto cleanup;
    }

    original_cancel = g_ntdll.NtCancelIoFileEx;
    counted_cancel_delegate = original_cancel;
    InterlockedExchange(&counted_cancel_calls, 0);
    g_ntdll.NtCancelIoFileEx = counted_cancel_stub;
    state_ok = modify_oneshot(&fixture, new_value, &new_context) == 0;
    g_ntdll.NtCancelIoFileEx = original_cancel;
    if (!state_ok) goto cleanup;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = InterlockedCompareExchange(&counted_cancel_calls, 0, 0) == 0 &&
        sock->generation != queued_generation &&
        sock->user_data.u64 == new_value &&
        sock->user_ctx == &new_context && sock->oneshot_fired == 0 &&
        fixture.port->oneshot_fired_count == 0 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        fixture.port->needs_rearm_count == 1;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok ||
        expect_one_oneshot(fixture.port, new_value, &new_context) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    fixture_close(&fixture);
    return result;
}

static int test_deferred_add_failure(void)
{
    static const uint64_t value = UINT64_C(0x6164646661696c31);
    state_fixture_t fixture;
    PNtDeviceIoControlFile original_submit = NULL;
    ep_sock_t *sock;
    epoll_data_t data;
    epoll_event_ex output;
    int context;
    int submit_stub_installed = 0;
    int register_result;
    int wait_result;
    int wait_error;
    int state_ok;
    int result = -1;

    if (fixture_open(&fixture) != 0) return -1;
    memset(&data, 0, sizeof(data));
    data.u64 = value;

    original_submit = g_ntdll.NtDeviceIoControlFile;
    counted_submit_delegate = submit_failure_stub;
    InterlockedExchange(&counted_submit_calls, 0);
    g_ntdll.NtDeviceIoControlFile = counted_submit_stub;
    submit_stub_installed = 1;

    register_result = ep_port_register(
        fixture.port, fixture.server, EPOLLIN | EPOLLONESHOT,
        EPOLLONESHOT, data, &context);
    memset(&output, 0, sizeof(output));
    errno = 0;
    wait_result = ep_port_wait(fixture.port, &output, 1, 0, NULL);
    wait_error = errno;

    g_ntdll.NtDeviceIoControlFile = original_submit;
    submit_stub_installed = 0;
    sock = fixture_sock(&fixture);
    if (sock == NULL) goto cleanup;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = register_result == 0 && wait_result == -1 &&
        wait_error == EACCES &&
        InterlockedCompareExchange(&counted_submit_calls, 0, 0) == 1 &&
        fixture.port->fd_table_count == 1 &&
        fixture.port->needs_rearm_count == 1 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok || send_byte(fixture.client) != 0 ||
        expect_one_oneshot(fixture.port, value, &context) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (submit_stub_installed)
        g_ntdll.NtDeviceIoControlFile = original_submit;
    fixture_close(&fixture);
    return result;
}

static int test_failed_mod_rollback(void)
{
    static const uint64_t old_value = UINT64_C(0x7172737475767778);
    static const uint64_t new_value = UINT64_C(0x8182838485868788);
    state_fixture_t fixture;
    PNtDeviceIoControlFile original_submit;
    ep_sock_t *sock;
    uint64_t old_generation;
    int old_context;
    int new_context;
    int modify_error;
    int modify_result;
    int state_ok;
    int result = -1;

    if (fixture_open(&fixture) != 0) return -1;
    if (register_oneshot(&fixture, old_value, &old_context) != 0 ||
        send_byte(fixture.client) != 0 ||
        pump_iocp(fixture.port, 2000) < 1) {
        goto cleanup;
    }
    sock = fixture_sock(&fixture);
    if (sock == NULL) goto cleanup;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    old_generation = sock->generation;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);

    original_submit = g_ntdll.NtDeviceIoControlFile;
    g_ntdll.NtDeviceIoControlFile = submit_failure_stub;
    atomic_store_explicit(&fixture.port->waiter_active, 1,
                          memory_order_release);
    errno = 0;
    modify_result = modify_oneshot(&fixture, new_value, &new_context);
    modify_error = errno;
    atomic_store_explicit(&fixture.port->waiter_active, 0,
                          memory_order_release);
    g_ntdll.NtDeviceIoControlFile = original_submit;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = modify_result == -1 && modify_error == EACCES &&
        sock->generation == old_generation &&
        sock->user_data.u64 == old_value && sock->user_ctx == &old_context &&
        sock->oneshot_fired != 0 &&
        fixture.port->oneshot_fired_count == 1 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) != 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        fixture.port->needs_rearm_count == 0;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok ||
        expect_one_oneshot(fixture.port, old_value, &old_context) != 0 ||
        modify_oneshot(&fixture, new_value, &new_context) != 0 ||
        expect_one_oneshot(fixture.port, new_value, &new_context) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    fixture_close(&fixture);
    return result;
}

static int test_failed_rearm_rollback(void)
{
    static const uint64_t value = UINT64_C(0x9192939495969798);
    state_fixture_t fixture;
    PNtDeviceIoControlFile original_submit;
    ep_sock_t *sock;
    uint64_t old_generation;
    int context;
    int rearm_error;
    int rearm_result;
    int state_ok;
    int result = -1;

    if (fixture_open(&fixture) != 0) return -1;
    if (register_oneshot(&fixture, value, &context) != 0 ||
        send_byte(fixture.client) != 0 ||
        pump_iocp(fixture.port, 2000) < 1) {
        goto cleanup;
    }
    sock = fixture_sock(&fixture);
    if (sock == NULL) goto cleanup;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    old_generation = sock->generation;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);

    original_submit = g_ntdll.NtDeviceIoControlFile;
    g_ntdll.NtDeviceIoControlFile = submit_failure_stub;
    atomic_store_explicit(&fixture.port->waiter_active, 1,
                          memory_order_release);
    errno = 0;
    rearm_result = ep_port_rearm(fixture.port, fixture.server);
    rearm_error = errno;
    atomic_store_explicit(&fixture.port->waiter_active, 0,
                          memory_order_release);
    g_ntdll.NtDeviceIoControlFile = original_submit;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = rearm_result == -1 && rearm_error == EACCES &&
        sock->generation == old_generation && sock->oneshot_fired != 0 &&
        fixture.port->oneshot_fired_count == 1 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) != 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        fixture.port->needs_rearm_count == 0;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok ||
        expect_one_oneshot(fixture.port, value, &context) != 0 ||
        ep_port_rearm(fixture.port, fixture.server) != 0 ||
        expect_one_oneshot(fixture.port, value, &context) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    fixture_close(&fixture);
    return result;
}

static int test_pending_metadata_mod(void)
{
    static const uint64_t old_value = UINT64_C(0x4142434445464748);
    static const uint64_t new_value = UINT64_C(0x5152535455565758);
    state_fixture_t fixture;
    ep_sock_t *sock;
    PNtCancelIoFileEx original_cancel;
    uint32_t submitted_mask;
    int old_context;
    int new_context;
    int state_ok;
    int result = -1;

    if (fixture_open(&fixture) != 0) return -1;
    if (register_oneshot(&fixture, old_value, &old_context) != 0)
        goto cleanup;
    sock = fixture_sock(&fixture);
    if (sock == NULL) goto cleanup;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    submitted_mask = sock->submitted_afd_events;
    state_ok = atomic_load_explicit(&sock->poll_status,
                                    memory_order_relaxed) == EP_POLL_PENDING &&
        submitted_mask == ep_epoll_to_afd_events(EPOLLIN | EPOLLONESHOT);
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok) goto cleanup;

    original_cancel = g_ntdll.NtCancelIoFileEx;
    counted_cancel_delegate = original_cancel;
    InterlockedExchange(&counted_cancel_calls, 0);
    g_ntdll.NtCancelIoFileEx = counted_cancel_stub;
    state_ok = modify_oneshot(&fixture, new_value, &new_context) == 0;
    g_ntdll.NtCancelIoFileEx = original_cancel;
    if (!state_ok) goto cleanup;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = InterlockedCompareExchange(&counted_cancel_calls, 0, 0) == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING &&
        sock->submitted_afd_events == submitted_mask &&
        sock->user_data.u64 == new_value && sock->user_ctx == &new_context &&
        fixture.port->needs_rearm_count == 0;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok || send_byte(fixture.client) != 0 ||
        expect_one_oneshot(fixture.port, new_value, &new_context) != 0)
        goto cleanup;
    result = 0;

cleanup:
    fixture_close(&fixture);
    return result;
}

static int test_pending_narrowing_mod(void)
{
    static const uint64_t old_value = UINT64_C(0x6162636465666768);
    static const uint64_t new_value = UINT64_C(0x7172737475767778);
    const uint32_t old_events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT;
    const uint32_t new_events = EPOLLRDHUP | EPOLLONESHOT;
    state_fixture_t fixture;
    ep_sock_t *sock;
    PNtCancelIoFileEx original_cancel;
    uint32_t submitted_mask;
    epoll_event_ex event;
    epoll_event_ex extra;
    int old_context;
    int new_context;
    int state_ok;
    int result = -1;

    if (fixture_open(&fixture) != 0) return -1;
    if (register_events(&fixture, old_events, EPOLLONESHOT,
                        old_value, &old_context) != 0)
        goto cleanup;
    sock = fixture_sock(&fixture);
    if (sock == NULL) goto cleanup;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    submitted_mask = sock->submitted_afd_events;
    state_ok = atomic_load_explicit(&sock->poll_status,
                                    memory_order_relaxed) == EP_POLL_PENDING;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok) goto cleanup;

    original_cancel = g_ntdll.NtCancelIoFileEx;
    counted_cancel_delegate = original_cancel;
    InterlockedExchange(&counted_cancel_calls, 0);
    g_ntdll.NtCancelIoFileEx = counted_cancel_stub;
    state_ok = modify_events(&fixture, new_events, EPOLLONESHOT,
                             new_value, &new_context) == 0;
    g_ntdll.NtCancelIoFileEx = original_cancel;
    if (!state_ok) goto cleanup;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = InterlockedCompareExchange(&counted_cancel_calls, 0, 0) == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING &&
        (ep_epoll_to_afd_events(new_events) & ~submitted_mask) == 0 &&
        sock->submitted_afd_events == submitted_mask &&
        sock->user_data.u64 == new_value && sock->user_ctx == &new_context &&
        fixture.port->needs_rearm_count == 0;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok || send_byte(fixture.client) != 0) goto cleanup;

    /* RECEIVE was part of the old request but not the narrowed interest.
     * Its completion must be filtered, then re-arm only the retained mask. */
    memset(&event, 0, sizeof(event));
    if (ep_port_wait(fixture.port, &event, 1, 100, NULL) != 0) goto cleanup;
    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = atomic_load_explicit(&sock->poll_status,
                                    memory_order_relaxed) == EP_POLL_PENDING &&
        sock->submitted_afd_events == ep_epoll_to_afd_events(new_events) &&
        sock->user_data.u64 == new_value && sock->user_ctx == &new_context &&
        fixture.port->needs_rearm_count == 0;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok || shutdown(fixture.client, SD_SEND) == SOCKET_ERROR)
        goto cleanup;

    memset(&event, 0, sizeof(event));
    if (ep_port_wait(fixture.port, &event, 1, 2000, NULL) != 1 ||
        event.data.u64 != new_value || event.user_ctx != &new_context ||
        (event.events & EPOLLRDHUP) == 0 ||
        (event.events & EPOLLIN) != 0 ||
        (event.flags & WEPOLL_FLAG_ONESHOT_FIRED) == 0) {
        goto cleanup;
    }
    memset(&extra, 0, sizeof(extra));
    if (ep_port_wait(fixture.port, &extra, 1, 0, NULL) != 0) goto cleanup;
    result = 0;

cleanup:
    fixture_close(&fixture);
    return result;
}

static int test_pending_expansion_cancelled_mod(void)
{
    static const uint64_t old_value = UINT64_C(0x8182838485868788);
    static const uint64_t new_value = UINT64_C(0x9192939495969798);
    static const uint64_t latest_value = UINT64_C(0xa1a2a3a4a5a6a7a8);
    const uint32_t old_events = EPOLLRDHUP | EPOLLONESHOT;
    const uint32_t new_events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT;
    state_fixture_t fixture;
    ep_sock_t *sock;
    PNtCancelIoFileEx original_cancel;
    int old_context;
    int new_context;
    int latest_context;
    int state_ok;
    int result = -1;

    if (fixture_open(&fixture) != 0) return -1;
    if (register_events(&fixture, old_events, EPOLLONESHOT,
                        old_value, &old_context) != 0)
        goto cleanup;
    sock = fixture_sock(&fixture);
    if (sock == NULL) goto cleanup;

    original_cancel = g_ntdll.NtCancelIoFileEx;
    counted_cancel_delegate = original_cancel;
    InterlockedExchange(&counted_cancel_calls, 0);
    g_ntdll.NtCancelIoFileEx = counted_cancel_stub;
    if (modify_events(&fixture, new_events, EPOLLONESHOT,
                      new_value, &new_context) != 0)
        goto restore_cancel;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = InterlockedCompareExchange(&counted_cancel_calls, 0, 0) == 1 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_CANCELLED &&
        sock->user_data.u64 == new_value && sock->user_ctx == &new_context &&
        fixture.port->needs_rearm_count == 1;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok) goto restore_cancel;

    if (modify_events(&fixture, new_events, EPOLLONESHOT,
                      latest_value, &latest_context) != 0)
        goto restore_cancel;
    g_ntdll.NtCancelIoFileEx = original_cancel;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = InterlockedCompareExchange(&counted_cancel_calls, 0, 0) == 1 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_CANCELLED &&
        sock->user_data.u64 == latest_value &&
        sock->user_ctx == &latest_context &&
        fixture.port->needs_rearm_count == 1;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok || send_byte(fixture.client) != 0 ||
        expect_one_oneshot(fixture.port, latest_value, &latest_context) != 0)
        goto cleanup;
    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = fixture.port->needs_rearm_count == 0;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok) goto cleanup;
    result = 0;
    goto cleanup;

restore_cancel:
    g_ntdll.NtCancelIoFileEx = original_cancel;
cleanup:
    fixture_close(&fixture);
    return result;
}

static int test_transitional_idle(void)
{
    state_fixture_t fixture;
    PNtDeviceIoControlFile original_submit;
    epoll_data_t data;
    epoll_event_ex event;
    size_t fd_count;
    int context;
    int wait_result;
    int result = -1;

    fixture_reset(&fixture);
    fixture.server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fixture.server == INVALID_SOCKET ||
        ep_port_create(0, 0, &fixture.port) != 0) {
        goto cleanup;
    }

    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0xb1b2b3b4b5b6b7b8);
    original_submit = g_ntdll.NtDeviceIoControlFile;
    counted_submit_delegate = original_submit;
    InterlockedExchange(&counted_submit_calls, 0);
    g_ntdll.NtDeviceIoControlFile = counted_submit_stub;
    if (ep_port_register(fixture.port, fixture.server, EPOLLIN, 0,
                         data, &context) != 0) {
        g_ntdll.NtDeviceIoControlFile = original_submit;
        goto cleanup;
    }
    memset(&event, 0, sizeof(event));
    wait_result = ep_port_wait(fixture.port, &event, 1, 200, NULL);
    g_ntdll.NtDeviceIoControlFile = original_submit;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    fd_count = fixture.port->fd_table_count;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);

    /* One request is submitted either by ADD (hardened identity mode) or by
     * the first wait (synchronized-lifetime mode).  More submissions mean a
     * broad transitional mask is completing and re-arming in a hot loop. */
    if (wait_result == 0 &&
        InterlockedCompareExchange(&counted_submit_calls, 0, 0) == 1 &&
        fd_count == 1) {
        result = 0;
    }

cleanup:
    fixture_close(&fixture);
    return result;
}

int main(int argc, char **argv)
{
    WSADATA wsa_data;
    int result = -1;

    if (argc != 2 || WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return 2;
    }
    if (strcmp(argv[1], "queued-rearm") == 0) {
        result = test_queued_rearm();
    } else if (strcmp(argv[1], "queued-mod") == 0) {
        result = test_queued_mod();
    } else if (strcmp(argv[1], "deferred-add-failure") == 0) {
        result = test_deferred_add_failure();
    } else if (strcmp(argv[1], "failed-mod") == 0) {
        result = test_failed_mod_rollback();
    } else if (strcmp(argv[1], "failed-rearm") == 0) {
        result = test_failed_rearm_rollback();
    } else if (strcmp(argv[1], "pending-metadata-mod") == 0) {
        result = test_pending_metadata_mod();
    } else if (strcmp(argv[1], "pending-narrowing-mod") == 0) {
        result = test_pending_narrowing_mod();
    } else if (strcmp(argv[1], "pending-expansion-mod") == 0) {
        result = test_pending_expansion_cancelled_mod();
    } else if (strcmp(argv[1], "transitional-idle") == 0) {
        result = test_transitional_idle();
    }
    (void)WSACleanup();

    if (result != 0) {
        fprintf(stderr, "state mode failed: %s (errno=%d)\n",
                argc > 1 ? argv[1] : "missing", errno);
        return 1;
    }
    return 0;
}
