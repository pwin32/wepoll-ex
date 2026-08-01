/* Internal Windows shutdown fault tests.  Each mode runs in its own process
 * so a regression is bounded by the corresponding CTest timeout. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef STATUS_ACCESS_DENIED
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022L)
#endif

static ep_sock_t *find_registered_sock(ep_port_t *port, SOCKET socket_fd)
{
    for (size_t i = 0; i < port->fd_table_size; i++) {
        ep_sock_t *sock = port->fd_table[i];
        if (sock != NULL && sock->fd == socket_fd) {
            return sock;
        }
    }
    return NULL;
}

static int make_pending_port(ep_port_t **port_out, SOCKET *socket_out)
{
    ep_port_t *port = NULL;
    SOCKET socket_fd = INVALID_SOCKET;
    struct sockaddr_in address;
    epoll_data_t data;

    *port_out = NULL;
    *socket_out = INVALID_SOCKET;
    if (ep_port_create(0, 0, &port) != 0) return -1;

    socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd == INVALID_SOCKET) goto fail;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (bind(socket_fd, (const struct sockaddr *)&address,
             (int)sizeof(address)) == SOCKET_ERROR) {
        goto fail;
    }

    memset(&data, 0, sizeof(data));
    if (ep_port_register(port, socket_fd, EPOLLIN, 0, data, NULL) != 0) {
        goto fail;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    ep_sock_t *sock = find_registered_sock(port, socket_fd);
    int armed = sock != NULL &&
        atomic_load_explicit(&sock->state, memory_order_relaxed) ==
            EP_SOCK_POLLING &&
        atomic_load_explicit(&sock->poll_status, memory_order_relaxed) ==
            EP_POLL_PENDING &&
        !sock->needs_rearm &&
        port->pending_poll_count == 1;
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!armed) goto fail;

    *port_out = port;
    *socket_out = socket_fd;
    return 0;

fail:
    if (port != NULL) (void)ep_port_destroy(port);
    if (socket_fd != INVALID_SOCKET) closesocket(socket_fd);
    return -1;
}

static NTSTATUS NTAPI cancel_failure_stub(
    HANDLE file_handle,
    PIO_STATUS_BLOCK request,
    PIO_STATUS_BLOCK status)
{
    (void)file_handle;
    (void)request;
    (void)status;
    return STATUS_ACCESS_DENIED;
}

static PGetQueuedCompletionStatusEx late_reap_native_dequeue;
static volatile LONG late_reap_allow_dequeue;

static BOOL WINAPI late_reap_dequeue_stub(
    HANDLE completion_port,
    OVERLAPPED_ENTRY *entries,
    ULONG count,
    PULONG removed,
    DWORD milliseconds,
    BOOL alertable)
{
    if (InterlockedCompareExchange(&late_reap_allow_dequeue, 0, 0) == 0) {
        DWORD delay = milliseconds == 0 ? 1 : milliseconds;
        if (delay > 10) delay = 10;
        Sleep(delay);
        *removed = 0;
        SetLastError(WAIT_TIMEOUT);
        return FALSE;
    }
    return late_reap_native_dequeue(completion_port, entries, count,
                                    removed, milliseconds, alertable);
}

static int test_cancel_failure(void)
{
    ep_port_t *port;
    SOCKET socket_fd;
    PNtCancelIoFileEx original_cancel;
    int result;
    int error;

    if (make_pending_port(&port, &socket_fd) != 0) return -1;
    /* No wait has run: close must cancel the request armed by ADD itself. */
    original_cancel = g_ntdll.NtCancelIoFileEx;
    g_ntdll.NtCancelIoFileEx = cancel_failure_stub;
    errno = 0;
    result = ep_port_destroy(port);
    error = errno;
    g_ntdll.NtCancelIoFileEx = original_cancel;
    closesocket(socket_fd);

    if (result == 0) return 0;
    return result == -1 &&
           (error == EIO || error == ETIMEDOUT || error == EBADF)
        ? 0 : -1;
}

