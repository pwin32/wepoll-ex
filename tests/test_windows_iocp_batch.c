/*
 * Focused regression for zero-timeout waits that consume an IOCP batch made
 * up only of ignored/internal packets.  The real AFD readiness completion is
 * deliberately queued after one more wake packet than the configured batch
 * can hold.
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

static int test_internal_batch_then_readiness(void)
{
    static const uint64_t expected_data =
        UINT64_C(0x7061636b65746261); /* "packetba" */
    ep_port_t *port = NULL;
    ep_sock_t *sock = NULL;
    tcp_pair_t pair;
    epoll_data_t data;
    epoll_event_ex output;
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
    g_ntdll.NtDeviceIoControlFile = original_submit;
    submit_stub_installed = 0;

    sock = find_registered_socket(port, pair.server);
    if (sock == NULL) {
        fprintf(stderr, "batch: registered socket was not discoverable\n");
        goto cleanup;
    }

    /* NULL OVERLAPPED packets are the stable internal/wakeup completion form
     * ignored by epoll_wait.  They stand in for cancellation/stale packets
     * without fabricating an unsafe pointer into ep_sock_t storage.  One more
     * packet than the configured batch guarantees a second dequeue attempt. */
    internal_packet_count = (size_t)port->iocp_batch_size + 1;
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

int main(void)
{
    WSADATA wsa_data;
    int result;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 2;
    }
    result = test_internal_batch_then_readiness();
    (void)WSACleanup();
    return result == 0 ? 0 : 1;
}

#else

int main(void)
{
    return 0;
}

#endif
