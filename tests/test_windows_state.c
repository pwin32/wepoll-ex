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

static int register_oneshot(state_fixture_t *fixture, uint64_t value,
                            void *context)
{
    epoll_data_t data;

    memset(&data, 0, sizeof(data));
    data.u64 = value;
    return ep_port_register(fixture->port, fixture->server,
                            EPOLLIN | EPOLLONESHOT, EPOLLONESHOT,
                            data, context);
}

static int modify_oneshot(state_fixture_t *fixture, uint64_t value,
                          void *context)
{
    epoll_data_t data;

    memset(&data, 0, sizeof(data));
    data.u64 = value;
    return ep_port_modify(fixture->port, fixture->server,
                          EPOLLIN | EPOLLONESHOT, EPOLLONESHOT,
                          data, context);
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
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) != 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&fixture.port->ready_queue.queued,
                             memory_order_relaxed) == 1;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok || ep_port_rearm(fixture.port, fixture.server) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = sock->generation != queued_generation &&
        sock->oneshot_fired == 0 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING;
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
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) != 0;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok ||
        modify_oneshot(&fixture, new_value, &new_context) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = sock->generation != queued_generation &&
        sock->user_data.u64 == new_value &&
        sock->user_ctx == &new_context && sock->oneshot_fired == 0 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING;
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
    errno = 0;
    modify_result = modify_oneshot(&fixture, new_value, &new_context);
    modify_error = errno;
    g_ntdll.NtDeviceIoControlFile = original_submit;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = modify_result == -1 && modify_error == EACCES &&
        sock->generation == old_generation &&
        sock->user_data.u64 == old_value && sock->user_ctx == &old_context &&
        sock->oneshot_fired != 0 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) != 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE;
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
    errno = 0;
    rearm_result = ep_port_rearm(fixture.port, fixture.server);
    rearm_error = errno;
    g_ntdll.NtDeviceIoControlFile = original_submit;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = rearm_result == -1 && rearm_error == EACCES &&
        sock->generation == old_generation && sock->oneshot_fired != 0 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) != 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE;
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

static int test_rapid_pending_mod(void)
{
    static const uint64_t value_a = UINT64_C(0x4142434445464748);
    static const uint64_t value_b = UINT64_C(0x5152535455565758);
    static const uint64_t value_c = UINT64_C(0x6162636465666768);
    state_fixture_t fixture;
    ep_sock_t *sock;
    uint64_t generation_a;
    uint64_t generation_b;
    uint64_t generation_c;
    int context_a;
    int context_b;
    int context_c;
    int state_ok;
    int result = -1;

    if (fixture_open(&fixture) != 0) return -1;
    if (register_oneshot(&fixture, value_a, &context_a) != 0) goto cleanup;
    sock = fixture_sock(&fixture);
    if (sock == NULL) goto cleanup;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    generation_a = sock->generation;
    state_ok = atomic_load_explicit(&sock->poll_status,
                                    memory_order_relaxed) == EP_POLL_PENDING;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok || modify_oneshot(&fixture, value_b, &context_b) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    generation_b = sock->generation;
    state_ok = generation_b != generation_a &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_CANCELLED;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok || modify_oneshot(&fixture, value_c, &context_c) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    generation_c = sock->generation;
    state_ok = generation_c != generation_b &&
        sock->user_data.u64 == value_c && sock->user_ctx == &context_c &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_CANCELLED;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok || send_byte(fixture.client) != 0 ||
        expect_one_oneshot(fixture.port, value_c, &context_c) != 0) {
        goto cleanup;
    }
    result = 0;

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
    } else if (strcmp(argv[1], "failed-mod") == 0) {
        result = test_failed_mod_rollback();
    } else if (strcmp(argv[1], "failed-rearm") == 0) {
        result = test_failed_rearm_rollback();
    } else if (strcmp(argv[1], "rapid-pending-mod") == 0) {
        result = test_rapid_pending_mod();
    }
    (void)WSACleanup();

    if (result != 0) {
        fprintf(stderr, "state mode failed: %s (errno=%d)\n",
                argc > 1 ? argv[1] : "missing", errno);
        return 1;
    }
    return 0;
}
