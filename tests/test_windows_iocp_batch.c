/*
 * Focused regressions for Windows IOCP waits.  They cover a zero-timeout wait
 * draining a large ignored/internal packet burst before real readiness and a
 * finite wait retrying an early WAIT_TIMEOUT against its absolute deadline.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex_internal.h"

#ifdef _WIN32

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct tcp_pair {
    SOCKET client;
    SOCKET server;
} tcp_pair_t;

static void tcp_pair_init(tcp_pair_t *pair)
{
    pair->client = INVALID_SOCKET;
    pair->server = INVALID_SOCKET;
}

static void tcp_pair_close(tcp_pair_t *pair)
{
    if (pair->server != INVALID_SOCKET) {
        (void)closesocket(pair->server);
        pair->server = INVALID_SOCKET;
    }
    if (pair->client != INVALID_SOCKET) {
        (void)closesocket(pair->client);
        pair->client = INVALID_SOCKET;
    }
}

static int make_tcp_pair(tcp_pair_t *pair)
{
    SOCKET listener = INVALID_SOCKET;
    struct sockaddr_in address;
    int address_length = (int)sizeof(address);

    tcp_pair_init(pair);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (bind(listener, (const struct sockaddr *)&address,
             (int)sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR ||
        getsockname(listener, (struct sockaddr *)&address,
                    &address_length) == SOCKET_ERROR) {
        (void)closesocket(listener);
        return -1;
    }

    pair->client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pair->client == INVALID_SOCKET ||
        connect(pair->client, (const struct sockaddr *)&address,
                (int)sizeof(address)) == SOCKET_ERROR) {
        (void)closesocket(listener);
        tcp_pair_close(pair);
        return -1;
    }

    pair->server = accept(listener, NULL, NULL);
    (void)closesocket(listener);
    if (pair->server == INVALID_SOCKET) {
        tcp_pair_close(pair);
        return -1;
    }
    return 0;
}

static ep_sock_t *find_registered_socket(ep_port_t *port, SOCKET fd)
{
    ep_sock_t *result = NULL;

    pthread_mutex_lock(&port->fd_table_lock);
    for (size_t i = 0; i < port->fd_table_size; i++) {
        ep_sock_t *sock = port->fd_table[i];
        if (sock != NULL && sock->fd == fd) {
            result = sock;
            break;
        }
    }
    pthread_mutex_unlock(&port->fd_table_lock);
    return result;
}

static NTSTATUS NTAPI submit_pending_stub(
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
    return STATUS_PENDING;
}

static int early_timeout_calls;

static BOOL WINAPI early_timeout_stub(
    HANDLE completion_port,
    OVERLAPPED_ENTRY *entries,
    ULONG count,
    PULONG removed,
    DWORD milliseconds,
    BOOL alertable)
{
    (void)completion_port;
    (void)entries;
    (void)count;
    (void)milliseconds;
    (void)alertable;
    if (removed != NULL) {
        *removed = 0;
    }
    early_timeout_calls++;
    SetLastError(WAIT_TIMEOUT);
    return FALSE;
}

static int test_internal_batch_then_readiness(void)
{
    static const uint64_t expected_data =
        UINT64_C(0x7061636b65746261); /* "packetba" */
    ep_port_t *port = NULL;
    ep_sock_t *sock = NULL;
    tcp_pair_t pair;
    epoll_data_t data;
    epoll_event_ex output;
    epoll_event_ex ignored;
    PNtDeviceIoControlFile original_submit = NULL;
    int submit_stub_installed = 0;
    int completion_posted = 0;
    size_t internal_packet_count = 0;
    int result = -1;
    int wait_result;

    tcp_pair_init(&pair);
    memset(&data, 0, sizeof(data));
    data.u64 = expected_data;

    if (ep_port_create(0, 0, &port) != 0 || make_tcp_pair(&pair) != 0) {
        fprintf(stderr, "batch: setup failed (errno=%d, WSA=%d)\n",
                errno, WSAGetLastError());
        goto cleanup;
    }

    /* Use a real ep_sock_t/IO_STATUS_BLOCK, but suppress the kernel request so
     * the completion order is deterministic and this test never races a live
     * AFD operation. */
    original_submit = g_ntdll.NtDeviceIoControlFile;
    g_ntdll.NtDeviceIoControlFile = submit_pending_stub;
    submit_stub_installed = 1;
    if (ep_port_register(port, pair.server, EPOLLIN, 0, data, NULL) != 0) {
        fprintf(stderr, "batch: registration failed (errno=%d, WSA=%d)\n",
                errno, WSAGetLastError());
        goto cleanup;
    }
    memset(&ignored, 0, sizeof(ignored));
    if (ep_port_wait(port, &ignored, 1, 0, NULL) < 0) {
        fprintf(stderr, "batch: deferred registration arm failed\n");
        goto cleanup;
    }
    g_ntdll.NtDeviceIoControlFile = original_submit;
    submit_stub_installed = 0;

    sock = find_registered_socket(port, pair.server);
    if (sock == NULL) {
        fprintf(stderr, "batch: registered socket was not discoverable\n");
        goto cleanup;
    }

    /* NULL OVERLAPPED packets are the stable internal/wakeup completion form
     * ignored by epoll_wait.  They stand in for cancellation/stale packets
     * without fabricating an unsafe pointer into ep_sock_t storage.  Queue
     * more than eight complete batches so the regression exceeds the former
     * four-batch (256 packet) drain limit. */
    internal_packet_count = (size_t)port->iocp_batch_size * 8U + 1U;
    for (size_t i = 0; i < internal_packet_count; i++) {
        if (!PostQueuedCompletionStatus(port->iocp, 0, 0, NULL)) {
            fprintf(stderr, "batch: PostQueuedCompletionStatus failed\n");
            goto cleanup;
        }
    }

    pthread_mutex_lock(&port->fd_table_lock);
    sock->afd_info->NumberOfHandles = 1;
    sock->afd_info->Handles[0].Events = AFD_POLL_RECEIVE;
    sock->io_status_block.Status = STATUS_SUCCESS;
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!PostQueuedCompletionStatus(port->iocp, 0, 0,
                                    (OVERLAPPED *)&sock->io_status_block)) {
        fprintf(stderr, "batch: readiness completion post failed\n");
        goto cleanup;
    }
    completion_posted = 1;

    memset(&output, 0, sizeof(output));
    wait_result = ep_port_wait(port, &output, 1, 0, NULL);
    if (wait_result != 1 || output.data.u64 != expected_data ||
        (output.events & EPOLLIN) == 0) {
        fprintf(stderr,
                "batch: zero-timeout wait returned %d (errno=%d, data=%llx, events=0x%lx)\n",
                wait_result, errno, (unsigned long long)output.data.u64,
                (unsigned long)output.events);
        goto cleanup;
    }

    result = 0;
    puts("batch: OK");