static int test_del_cancel_failure(void)
{
    /* A socket whose AFD poll request is still pending may not be returned by
     * the Windows handle allocator immediately after closesocket().  Keep a
     * bounded batch of candidates open so the test reaches numeric reuse on
     * normal Windows builds without making reuse itself an optional check. */
    enum { MAX_CANDIDATES = 4096 };
    static const char byte = 'd';
    static const uint64_t replacement_data =
        UINT64_C(0xdedecafefeed1234);
    ep_port_t *port = NULL;
    SOCKET socket_fd = INVALID_SOCKET;
    SOCKET replacement = INVALID_SOCKET;
    SOCKET sender = INVALID_SOCKET;
    SOCKET candidates[MAX_CANDIDATES];
    struct sockaddr_in address;
    epoll_data_t data;
    epoll_event_ex event;
    PNtCancelIoFileEx original_cancel = NULL;
    int cancel_stub_installed = 0;
    int candidate_count = 0;
    int result = -1;
    char received;

    for (int i = 0; i < MAX_CANDIDATES; i++) {
        candidates[i] = INVALID_SOCKET;
    }
    if (make_pending_port(&port, &socket_fd) != 0) goto cleanup;

    /* No wait has run: DEL must detach the request armed by ADD itself. */
    original_cancel = g_ntdll.NtCancelIoFileEx;
    g_ntdll.NtCancelIoFileEx = cancel_failure_stub;
    cancel_stub_installed = 1;
    if (ep_port_unregister(port, socket_fd) != 0) {
        goto cleanup;
    }
    g_ntdll.NtCancelIoFileEx = original_cancel;
    cancel_stub_installed = 0;

    pthread_mutex_lock(&port->fd_table_lock);
    ep_sock_t *detached = port->sock_list_head;
    int detached_ok =
        find_registered_sock(port, socket_fd) == NULL &&
        detached != NULL && detached->fd == socket_fd &&
        atomic_load_explicit(&detached->delete_pending,
                             memory_order_relaxed) &&
        atomic_load_explicit(&detached->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING &&
        port->fd_table_count == 0;
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!detached_ok) goto cleanup;

    SOCKET reused_fd = socket_fd;
    if (closesocket(socket_fd) == SOCKET_ERROR) goto cleanup;
    socket_fd = INVALID_SOCKET;

    for (; candidate_count < MAX_CANDIDATES; candidate_count++) {
        SOCKET candidate = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (candidate == INVALID_SOCKET) goto cleanup;
        if (candidate == reused_fd) {
            replacement = candidate;
            break;
        }
        candidates[candidate_count] = candidate;
    }
    if (replacement == INVALID_SOCKET) goto cleanup;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (bind(replacement, (const struct sockaddr *)&address,
             (int)sizeof(address)) == SOCKET_ERROR) {
        goto cleanup;
    }
    int address_length = (int)sizeof(address);
    if (getsockname(replacement, (struct sockaddr *)&address,
                    &address_length) == SOCKET_ERROR) {
        goto cleanup;
    }

    memset(&data, 0, sizeof(data));
    data.u64 = replacement_data;
    if (ep_port_register(port, replacement, EPOLLIN, 0, data, NULL) != 0) {
        goto cleanup;
    }
    sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sender == INVALID_SOCKET ||
        sendto(sender, &byte, 1, 0, (const struct sockaddr *)&address,
               (int)sizeof(address)) != 1 ||
        ep_port_wait(port, &event, 1, 2000, NULL) != 1 ||
        (event.events & EPOLLIN) == 0 ||
        event.data.u64 != replacement_data ||
        recv(replacement, &received, 1, 0) != 1 || received != byte) {
        goto cleanup;
    }

    if (ep_port_unregister(port, replacement) != 0 ||
        closesocket(replacement) == SOCKET_ERROR) {
        goto cleanup;
    }
    replacement = INVALID_SOCKET;

    {
        int destroy_result = ep_port_destroy(port);
        port = NULL;
        if (destroy_result != 0) goto cleanup;
    }
    result = 0;

cleanup:
    if (cancel_stub_installed) {
        g_ntdll.NtCancelIoFileEx = original_cancel;
    }
    if (socket_fd != INVALID_SOCKET) closesocket(socket_fd);
    if (replacement != INVALID_SOCKET) closesocket(replacement);
    if (sender != INVALID_SOCKET) closesocket(sender);
    for (int i = 0; i < candidate_count; i++) {
        if (candidates[i] != INVALID_SOCKET) closesocket(candidates[i]);
    }
    if (port != NULL) (void)ep_port_destroy(port);
    return result;
}

static int test_iocp_failure(void)
{
    ep_port_t *port;
    SOCKET socket_fd;
    int result;
    int error;

    if (make_pending_port(&port, &socket_fd) != 0) return -1;
    ep_port_begin_close(port);
    if (!CloseHandle(port->iocp)) {
        closesocket(socket_fd);
        return -1;
    }

    errno = 0;
    result = ep_port_destroy(port);
    error = errno;
    closesocket(socket_fd);
    return result == -1 && (error == EBADF || error == EIO) ? 0 : -1;
}

static int test_drain_timeout(void)
{
    ep_port_t *port = NULL;
    ULONGLONG started;
    ULONGLONG elapsed;
    int result;
    int error;

    if (ep_port_create(0, 0, &port) != 0) return -1;
    pthread_mutex_lock(&port->fd_table_lock);
    port->pending_poll_count = 1;
    pthread_mutex_unlock(&port->fd_table_lock);

    started = GetTickCount64();
    errno = 0;
    result = ep_port_destroy(port);
    elapsed = GetTickCount64() - started;
    error = errno;
    return result == -1 && error == ETIMEDOUT &&
           elapsed >= 4000 && elapsed < 12000 ? 0 : -1;
}

