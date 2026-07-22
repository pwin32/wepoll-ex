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
    size_t pending = port->pending_poll_count;
    pthread_mutex_unlock(&port->fd_table_lock);
    if (pending == 0) goto fail;

    *port_out = port;
    *socket_out = socket_fd;
    return 0;

fail:
    if (socket_fd != INVALID_SOCKET) closesocket(socket_fd);
    if (port != NULL) (void)ep_port_destroy(port);
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

static int test_cancel_failure(void)
{
    ep_port_t *port;
    SOCKET socket_fd;
    PNtCancelIoFileEx original_cancel;
    int result;
    int error;

    if (make_pending_port(&port, &socket_fd) != 0) return -1;
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
    } else if (strcmp(argv[1], "iocp-failure") == 0) {
        result = test_iocp_failure();
    } else if (strcmp(argv[1], "drain-timeout") == 0) {
        result = test_drain_timeout();
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