cleanup:
    if (submit_stub_installed) {
        g_ntdll.NtDeviceIoControlFile = original_submit;
        submit_stub_installed = 0;
    }
    if (port != NULL) {
        if (sock == NULL) {
            sock = find_registered_socket(port, pair.server);
        }
        /* Consume the synthetic request before destruction.  If setup failed
         * before the request was posted, add a cancellation-shaped packet so
         * the stubbed pending poll cannot force a teardown timeout. */
        if (sock != NULL && !completion_posted) {
            sock->io_status_block.Status = STATUS_CANCELLED;
            (void)PostQueuedCompletionStatus(
                port->iocp, 0, 0,
                (OVERLAPPED *)&sock->io_status_block);
            completion_posted = 1;
        }
        if (result != 0 && completion_posted) {
            memset(&output, 0, sizeof(output));
            (void)ep_port_wait(port, &output, 1, 1000, NULL);
        }
        (void)ep_port_destroy(port);
    }
    tcp_pair_close(&pair);
    return result;
}

static int test_finite_wait_retries_early_timeout(void)
{
    static const int timeout_ms = 25;
    ep_port_t *port = NULL;
    epoll_event_ex output;
    PGetQueuedCompletionStatusEx original_dequeue;
    uint64_t started;
    uint64_t elapsed;
    int wait_result;
    int result = -1;

    if (ep_port_create(0, 0, &port) != 0) {
        fprintf(stderr, "timeout: setup failed (errno=%d)\n", errno);
        return -1;
    }

    original_dequeue = port->get_queued_completion_status_ex;
    port->get_queued_completion_status_ex = early_timeout_stub;
    early_timeout_calls = 0;
    memset(&output, 0, sizeof(output));
    started = GetTickCount64();
    wait_result = ep_port_wait(port, &output, 1, timeout_ms, NULL);
    elapsed = GetTickCount64() - started;
    port->get_queued_completion_status_ex = original_dequeue;

    if (wait_result != 0 || elapsed < (uint64_t)timeout_ms ||
        early_timeout_calls < 2) {
        fprintf(stderr,
                "timeout: finite wait returned %d after %llu ms and %d calls (errno=%d)\n",
                wait_result, (unsigned long long)elapsed,
                early_timeout_calls, errno);
        goto cleanup;
    }

    result = 0;
    puts("timeout: OK");

cleanup:
    (void)ep_port_destroy(port);
    return result;
}

int main(void)
{
    WSADATA wsa_data;
    int batch_result;
    int timeout_result;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 2;
    }
    batch_result = test_internal_batch_then_readiness();
    timeout_result = test_finite_wait_retries_early_timeout();
    (void)WSACleanup();
    return batch_result == 0 && timeout_result == 0 ? 0 : 1;
}

#else

int main(void)
{
    return 0;
}

#endif