static int test_late_completion_reap(void)
{
    wepoll_ex_global_stats before;
    wepoll_ex_global_stats after;
    ep_port_t *port = NULL;
    SOCKET socket_fd = INVALID_SOCKET;
    int destroy_result;
    int destroy_error;

    if (wepoll_ex_get_global_stats(&before, sizeof(before)) != 0 ||
        make_pending_port(&port, &socket_fd) != 0) {
        return -1;
    }

    late_reap_native_dequeue = GetQueuedCompletionStatusEx;
    InterlockedExchange(&late_reap_allow_dequeue, 0);
    port->get_queued_completion_status_ex = late_reap_dequeue_stub;
    port->close_drain_timeout_ms = 20;
    port->quarantine_drain_timeout_ms = 2000;

    errno = 0;
    destroy_result = ep_port_destroy(port);
    destroy_error = errno;
    port = NULL; /* timeout transfers ownership to the detached reaper */
    if (destroy_result != -1 || destroy_error != ETIMEDOUT) {
        if (socket_fd != INVALID_SOCKET) closesocket(socket_fd);
        return -1;
    }

    InterlockedExchange(&late_reap_allow_dequeue, 1);
    for (int attempt = 0; attempt < 300; attempt++) {
        if (wepoll_ex_get_global_stats(&after, sizeof(after)) != 0) break;
        if (after.quarantined_ports == before.quarantined_ports + 1 &&
            after.reaped_ports == before.reaped_ports + 1 &&
            after.irrecoverable_ports == before.irrecoverable_ports) {
            closesocket(socket_fd);
            return 0;
        }
        Sleep(10);
    }

    if (socket_fd != INVALID_SOCKET) closesocket(socket_fd);
    return -1;
}

typedef struct internal_wait_context {
    ep_port_t *port;
    HANDLE started;
    int result;
    int error;
} internal_wait_context_t;

static DWORD WINAPI internal_wait_thread(void *opaque)
{
    internal_wait_context_t *context =
        (internal_wait_context_t *)opaque;
    epoll_event_ex event;

    SetEvent(context->started);
    context->result = ep_port_wait(context->port, &event, 1, -1, NULL);
    context->error = errno;
    return 0;
}

static int test_wake_failure(void)
{
    internal_wait_context_t context;
    HANDLE thread = NULL;
    ep_port_t *port = NULL;
    int result = -1;

    memset(&context, 0, sizeof(context));
    context.started = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (context.started == NULL || ep_port_create(0, 0, &port) != 0) {
        goto cleanup;
    }
    context.port = port;
    thread = CreateThread(NULL, 0, internal_wait_thread,
                          &context, 0, NULL);
    if (thread == NULL ||
        WaitForSingleObject(context.started, 2000) != WAIT_OBJECT_0) {
        goto cleanup;
    }
    Sleep(100);

    if (!CloseHandle(port->iocp)) goto cleanup;
    ep_port_begin_close(port);
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) goto cleanup;
    if (context.result != -1 || context.error != EBADF) goto cleanup;
    if (ep_port_destroy(port) != 0) goto cleanup;
    port = NULL;
    result = 0;

cleanup:
    if (port != NULL) (void)ep_port_destroy(port);
    if (thread != NULL &&
        WaitForSingleObject(thread, 0) != WAIT_OBJECT_0) {
        TerminateThread(thread, 1);
    }
    if (thread != NULL) CloseHandle(thread);
    if (context.started != NULL) CloseHandle(context.started);
    return result;
}

int main(int argc, char **argv)
{
    WSADATA wsa_data;
    int result;

    if (argc != 2 || WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return 2;
    }
    if (strcmp(argv[1], "cancel-failure") == 0) {
        result = test_cancel_failure();
    } else if (strcmp(argv[1], "del-cancel-failure") == 0) {
        result = test_del_cancel_failure();
    } else if (strcmp(argv[1], "iocp-failure") == 0) {
        result = test_iocp_failure();
    } else if (strcmp(argv[1], "drain-timeout") == 0) {
        result = test_drain_timeout();
    } else if (strcmp(argv[1], "late-completion-reap") == 0) {
        result = test_late_completion_reap();
    } else if (strcmp(argv[1], "wake-failure") == 0) {
        result = test_wake_failure();
    } else {
        result = -1;
    }
    WSACleanup();
    if (result != 0) {
        fprintf(stderr, "lifecycle mode failed: %s (errno=%d)\n",
                argv[1], errno);
        return 1;
    }
    return 0;
}
