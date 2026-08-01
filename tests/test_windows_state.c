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

static int fixture_open_udp(state_fixture_t *fixture)
{
    struct sockaddr_in client_address;
    struct sockaddr_in server_address;
    int client_address_length = (int)sizeof(client_address);
    int server_address_length = (int)sizeof(server_address);

    fixture_reset(fixture);
    fixture->client = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP,
                                 NULL, 0, WSA_FLAG_OVERLAPPED);
    fixture->server = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP,
                                 NULL, 0, WSA_FLAG_OVERLAPPED);
    if (fixture->client == INVALID_SOCKET ||
        fixture->server == INVALID_SOCKET) {
        goto fail;
    }

    memset(&client_address, 0, sizeof(client_address));
    client_address.sin_family = AF_INET;
    client_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    client_address.sin_port = htons(0);
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_address.sin_port = htons(0);
    if (bind(fixture->client,
             (const struct sockaddr *)&client_address,
             (int)sizeof(client_address)) == SOCKET_ERROR ||
        bind(fixture->server,
             (const struct sockaddr *)&server_address,
             (int)sizeof(server_address)) == SOCKET_ERROR ||
        getsockname(fixture->client,
                    (struct sockaddr *)&client_address,
                    &client_address_length) == SOCKET_ERROR ||
        getsockname(fixture->server,
                    (struct sockaddr *)&server_address,
                    &server_address_length) == SOCKET_ERROR ||
        connect(fixture->client,
                (const struct sockaddr *)&server_address,
                server_address_length) == SOCKET_ERROR ||
        connect(fixture->server,
                (const struct sockaddr *)&client_address,
                client_address_length) == SOCKET_ERROR ||
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

static ep_sock_t *single_port_sock(ep_port_t *port)
{
    ep_sock_t *sock;

    if (port == NULL) return NULL;
    pthread_mutex_lock(&port->fd_table_lock);
    sock = port->sock_list_head;
    if (sock != NULL && sock->next != NULL) sock = NULL;
    pthread_mutex_unlock(&port->fd_table_lock);
    return sock;
}

/* Attach inert registrations to the live-socket list without allocating
 * Winsock handles.  They are deliberately absent from both worklists and the
 * fd table; this lets the queued-rearm regression distinguish O(work) from an
 * accidental O(all registrations) arm pass without consuming 50k sockets. */
static ep_sock_t *attach_idle_socks(ep_port_t *port, size_t count)
{
    ep_sock_t *socks = (ep_sock_t *)calloc(count, sizeof(*socks));

    if (socks == NULL) return NULL;
    for (size_t i = 0; i < count; i++) {
        socks[i].port = port;
        socks[i].prev = i == 0 ? NULL : &socks[i - 1];
        socks[i].next = i + 1 == count ? NULL : &socks[i + 1];
        atomic_init(&socks[i].state, EP_SOCK_REGISTERED);
        atomic_init(&socks[i].poll_status, EP_POLL_IDLE);
        atomic_init(&socks[i].delete_pending, 0);
        atomic_init(&socks[i].ready_queued, 0);
    }

    pthread_mutex_lock(&port->fd_table_lock);
    if (count > 0) {
        ep_sock_t *old_head = port->sock_list_head;
        socks[count - 1].next = old_head;
        if (old_head != NULL) old_head->prev = &socks[count - 1];
        port->sock_list_head = socks;
    }
    if (!ep_port_worklists_valid_locked(port)) {
        if (count > 0) {
            port->sock_list_head = socks[count - 1].next;
            if (port->sock_list_head != NULL) port->sock_list_head->prev = NULL;
        }
        pthread_mutex_unlock(&port->fd_table_lock);
        free(socks);
        ep_set_errno(EIO);
        return NULL;
    }
    pthread_mutex_unlock(&port->fd_table_lock);
    return socks;
}

static int detach_idle_socks(ep_port_t *port, ep_sock_t *socks, size_t count)
{
    int valid;

    if (socks == NULL) return 0;
    pthread_mutex_lock(&port->fd_table_lock);
    if (count == 0 || port->sock_list_head != socks) {
        pthread_mutex_unlock(&port->fd_table_lock);
        ep_set_errno(EIO);
        return -1;
    }
    port->sock_list_head = socks[count - 1].next;
    if (port->sock_list_head != NULL) port->sock_list_head->prev = NULL;
    valid = ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    free(socks);
    if (!valid) {
        ep_set_errno(EIO);
        return -1;
    }
    return 0;
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

static PPostQueuedCompletionStatusFn g_lease_native_post;
static HANDLE g_lease_aux_entered;
static HANDLE g_lease_aux_release;
static volatile LONG g_lease_block_aux;

static BOOL WINAPI lease_post_stub(HANDLE completion_port,
                                   DWORD bytes,
                                   ULONG_PTR key,
                                   LPOVERLAPPED overlapped)
{
    if (overlapped != NULL &&
        InterlockedCompareExchange(&g_lease_block_aux, 0, 0) != 0) {
        (void)SetEvent(g_lease_aux_entered);
        if (WaitForSingleObject(g_lease_aux_release, 5000) !=
            WAIT_OBJECT_0) {
            SetLastError(ERROR_TIMEOUT);
            return FALSE;
        }
    }
    return g_lease_native_post(completion_port, bytes, key, overlapped);
}

typedef struct lease_close_context {
    ep_port_t *port;
    HANDLE started;
    HANDLE done;
} lease_close_context_t;

static DWORD WINAPI lease_close_thread(void *parameter)
{
    lease_close_context_t *context = (lease_close_context_t *)parameter;

    (void)SetEvent(context->started);
    ep_port_begin_close(context->port);
    (void)SetEvent(context->done);
    return 0;
}

static int test_aux_post_close_lease(void)
{
    lease_close_context_t context;
    ep_port_t *port = NULL;
    HANDLE event_handle = NULL;
    HANDLE thread = NULL;
    epoll_data_t data;
    epoll_event_ex ignored;
    int result = -1;

    memset(&context, 0, sizeof(context));
    context.started = CreateEventW(NULL, TRUE, FALSE, NULL);
    context.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_lease_aux_entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_lease_aux_release = CreateEventW(NULL, TRUE, FALSE, NULL);
    event_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (context.started == NULL || context.done == NULL ||
        g_lease_aux_entered == NULL || g_lease_aux_release == NULL ||
        event_handle == NULL || ep_port_create(0, 0, &port) != 0) {
        goto cleanup;
    }

    memset(&data, 0, sizeof(data));
    if (ep_port_register(port, (SOCKET)(uintptr_t)event_handle,
                         EPOLLIN, 0, data, NULL) != 0) {
        goto cleanup;
    }
    memset(&ignored, 0, sizeof(ignored));
    if (ep_port_wait(port, &ignored, 1, 0, NULL) != 0) {
        goto cleanup;
    }

    g_lease_native_post = port->post_queued_completion_status;
    port->post_queued_completion_status = lease_post_stub;
    InterlockedExchange(&g_lease_block_aux, 1);
    if (!SetEvent(event_handle) ||
        WaitForSingleObject(g_lease_aux_entered, 2000) != WAIT_OBJECT_0) {
        goto cleanup;
    }

    context.port = port;
    thread = CreateThread(NULL, 0, lease_close_thread, &context, 0, NULL);
    if (thread == NULL ||
        WaitForSingleObject(context.started, 2000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(context.done, 100) != WAIT_TIMEOUT) {
        goto cleanup;
    }

    InterlockedExchange(&g_lease_block_aux, 0);
    if (!SetEvent(g_lease_aux_release) ||
        WaitForSingleObject(context.done, 5000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) {
        goto cleanup;
    }
    port->post_queued_completion_status = g_lease_native_post;
    if (ep_port_destroy(port) != 0) {
        port = NULL;
        goto cleanup;
    }
    port = NULL;
    result = 0;

cleanup:
    InterlockedExchange(&g_lease_block_aux, 0);
    if (g_lease_aux_release != NULL) (void)SetEvent(g_lease_aux_release);
    if (thread != NULL &&
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) {
        (void)TerminateThread(thread, 1);
        result = -1;
    }
    if (port != NULL) {
        if (g_lease_native_post != NULL) {
            port->post_queued_completion_status = g_lease_native_post;
        }
        if (ep_port_destroy(port) != 0) result = -1;
    }
    if (thread != NULL) CloseHandle(thread);
    if (event_handle != NULL) CloseHandle(event_handle);
    if (g_lease_aux_release != NULL) CloseHandle(g_lease_aux_release);
    if (g_lease_aux_entered != NULL) CloseHandle(g_lease_aux_entered);
    if (context.done != NULL) CloseHandle(context.done);
    if (context.started != NULL) CloseHandle(context.started);
    g_lease_native_post = NULL;
    g_lease_aux_release = NULL;
    g_lease_aux_entered = NULL;
    return result;
}

static int test_aux_posted_cancel(void)
{
    ep_port_t *port = NULL;
    ep_sock_t *sock = NULL;
    HANDLE event_handle = NULL;
    epoll_data_t data;
    epoll_event_ex ignored;
    SOCKET fd;
    ULONGLONG deadline;
    int state_ok;
    int result = -1;

    event_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (event_handle == NULL || ep_port_create(0, 0, &port) != 0) {
        goto cleanup;
    }
    fd = (SOCKET)(uintptr_t)event_handle;
    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0xc1c2c3c4c5c6c7c8);
    if (ep_port_register(port, fd, EPOLLIN, 0, data, NULL) != 0) {
        goto cleanup;
    }
    memset(&ignored, 0, sizeof(ignored));
    if (ep_port_wait(port, &ignored, 1, 0, NULL) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    sock = port->sock_list_head;
    state_ok = sock != NULL && sock->next == NULL &&
        sock->kind == EP_REG_WAITABLE && port->pending_poll_count == 1 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING;
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok || !SetEvent(event_handle)) {
        goto cleanup;
    }

    deadline = GetTickCount64() + 2000;
    while (atomic_load_explicit(&sock->completion_posted,
                                memory_order_acquire) == 0 &&
           GetTickCount64() < deadline) {
        Sleep(1);
    }
    if (atomic_load_explicit(&sock->completion_posted,
                             memory_order_acquire) == 0 ||
        ep_port_unregister(port, fd) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    state_ok = port->fd_table_count == 0 &&
        port->pending_poll_count == 1 && port->sock_list_head == sock &&
        sock->next == NULL &&
        atomic_load_explicit(&sock->delete_pending,
                             memory_order_relaxed) != 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_CANCELLED &&
        atomic_load_explicit(&sock->completion_posted,
                             memory_order_acquire) != 0;
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok || pump_iocp(port, 1000) < 1) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    state_ok = port->fd_table_count == 0 &&
        port->pending_poll_count == 0 && port->sock_list_head == NULL;
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (port != NULL && ep_port_destroy(port) != 0) {
        result = -1;
    }
    if (event_handle != NULL) CloseHandle(event_handle);
    return result;
}

static int test_waitable_zero_callback(void)
{
    ep_port_t *port = NULL;
    ep_sock_t *sock = NULL;
    HANDLE semaphore = NULL;
    epoll_data_t data;
    epoll_event_ex event;
    SOCKET fd;
    ULONGLONG deadline;
    LONG previous = -1;
    int state_ok;
    int result = -1;

    semaphore = CreateSemaphoreW(NULL, 0, 1, NULL);
    if (semaphore == NULL || ep_port_create(0, 0, &port) != 0) {
        goto cleanup;
    }
    fd = (SOCKET)(uintptr_t)semaphore;
    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0x1112131415161718);
    if (ep_port_register(port, fd, EPOLLIN, 0, data, NULL) != 0) {
        goto cleanup;
    }
    memset(&event, 0, sizeof(event));
    if (ep_port_wait(port, &event, 1, 0, NULL) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    sock = port->sock_list_head;
    state_ok = sock != NULL && sock->next == NULL &&
        sock->kind == EP_REG_WAITABLE &&
        sock->waitable_semantics == EP_WAITABLE_CONSUMPTIVE &&
        port->pending_poll_count == 1 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING &&
        atomic_load_explicit(&sock->waitable_notification_owned,
                             memory_order_acquire) == 0;
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok ||
        !ReleaseSemaphore(semaphore, 1, &previous) || previous != 0) {
        goto cleanup;
    }

    deadline = GetTickCount64() + 2000;
    while (atomic_load_explicit(&sock->completion_posted,
                                memory_order_acquire) == 0 &&
           GetTickCount64() < deadline) {
        Sleep(1);
    }
    if (atomic_load_explicit(&sock->completion_posted,
                             memory_order_acquire) == 0 ||
        WaitForSingleObject(semaphore, 0) != WAIT_TIMEOUT) {
        goto cleanup;
    }

    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0x2122232425262728);
    if (ep_port_modify(port, fd, 0, 0, data, NULL) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    state_ok = sock->user_events == 0 && sock->user_data.u64 == data.u64 &&
        !sock->needs_rearm && !sock->et_holdoff &&
        sock->wait_registration == NULL && port->pending_poll_count == 1 &&
        port->needs_rearm_count == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_CANCELLED &&
        atomic_load_explicit(&sock->state,
                             memory_order_relaxed) == EP_SOCK_REGISTERED &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->waitable_notification_owned,
                             memory_order_acquire) == 1 &&
        atomic_load_explicit(&sock->completion_posted,
                             memory_order_acquire) == 1 &&
        ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok || pump_iocp(port, 1000) < 1) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    state_ok = sock->user_events == 0 && !sock->needs_rearm &&
        !sock->et_holdoff && sock->wait_registration == NULL &&
        port->pending_poll_count == 0 && port->needs_rearm_count == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->state,
                             memory_order_relaxed) == EP_SOCK_REGISTERED &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->waitable_notification_owned,
                             memory_order_acquire) == 1 &&
        atomic_load_explicit(&sock->completion_posted,
                             memory_order_acquire) == 0 &&
        atomic_load_explicit(&port->ready_queue.queued,
                             memory_order_relaxed) == 0 &&
        ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok) {
        goto cleanup;
    }

    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0x3132333435363738);
    if (ep_port_modify(port, fd, EPOLLIN, 0, data, NULL) != 0) {
        goto cleanup;
    }
    memset(&event, 0, sizeof(event));
    if (ep_port_wait(port, &event, 1, 1000, NULL) != 1 ||
        event.events != EPOLLIN || event.data.u64 != data.u64) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    state_ok = atomic_load_explicit(&sock->waitable_notification_owned,
                                    memory_order_acquire) == 0 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok ||
        ep_port_wait(port, &event, 1, 30, NULL) != 0 ||
        WaitForSingleObject(semaphore, 0) != WAIT_TIMEOUT) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    if (semaphore != NULL) CloseHandle(semaphore);
    return result;
}

static int test_waitable_zero_ready(void)
{
    ep_port_t *port = NULL;
    ep_sock_t *sock = NULL;
    HANDLE semaphore = NULL;
    epoll_data_t data;
    epoll_event_ex event;
    SOCKET fd;
    ULONGLONG deadline;
    uint64_t stale_before;
    LONG previous = -1;
    int state_ok;
    int result = -1;

    semaphore = CreateSemaphoreW(NULL, 0, 1, NULL);
    if (semaphore == NULL || ep_port_create(0, 0, &port) != 0) {
        goto cleanup;
    }
    fd = (SOCKET)(uintptr_t)semaphore;
    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0x4142434445464748);
    if (ep_port_register(port, fd, EPOLLIN, 0, data, NULL) != 0) {
        goto cleanup;
    }
    memset(&event, 0, sizeof(event));
    if (ep_port_wait(port, &event, 1, 0, NULL) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    sock = port->sock_list_head;
    state_ok = sock != NULL && sock->next == NULL &&
        sock->kind == EP_REG_WAITABLE &&
        sock->waitable_semantics == EP_WAITABLE_CONSUMPTIVE &&
        port->pending_poll_count == 1 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING;
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok ||
        !ReleaseSemaphore(semaphore, 1, &previous) || previous != 0) {
        goto cleanup;
    }

    deadline = GetTickCount64() + 2000;
    while (atomic_load_explicit(&sock->completion_posted,
                                memory_order_acquire) == 0 &&
           GetTickCount64() < deadline) {
        Sleep(1);
    }
    if (atomic_load_explicit(&sock->completion_posted,
                             memory_order_acquire) == 0 ||
        WaitForSingleObject(semaphore, 0) != WAIT_TIMEOUT ||
        pump_iocp(port, 1000) < 1) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    state_ok = port->pending_poll_count == 0 &&
        sock->pending_events == EPOLLIN &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->state,
                             memory_order_relaxed) == EP_SOCK_READY &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 1 &&
        atomic_load_explicit(&sock->waitable_notification_owned,
                             memory_order_acquire) == 0 &&
        atomic_load_explicit(&port->ready_queue.queued,
                             memory_order_relaxed) == 1 &&
        ep_port_worklists_valid_locked(port);
    stale_before = port->stale_events_dropped;
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok) {
        goto cleanup;
    }

    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0x5152535455565758);
    if (ep_port_modify(port, fd, 0, 0, data, NULL) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    state_ok = sock->user_events == 0 && sock->user_data.u64 == data.u64 &&
        sock->pending_events == 0 && !sock->needs_rearm &&
        !sock->et_holdoff && port->pending_poll_count == 0 &&
        port->needs_rearm_count == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->state,
                             memory_order_relaxed) == EP_SOCK_REGISTERED &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->waitable_notification_owned,
                             memory_order_acquire) == 1 &&
        atomic_load_explicit(&port->ready_queue.queued,
                             memory_order_relaxed) == 1 &&
        ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok) {
        goto cleanup;
    }

    memset(&event, 0, sizeof(event));
    if (ep_port_wait(port, &event, 1, 0, NULL) != 0) {
        goto cleanup;
    }
    pthread_mutex_lock(&port->fd_table_lock);
    state_ok = port->stale_events_dropped == stale_before + 1 &&
        atomic_load_explicit(&port->ready_queue.queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->waitable_notification_owned,
                             memory_order_acquire) == 1 &&
        !sock->needs_rearm && ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok) {
        goto cleanup;
    }

    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0x6162636465666768);
    if (ep_port_modify(port, fd, EPOLLIN, 0, data, NULL) != 0) {
        goto cleanup;
    }
    if (ep_port_wait(port, &event, 1, 1000, NULL) != 1 ||
        event.events != EPOLLIN || event.data.u64 != data.u64) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    state_ok = atomic_load_explicit(&sock->waitable_notification_owned,
                                    memory_order_acquire) == 0 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok ||
        ep_port_wait(port, &event, 1, 30, NULL) != 0 ||
        WaitForSingleObject(semaphore, 0) != WAIT_TIMEOUT) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    if (semaphore != NULL) CloseHandle(semaphore);
    return result;
}

static int test_waitable_queued_rearm(void)
{
    ep_port_t *port = NULL;
    ep_sock_t *sock = NULL;
    HANDLE semaphore = NULL;
    epoll_data_t data;
    epoll_event_ex event;
    SOCKET fd;
    ULONGLONG deadline;
    uint64_t old_generation;
    uint64_t stale_before;
    LONG previous = -1;
    int context = 1;
    int state_ok;
    int result = -1;

    semaphore = CreateSemaphoreW(NULL, 0, 1, NULL);
    if (semaphore == NULL || ep_port_create(0, 0, &port) != 0) {
        goto cleanup;
    }
    fd = (SOCKET)(uintptr_t)semaphore;
    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0x7172737475767778);
    if (ep_port_register(port, fd, EPOLLIN | EPOLLONESHOT,
                         EPOLLONESHOT, data, &context) != 0) {
        goto cleanup;
    }
    memset(&event, 0, sizeof(event));
    if (ep_port_wait(port, &event, 1, 0, NULL) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    sock = port->sock_list_head;
    state_ok = sock != NULL && sock->next == NULL &&
        sock->kind == EP_REG_WAITABLE &&
        sock->waitable_semantics == EP_WAITABLE_CONSUMPTIVE &&
        port->pending_poll_count == 1 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING;
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok ||
        !ReleaseSemaphore(semaphore, 1, &previous) || previous != 0) {
        goto cleanup;
    }

    deadline = GetTickCount64() + 2000;
    while (atomic_load_explicit(&sock->completion_posted,
                                memory_order_acquire) == 0 &&
           GetTickCount64() < deadline) {
        Sleep(1);
    }
    if (atomic_load_explicit(&sock->completion_posted,
                             memory_order_acquire) == 0 ||
        WaitForSingleObject(semaphore, 0) != WAIT_TIMEOUT ||
        pump_iocp(port, 1000) < 1) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    state_ok = sock->oneshot_fired != 0 &&
        port->oneshot_fired_count == 1 && sock->pending_events == EPOLLIN &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->state,
                             memory_order_relaxed) == EP_SOCK_READY &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 1 &&
        atomic_load_explicit(&sock->waitable_notification_owned,
                             memory_order_acquire) == 0 &&
        atomic_load_explicit(&port->ready_queue.queued,
                             memory_order_relaxed) == 1 &&
        ep_port_worklists_valid_locked(port);
    old_generation = sock->generation;
    stale_before = port->stale_events_dropped;
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok || ep_port_rearm(port, fd) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    state_ok = sock->generation != old_generation &&
        sock->oneshot_fired == 0 && port->oneshot_fired_count == 0 &&
        sock->pending_events == 0 && sock->needs_rearm &&
        port->needs_rearm_count == 1 && port->rearm_head == sock &&
        port->rearm_tail == sock &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->waitable_notification_owned,
                             memory_order_acquire) == 1 &&
        atomic_load_explicit(&port->ready_queue.queued,
                             memory_order_relaxed) == 1 &&
        ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok) {
        goto cleanup;
    }

    memset(&event, 0, sizeof(event));
    if (ep_port_wait(port, &event, 1, 1000, NULL) != 1 ||
        event.events != EPOLLIN || event.data.u64 != data.u64 ||
        event.user_ctx != &context ||
        (event.flags & WEPOLL_FLAG_ONESHOT_FIRED) == 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    state_ok = port->stale_events_dropped == stale_before + 1 &&
        sock->oneshot_fired != 0 && port->oneshot_fired_count == 1 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->waitable_notification_owned,
                             memory_order_acquire) == 0 &&
        atomic_load_explicit(&port->ready_queue.queued,
                             memory_order_relaxed) == 0 &&
        ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok ||
        ep_port_wait(port, &event, 1, 30, NULL) != 0 ||
        WaitForSingleObject(semaphore, 0) != WAIT_TIMEOUT) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    if (semaphore != NULL) CloseHandle(semaphore);
    return result;
}

static int register_events(state_fixture_t *fixture, uint32_t events,
                           uint32_t flags, uint64_t value, void *context)
{
    epoll_data_t data;

    memset(&data, 0, sizeof(data));
    data.u64 = value;
    return ep_port_register(fixture->port, fixture->server,
                            events, flags, data, context);
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

static ep_port_t *expansion_refresh_port;
static volatile LONG expansion_refresh_cancel_calls;
static volatile LONG expansion_refresh_submit_calls;
static volatile LONG expansion_refresh_invalid;
static ULONG expansion_refresh_masks[2];

static ep_port_t *tcp_current_port;
static ep_sock_t *tcp_current_expected_sock;
static volatile LONG tcp_current_submit_calls;
static volatile LONG tcp_current_submit_invalid;
static volatile LONG tcp_current_query_calls;
static volatile LONG tcp_current_query_mismatch;
static ULONG tcp_current_submit_mask;
static ULONG tcp_current_query_state;

#define TEST_FILE_PIPE_LOCAL_INFORMATION_CLASS 24U
#define TEST_STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)
#define TEST_TCP_STATE_ESTABLISHED 4UL
#define TEST_TCP_STATE_CLOSE_WAIT  7UL
#define TEST_TCP_STATE_CLOSING     8UL
#define TEST_TCP_STATE_LAST_ACK    9UL
#define TEST_TCP_STATE_TIME_WAIT  10UL

typedef enum pipe_query_injection {
    PIPE_QUERY_INJECT_UNKNOWN_FAILURE = 1,
    PIPE_QUERY_INJECT_INVALID_LENGTH = 2
} pipe_query_injection_t;

static PNtQueryInformationFileFn pipe_query_delegate;
static HANDLE pipe_query_target;
static volatile LONG pipe_query_calls;
static volatile LONG pipe_query_injected;
static volatile LONG pipe_name_serial;
static int pipe_query_injection_mode;

static NTSTATUS NTAPI pipe_query_transient_stub(
    HANDLE file_handle,
    PIO_STATUS_BLOCK io_status_block,
    PVOID file_information,
    ULONG file_information_length,
    ULONG file_information_class)
{
    LONG call;

    if (file_handle != pipe_query_target ||
        file_information_class != TEST_FILE_PIPE_LOCAL_INFORMATION_CLASS) {
        return pipe_query_delegate(file_handle, io_status_block,
                                   file_information, file_information_length,
                                   file_information_class);
    }

    call = InterlockedIncrement(&pipe_query_calls);
    if (call != 3) {
        return pipe_query_delegate(file_handle, io_status_block,
                                   file_information, file_information_length,
                                   file_information_class);
    }

    InterlockedIncrement(&pipe_query_injected);
    if (pipe_query_injection_mode == PIPE_QUERY_INJECT_UNKNOWN_FAILURE) {
        if (io_status_block != NULL) {
            io_status_block->Status = TEST_STATUS_UNSUCCESSFUL;
            io_status_block->Information = 0;
        }
        return TEST_STATUS_UNSUCCESSFUL;
    }

    {
        NTSTATUS status = pipe_query_delegate(
            file_handle, io_status_block, file_information,
            file_information_length, file_information_class);

        if (status == STATUS_SUCCESS && io_status_block != NULL) {
            io_status_block->Information = file_information_length > 0
                ? file_information_length - 1 : 0;
        }
        return status;
    }
}

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

static NTSTATUS NTAPI expansion_refresh_cancel_stub(
    HANDLE file_handle,
    PIO_STATUS_BLOCK io_request_to_cancel,
    PIO_STATUS_BLOCK io_status_block)
{
    (void)file_handle;
    (void)io_request_to_cancel;
    (void)io_status_block;
    InterlockedIncrement(&expansion_refresh_cancel_calls);
    /* Model the documented race: the old AFD completion has already won and
     * cannot be cancelled, but its IOCP packet has not been handled yet. */
    return STATUS_NOT_FOUND;
}

static NTSTATUS NTAPI expansion_refresh_submit_stub(
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
    AFD_POLL_INFO *info = (AFD_POLL_INFO *)output_buffer;
    LONG call = InterlockedIncrement(&expansion_refresh_submit_calls);

    (void)event;
    (void)apc_routine;
    if (expansion_refresh_port == NULL ||
        file_handle != expansion_refresh_port->afd ||
        apc_context != io_status_block || io_status_block == NULL ||
        io_control_code != IOCTL_AFD_POLL || input_buffer != output_buffer ||
        info == NULL || input_buffer_length < sizeof(*info) ||
        output_buffer_length < sizeof(*info) ||
        info->NumberOfHandles != 1 || call < 1 || call > 2) {
        InterlockedIncrement(&expansion_refresh_invalid);
        return STATUS_ACCESS_DENIED;
    }

    expansion_refresh_masks[call - 1] = info->Handles[0].Events;
    if (call == 1) {
        io_status_block->Status = STATUS_PENDING;
        io_status_block->Information = 0;
        return STATUS_PENDING;
    }

    info->NumberOfHandles = 1;
    info->Handles[0].Events = AFD_POLL_RECEIVE | AFD_POLL_SEND;
    info->Handles[0].Status = STATUS_SUCCESS;
    io_status_block->Status = STATUS_SUCCESS;
    io_status_block->Information = sizeof(*info);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI tcp_current_submit_stub(
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
    AFD_POLL_INFO *info = (AFD_POLL_INFO *)output_buffer;
    LONG call = InterlockedIncrement(&tcp_current_submit_calls);

    (void)event;
    (void)apc_routine;
    if (tcp_current_port == NULL || file_handle != tcp_current_port->afd ||
        apc_context != io_status_block || io_status_block == NULL ||
        io_control_code != IOCTL_AFD_POLL || input_buffer != output_buffer ||
        info == NULL || input_buffer_length < sizeof(*info) ||
        output_buffer_length < sizeof(*info) ||
        info->NumberOfHandles != 1 || call != 1) {
        InterlockedIncrement(&tcp_current_submit_invalid);
        return STATUS_ACCESS_DENIED;
    }

    tcp_current_submit_mask = info->Handles[0].Events;
    io_status_block->Status = STATUS_PENDING;
    io_status_block->Information = 0;
    return STATUS_PENDING;
}

static int tcp_current_state_query_stub(ep_sock_t *sock, ULONG *state_out)
{
    InterlockedIncrement(&tcp_current_query_calls);
    if (sock != tcp_current_expected_sock || state_out == NULL) {
        InterlockedIncrement(&tcp_current_query_mismatch);
        return 0;
    }
    *state_out = tcp_current_query_state;
    return 1;
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

typedef struct immediate_success_wait_context {
    ep_port_t *port;
    epoll_event_ex event;
    int wait_result;
    int wait_error;
} immediate_success_wait_context_t;

static ep_port_t *immediate_success_port;
static PGetQueuedCompletionStatusEx immediate_success_dequeue_delegate;
static HANDLE immediate_success_wait_entered;
static HANDLE immediate_success_packet_dequeued;
static PVOID volatile immediate_success_expected_overlapped;
static volatile LONG immediate_success_submit_calls;
static volatile LONG immediate_success_invalid;

static BOOL WINAPI immediate_success_dequeue_stub(
    HANDLE completion_port,
    OVERLAPPED_ENTRY *entries,
    ULONG count,
    PULONG removed,
    DWORD milliseconds,
    BOOL alertable)
{
    BOOL ok;
    DWORD error;
    PVOID expected;

    if (immediate_success_port != NULL &&
        completion_port == immediate_success_port->iocp) {
        (void)SetEvent(immediate_success_wait_entered);
    }
    ok = immediate_success_dequeue_delegate(
        completion_port, entries, count, removed, milliseconds, alertable);
    error = GetLastError();
    expected = InterlockedCompareExchangePointer(
        &immediate_success_expected_overlapped, NULL, NULL);
    if (ok && entries != NULL && removed != NULL &&
        immediate_success_port != NULL &&
        completion_port == immediate_success_port->iocp &&
        expected != NULL) {
        ULONG limit = *removed < count ? *removed : count;

        for (ULONG i = 0; i < limit; i++) {
            if (entries[i].lpOverlapped == (LPOVERLAPPED)expected) {
                (void)InterlockedCompareExchangePointer(
                    &immediate_success_expected_overlapped,
                    NULL, expected);
                (void)SetEvent(immediate_success_packet_dequeued);
                break;
            }
        }
    }
    SetLastError(error);
    return ok;
}

static NTSTATUS NTAPI immediate_success_submit_stub(
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
    AFD_POLL_INFO *info = (AFD_POLL_INFO *)output_buffer;

    InterlockedIncrement(&immediate_success_submit_calls);
    (void)event;
    (void)apc_routine;
    if (immediate_success_port == NULL ||
        file_handle != immediate_success_port->afd ||
        apc_context != io_status_block || io_status_block == NULL ||
        io_control_code != IOCTL_AFD_POLL || input_buffer != output_buffer ||
        info == NULL || input_buffer_length < sizeof(*info) ||
        output_buffer_length < sizeof(*info) ||
        info->NumberOfHandles != 1) {
        InterlockedIncrement(&immediate_success_invalid);
        return STATUS_ACCESS_DENIED;
    }

    info->NumberOfHandles = 1;
    info->Handles[0].Events = AFD_POLL_RECEIVE;
    info->Handles[0].Status = STATUS_SUCCESS;
    io_status_block->Status = STATUS_SUCCESS;
    io_status_block->Information = sizeof(*info);
    (void)InterlockedExchangePointer(
        &immediate_success_expected_overlapped, io_status_block);
    /* The AFD handle suppresses a native IOCP packet for synchronous success.
     * Production must publish exactly one replacement after this stub
     * returns and after its pending accounting is visible. */
    return STATUS_SUCCESS;
}

static DWORD WINAPI immediate_success_wait_thread(void *parameter)
{
    immediate_success_wait_context_t *context =
        (immediate_success_wait_context_t *)parameter;

    memset(&context->event, 0, sizeof(context->event));
    errno = 0;
    context->wait_result = ep_port_wait(
        context->port, &context->event, 1, 5000, NULL);
    context->wait_error = errno;
    return 0;
}

#define UDP_SUBMIT_CAPTURE_LIMIT 4

static volatile LONG udp_submit_calls;
static volatile LONG udp_submit_invalid;
static ULONG udp_submit_events[UDP_SUBMIT_CAPTURE_LIMIT];
static ULONG udp_submit_exclusive[UDP_SUBMIT_CAPTURE_LIMIT];
static HANDLE udp_submit_handles[UDP_SUBMIT_CAPTURE_LIMIT];
static volatile LONG udp_probe_calls;
static volatile LONG udp_probe_mismatch;
static ep_sock_t *udp_probe_expected_sock;
static ep_udp_read_probe_result_t udp_probe_result;
static NTSTATUS udp_submit_result;

static void udp_capture_reset(ep_udp_read_probe_result_t probe_result)
{
    InterlockedExchange(&udp_submit_calls, 0);
    InterlockedExchange(&udp_submit_invalid, 0);
    memset(udp_submit_events, 0, sizeof(udp_submit_events));
    memset(udp_submit_exclusive, 0, sizeof(udp_submit_exclusive));
    memset(udp_submit_handles, 0, sizeof(udp_submit_handles));
    InterlockedExchange(&udp_probe_calls, 0);
    InterlockedExchange(&udp_probe_mismatch, 0);
    udp_probe_expected_sock = NULL;
    udp_probe_result = probe_result;
    udp_submit_result = STATUS_PENDING;
}

static NTSTATUS NTAPI udp_submit_capture_stub(
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
    LONG call = InterlockedIncrement(&udp_submit_calls) - 1;

    (void)file_handle;
    (void)event;
    (void)apc_routine;
    (void)apc_context;
    (void)io_status_block;
    (void)output_buffer;
    if (io_control_code != IOCTL_AFD_POLL || info == NULL ||
        input_buffer_length < sizeof(*info) ||
        output_buffer_length < sizeof(*info) ||
        info->NumberOfHandles != 1) {
        InterlockedIncrement(&udp_submit_invalid);
        return STATUS_ACCESS_DENIED;
    }
    if (call >= 0 && call < UDP_SUBMIT_CAPTURE_LIMIT) {
        udp_submit_events[call] = info->Handles[0].Events;
        udp_submit_exclusive[call] = info->Exclusive;
        udp_submit_handles[call] = info->Handles[0].Handle;
    }
    return udp_submit_result;
}

static ep_udp_read_probe_result_t udp_probe_stub(
    ep_sock_t *sock, int *identity_error_out)
{
    InterlockedIncrement(&udp_probe_calls);
    if (sock != udp_probe_expected_sock) {
        InterlockedIncrement(&udp_probe_mismatch);
    }
    if (identity_error_out != NULL) {
        *identity_error_out = 0;
    }
    return udp_probe_result;
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
    if (ep_afd_poll_submit(&sock, AFD_POLL_RECEIVE, NULL) == 0 &&
        cached_base_capture_calls == 1 &&
        cached_base_capture == expected_base) {
        result = 0;
    }
    g_ntdll.NtDeviceIoControlFile = original_submit;
    ep_afd_poll_key_release(&sock);
    atomic_store(&sock.poll_status, EP_POLL_IDLE);
    free(sock.afd_info);
    return result;
}
#endif

typedef struct udp_state_case {
    state_fixture_t fixture;
    ep_sock_t *sock;
    PNtDeviceIoControlFile original_submit;
    ep_udp_probe_read_fn original_probe;
    int submit_replaced;
} udp_state_case_t;

static void udp_state_case_init(udp_state_case_t *test_case)
{
    memset(test_case, 0, sizeof(*test_case));
    fixture_reset(&test_case->fixture);
}

/* Capture stubs do not create a kernel request or completion packet.  Retire
 * their synthetic pending accounting before normal fixture destruction. */
static void udp_state_discard_synthetic_pending(udp_state_case_t *test_case)
{
    ep_port_t *port = test_case->fixture.port;
    ep_sock_t *sock = test_case->sock;
    uint32_t poll_status;

    if (port == NULL) return;
    if (sock == NULL) {
        sock = fixture_sock(&test_case->fixture);
        test_case->sock = sock;
    }
    if (sock == NULL) return;

    pthread_mutex_lock(&port->fd_table_lock);
    poll_status = atomic_load_explicit(&sock->poll_status,
                                       memory_order_relaxed);
    if (poll_status == EP_POLL_PENDING ||
        poll_status == EP_POLL_CANCELLED) {
        ep_afd_poll_key_release(sock);
        if (port->pending_poll_count > 0) {
            port->pending_poll_count--;
        }
        atomic_store_explicit(&sock->poll_status, EP_POLL_IDLE,
                              memory_order_relaxed);
        sock->io_status_block.Status = STATUS_CANCELLED;
    }
    pthread_mutex_unlock(&port->fd_table_lock);
}

static void udp_state_case_close(udp_state_case_t *test_case)
{
    udp_state_discard_synthetic_pending(test_case);
    if (test_case->submit_replaced) {
        g_ntdll.NtDeviceIoControlFile = test_case->original_submit;
        test_case->submit_replaced = 0;
    }
    if (test_case->fixture.port != NULL) {
        test_case->fixture.port->udp_probe_read = test_case->original_probe;
    }
    fixture_close(&test_case->fixture);
    test_case->sock = NULL;
}

static int udp_state_case_open(udp_state_case_t *test_case,
                               uint32_t events, uint32_t flags,
                               uint64_t value, void *context,
                               ep_udp_read_probe_result_t probe_result)
{
    if (fixture_open_udp(&test_case->fixture) != 0) return -1;

    test_case->original_submit = g_ntdll.NtDeviceIoControlFile;
    test_case->original_probe = test_case->fixture.port->udp_probe_read;
    udp_capture_reset(probe_result);
    g_ntdll.NtDeviceIoControlFile = udp_submit_capture_stub;
    test_case->fixture.port->udp_probe_read = udp_probe_stub;
    test_case->submit_replaced = 1;
    if (register_events(&test_case->fixture, events, flags,
                        value, context) != 0) {
        return -1;
    }
    test_case->sock = fixture_sock(&test_case->fixture);
    if (test_case->sock == NULL) {
        ep_set_errno(EIO);
        return -1;
    }
    udp_probe_expected_sock = test_case->sock;
    return 0;
}

static int udp_state_complete(udp_state_case_t *test_case,
                              ULONG afd_events, NTSTATUS afd_status,
                              NTSTATUS completion_status)
{
    ep_port_t *port = test_case->fixture.port;
    ep_sock_t *sock = test_case->sock;
    int pending;

    pthread_mutex_lock(&port->fd_table_lock);
    pending = atomic_load_explicit(&sock->poll_status,
                                   memory_order_relaxed) == EP_POLL_PENDING &&
        port->pending_poll_count > 0 && sock->afd_info != NULL;
    if (pending) {
        sock->afd_info->NumberOfHandles = 1;
        sock->afd_info->Handles[0].Events = afd_events;
        sock->afd_info->Handles[0].Status = afd_status;
        sock->io_status_block.Status = completion_status;
    }
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!pending) {
        ep_set_errno(EIO);
        return -1;
    }
    ep_sock_handle_completion(sock, 0, completion_status);
    return 0;
}

static int state_complete_socket_receive(ep_port_t *port, ep_sock_t *sock)
{
    int pending;

    pthread_mutex_lock(&port->fd_table_lock);
    pending = atomic_load_explicit(&sock->poll_status,
                                   memory_order_relaxed) == EP_POLL_PENDING &&
        port->pending_poll_count > 0 && sock->afd_info != NULL;
    if (pending) {
        sock->afd_info->NumberOfHandles = 1;
        sock->afd_info->Handles[0].Events = AFD_POLL_RECEIVE;
        sock->afd_info->Handles[0].Status = STATUS_SUCCESS;
        sock->io_status_block.Status = STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!pending) {
        ep_set_errno(EIO);
        return -1;
    }
    ep_sock_handle_completion(sock, 0, STATUS_SUCCESS);
    return 0;
}

static void state_discard_synthetic_pending(ep_port_t *port)
{
    ep_sock_t *sock = single_port_sock(port);
    uint32_t poll_status;

    if (sock == NULL) return;
    pthread_mutex_lock(&port->fd_table_lock);
    poll_status = atomic_load_explicit(&sock->poll_status,
                                       memory_order_relaxed);
    if (poll_status == EP_POLL_PENDING ||
        poll_status == EP_POLL_CANCELLED) {
        ep_afd_poll_key_release(sock);
        if (port->pending_poll_count > 0) {
            port->pending_poll_count--;
        }
        atomic_store_explicit(&sock->poll_status, EP_POLL_IDLE,
                              memory_order_relaxed);
        sock->io_status_block.Status = STATUS_CANCELLED;
    }
    pthread_mutex_unlock(&port->fd_table_lock);
}

/* AFD's native Exclusive bit cancels peer polls for the same provider base,
 * including ordinary registrations.  Linux mixed EPOLLEXCLUSIVE semantics
 * require every ordinary watcher to remain eligible while at least one
 * exclusive watcher may win.  Keep the native submission non-exclusive and
 * exercise the process-wide claim filter with synthetic identical receives. */
static int test_exclusive_mixed_submit(void)
{
    state_fixture_t fixture;
    ep_port_t *exclusive_ports[2] = {NULL, NULL};
    ep_sock_t *socks[3] = {NULL, NULL, NULL};
    epoll_data_t data;
    PNtDeviceIoControlFile original_submit = NULL;
    int contexts[3] = {0, 0, 0};
    int submit_replaced = 0;
    int result = -1;
    int i;

    fixture_reset(&fixture);
    if (fixture_open(&fixture) != 0) return -1;

    original_submit = g_ntdll.NtDeviceIoControlFile;
    udp_capture_reset(EP_UDP_READ_PROBE_NOT_READY);
    g_ntdll.NtDeviceIoControlFile = udp_submit_capture_stub;
    submit_replaced = 1;

    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0x6d697865645f6f72); /* "mixed_or" */
    if (ep_port_register(fixture.port, fixture.server, EPOLLIN, 0,
                         data, &contexts[0]) != 0)
        goto cleanup;

    for (i = 0; i < 2; i++) {
        data.u64 = UINT64_C(0x6d697865645f6578) + (uint64_t)i;
        if (ep_port_create(0, 0, &exclusive_ports[i]) != 0 ||
            ep_port_register(exclusive_ports[i], fixture.server,
                             EPOLLIN | EPOLLEXCLUSIVE | EPOLLET,
                             EPOLLEXCLUSIVE | EPOLLET, data,
                             &contexts[i + 1]) != 0)
            goto cleanup;
    }

    /* The capture seam sees all public socket requests, including the two
     * EPOLLEXCLUSIVE registrations.  None may ask AFD to cancel the ordinary
     * pending request. */
    if (InterlockedCompareExchange(&udp_submit_calls, 0, 0) != 3 ||
        InterlockedCompareExchange(&udp_submit_invalid, 0, 0) != 0 ||
        udp_submit_exclusive[0] != FALSE ||
        udp_submit_exclusive[1] != FALSE ||
        udp_submit_exclusive[2] != FALSE)
        goto cleanup;

    socks[0] = fixture_sock(&fixture);
    socks[1] = single_port_sock(exclusive_ports[0]);
    socks[2] = single_port_sock(exclusive_ports[1]);
    if (socks[0] == NULL || socks[1] == NULL || socks[2] == NULL ||
        socks[0]->base_socket != socks[1]->base_socket ||
        socks[0]->base_socket != socks[2]->base_socket ||
        udp_submit_handles[0] !=
            (HANDLE)(uintptr_t)socks[0]->base_socket ||
        udp_submit_handles[1] == udp_submit_handles[0] ||
        udp_submit_handles[2] == udp_submit_handles[0] ||
        udp_submit_handles[2] == udp_submit_handles[1] ||
        socks[0]->afd_poll_key_reservation != NULL ||
        (socks[1]->afd_poll_key_reservation != NULL &&
         socks[1]->afd_poll_key_reservation != udp_submit_handles[1]) ||
        (socks[2]->afd_poll_key_reservation != NULL &&
         socks[2]->afd_poll_key_reservation != udp_submit_handles[2]) ||
        (socks[1]->user_flags & EPOLLEXCLUSIVE) == 0 ||
        (socks[2]->user_flags & EPOLLEXCLUSIVE) == 0)
        goto cleanup;

    if (send_byte(fixture.client) != 0 ||
        state_complete_socket_receive(fixture.port, socks[0]) != 0 ||
        state_complete_socket_receive(exclusive_ports[0], socks[1]) != 0 ||
        state_complete_socket_receive(exclusive_ports[1], socks[2]) != 0)
        goto cleanup;

    if (atomic_load_explicit(&fixture.port->ready_queue.queued,
                             memory_order_relaxed) != 1 ||
        atomic_load_explicit(&exclusive_ports[0]->ready_queue.queued,
                             memory_order_relaxed) != 1 ||
        atomic_load_explicit(&exclusive_ports[1]->ready_queue.queued,
                             memory_order_relaxed) != 0 ||
        socks[1]->exclusive_claim_classes == 0 ||
        socks[2]->exclusive_claim_classes != 0 ||
        socks[0]->afd_poll_key_owned != 0 ||
        socks[1]->afd_poll_key_owned != 0 ||
        socks[2]->afd_poll_key_owned != 0 ||
        socks[0]->afd_poll_key_reservation != NULL ||
        socks[1]->afd_poll_key_reservation != NULL ||
        socks[2]->afd_poll_key_reservation != NULL ||
        socks[1]->needs_rearm != 0 || socks[2]->needs_rearm == 0 ||
        fixture.port->pending_poll_count != 0 ||
        exclusive_ports[0]->pending_poll_count != 0 ||
        exclusive_ports[1]->pending_poll_count != 0)
        goto cleanup;

    result = 0;

cleanup:
    if (submit_replaced)
        g_ntdll.NtDeviceIoControlFile = original_submit;
    for (i = 0; i < 2; i++) {
        if (exclusive_ports[i] != NULL) {
            state_discard_synthetic_pending(exclusive_ports[i]);
            if (ep_port_destroy(exclusive_ports[i]) != 0) result = -1;
            exclusive_ports[i] = NULL;
        }
    }
    state_discard_synthetic_pending(fixture.port);
    fixture_close(&fixture);
    return result;
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

static int test_udp_readless_error_lt(void)
{
    static const uint64_t value = UINT64_C(0x7564706572723031);
    const uint32_t terminal_events = ep_epoll_to_afd_events(0);
    const uint32_t hidden_events = terminal_events | AFD_POLL_RECEIVE;
    udp_state_case_t test_case;
    epoll_event_ex event;
    ep_sock_t *sock;
    int context;
    int state_ok;
    int result = -1;

    udp_state_case_init(&test_case);
    if (udp_state_case_open(&test_case, 0, 0, value, &context,
                            EP_UDP_READ_PROBE_ERROR) != 0) {
        goto cleanup;
    }
    sock = test_case.sock;

    pthread_mutex_lock(&test_case.fixture.port->fd_table_lock);
    state_ok = sock->socket_protocol == EP_SOCKET_PROTOCOL_UDP &&
        sock->udp_afd_qualifier_eligible != 0 &&
        sock->async_read_capability == EP_SOCKET_ASYNC_READ_SAFE &&
        sock->udp_readless_receive_parked == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING &&
        sock->submitted_afd_events == hidden_events &&
        test_case.fixture.port->pending_poll_count == 1 &&
        sock->needs_rearm == 0 &&
        InterlockedCompareExchange(&udp_submit_calls, 0, 0) == 1 &&
        InterlockedCompareExchange(&udp_submit_invalid, 0, 0) == 0 &&
        udp_submit_events[0] == hidden_events &&
        udp_submit_exclusive[0] == FALSE &&
        ep_port_worklists_valid_locked(test_case.fixture.port);
    pthread_mutex_unlock(&test_case.fixture.port->fd_table_lock);
    if (!state_ok ||
        udp_state_complete(&test_case, AFD_POLL_RECEIVE,
                           STATUS_SUCCESS, STATUS_SUCCESS) != 0) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    pthread_mutex_lock(&test_case.fixture.port->fd_table_lock);
    state_ok = sock->udp_readless_receive_parked == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        test_case.fixture.port->pending_poll_count == 0 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) != 0 &&
        sock->pending_events == EPOLLERR &&
        InterlockedCompareExchange(&udp_probe_calls, 0, 0) == 1 &&
        InterlockedCompareExchange(&udp_probe_mismatch, 0, 0) == 0 &&
        InterlockedCompareExchange(&udp_submit_calls, 0, 0) == 1 &&
        ep_port_worklists_valid_locked(test_case.fixture.port);
    pthread_mutex_unlock(&test_case.fixture.port->fd_table_lock);
    if (!state_ok) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    memset(&event, 0, sizeof(event));
    if (ep_port_wait(test_case.fixture.port, &event, 1, 0, NULL) != 1 ||
        event.events != EPOLLERR || event.data.u64 != value ||
        event.user_ctx != &context || event.flags != 0) {
        ep_set_errno(EIO);
        goto cleanup;
    }
    pthread_mutex_lock(&test_case.fixture.port->fd_table_lock);
    state_ok = sock->udp_readless_receive_parked == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        sock->pending_events == 0 && sock->needs_rearm != 0 &&
        test_case.fixture.port->needs_rearm_count == 1 &&
        ep_port_worklists_valid_locked(test_case.fixture.port);
    pthread_mutex_unlock(&test_case.fixture.port->fd_table_lock);
    if (!state_ok) {
        ep_set_errno(EIO);
        goto cleanup;
    }
    result = 0;

cleanup:
    udp_state_case_close(&test_case);
    return result;
}

static int run_udp_readless_error_trigger_case(uint32_t flags)
{
    const uint64_t value = flags == EPOLLET
        ? UINT64_C(0x7564706572726574)
        : UINT64_C(0x7564706572726f73);
    const uint32_t terminal_events = ep_epoll_to_afd_events(flags);
    const uint32_t hidden_events = terminal_events | AFD_POLL_RECEIVE;
    const uint32_t expected_flags = flags == EPOLLET
        ? WEPOLL_FLAG_ET_DELIVERED | WEPOLL_FLAG_EDGE_ARMED
        : WEPOLL_FLAG_ONESHOT_FIRED;
    udp_state_case_t test_case;
    epoll_event_ex event;
    ep_sock_t *sock;
    int context;
    int state_ok;
    int result = -1;

    udp_state_case_init(&test_case);
    if (udp_state_case_open(&test_case, flags, flags, value, &context,
                            EP_UDP_READ_PROBE_ERROR) != 0) {
        goto cleanup;
    }
    sock = test_case.sock;
    if (udp_state_complete(&test_case, AFD_POLL_RECEIVE,
                           STATUS_SUCCESS, STATUS_SUCCESS) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&test_case.fixture.port->fd_table_lock);
    state_ok = sock->udp_readless_receive_parked == 0 &&
        sock->submitted_afd_events == hidden_events &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        test_case.fixture.port->pending_poll_count == 0 &&
        sock->pending_events == EPOLLERR &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) != 0 &&
        ((flags == EPOLLET && sock->observed_events == EPOLLERR &&
          sock->oneshot_fired == 0) ||
         (flags == EPOLLONESHOT && sock->observed_events == 0 &&
          sock->oneshot_fired != 0)) &&
        InterlockedCompareExchange(&udp_probe_calls, 0, 0) == 1 &&
        InterlockedCompareExchange(&udp_submit_calls, 0, 0) == 1 &&
        ep_port_worklists_valid_locked(test_case.fixture.port);
    pthread_mutex_unlock(&test_case.fixture.port->fd_table_lock);
    if (!state_ok) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    memset(&event, 0, sizeof(event));
    if (ep_port_wait(test_case.fixture.port, &event, 1, 0, NULL) != 1 ||
        event.events != EPOLLERR || event.data.u64 != value ||
        event.user_ctx != &context || event.flags != expected_flags) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    if (flags == EPOLLET) {
        udp_submit_result = STATUS_SUCCESS;
        memset(&event, 0, sizeof(event));
        if (ep_port_wait(test_case.fixture.port, &event, 1, 0, NULL) != 0 ||
            InterlockedCompareExchange(&udp_submit_calls, 0, 0) != 2) {
            ep_set_errno(EIO);
            goto cleanup;
        }
        pthread_mutex_lock(&test_case.fixture.port->fd_table_lock);
        state_ok = sock->udp_readless_receive_parked == 0 &&
            sock->observed_events == 0 && sock->et_holdoff == 0 &&
            sock->needs_rearm == 0 &&
            test_case.fixture.port->needs_rearm_count == 0 &&
            sock->pending_events == 0 &&
            atomic_load_explicit(&sock->ready_queued,
                                 memory_order_relaxed) == 0 &&
            atomic_load_explicit(&sock->poll_status,
                                 memory_order_relaxed) == EP_POLL_IDLE &&
            InterlockedCompareExchange(&udp_probe_calls, 0, 0) == 1 &&
            ep_port_worklists_valid_locked(test_case.fixture.port);
        pthread_mutex_unlock(&test_case.fixture.port->fd_table_lock);
        if (!state_ok) {
            ep_set_errno(EIO);
            goto cleanup;
        }
    } else {
        memset(&event, 0, sizeof(event));
        if (ep_port_wait(test_case.fixture.port, &event, 1, 0, NULL) != 0 ||
            InterlockedCompareExchange(&udp_submit_calls, 0, 0) != 1 ||
            ep_port_rearm(test_case.fixture.port,
                          test_case.fixture.server) != 0 ||
            ep_port_wait(test_case.fixture.port, &event, 1, 0, NULL) != 0 ||
            InterlockedCompareExchange(&udp_submit_calls, 0, 0) != 2 ||
            udp_state_complete(&test_case, AFD_POLL_RECEIVE,
                               STATUS_SUCCESS, STATUS_SUCCESS) != 0) {
            ep_set_errno(EIO);
            goto cleanup;
        }
        memset(&event, 0, sizeof(event));
        if (ep_port_wait(test_case.fixture.port, &event, 1, 0, NULL) != 1 ||
            event.events != EPOLLERR || event.data.u64 != value ||
            event.user_ctx != &context ||
            event.flags != WEPOLL_FLAG_ONESHOT_FIRED ||
            InterlockedCompareExchange(&udp_probe_calls, 0, 0) != 2) {
            ep_set_errno(EIO);
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    udp_state_case_close(&test_case);
    return result;
}

static int test_udp_readless_error(void)
{
    if (test_udp_readless_error_lt() != 0 ||
        run_udp_readless_error_trigger_case(EPOLLET) != 0) {
        return -1;
    }
    return run_udp_readless_error_trigger_case(EPOLLONESHOT);
}

static int run_udp_readless_park_case(
    ep_udp_read_probe_result_t probe_result)
{
    static const uint64_t value = UINT64_C(0x7564707061726b31);
    const uint32_t user_events = EPOLLERR | EPOLLEXCLUSIVE;
    const uint32_t terminal_events =
        ep_epoll_to_afd_events(user_events);
    const uint32_t hidden_events = terminal_events | AFD_POLL_RECEIVE;
    udp_state_case_t test_case;
    ep_sock_t *sock;
    int context;
    int state_ok;
    int result = -1;

    udp_state_case_init(&test_case);
    if (udp_state_case_open(&test_case, user_events, EPOLLEXCLUSIVE,
                            value, &context, probe_result) != 0) {
        goto cleanup;
    }
    sock = test_case.sock;

    pthread_mutex_lock(&test_case.fixture.port->fd_table_lock);
    state_ok = sock->socket_protocol == EP_SOCKET_PROTOCOL_UDP &&
        sock->udp_afd_qualifier_eligible != 0 &&
        sock->async_read_capability == EP_SOCKET_ASYNC_READ_SAFE &&
        sock->udp_readless_receive_parked == 0 &&
        sock->submitted_afd_events == hidden_events &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING &&
        test_case.fixture.port->pending_poll_count == 1 &&
        InterlockedCompareExchange(&udp_submit_calls, 0, 0) == 1 &&
        InterlockedCompareExchange(&udp_submit_invalid, 0, 0) == 0 &&
        udp_submit_events[0] == hidden_events &&
        udp_submit_exclusive[0] == FALSE &&
        ep_port_worklists_valid_locked(test_case.fixture.port);
    pthread_mutex_unlock(&test_case.fixture.port->fd_table_lock);
    if (!state_ok ||
        udp_state_complete(&test_case, AFD_POLL_RECEIVE,
                           STATUS_SUCCESS, STATUS_SUCCESS) != 0) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    pthread_mutex_lock(&test_case.fixture.port->fd_table_lock);
    state_ok = sock->udp_readless_receive_parked != 0 &&
        sock->submitted_afd_events == terminal_events &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING &&
        test_case.fixture.port->pending_poll_count == 1 &&
        sock->needs_rearm == 0 && sock->pending_events == 0 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        InterlockedCompareExchange(&udp_probe_calls, 0, 0) == 1 &&
        InterlockedCompareExchange(&udp_probe_mismatch, 0, 0) == 0 &&
        InterlockedCompareExchange(&udp_submit_calls, 0, 0) == 2 &&
        InterlockedCompareExchange(&udp_submit_invalid, 0, 0) == 0 &&
        udp_submit_events[1] == terminal_events &&
        udp_submit_exclusive[1] == FALSE &&
        ep_port_worklists_valid_locked(test_case.fixture.port);
    pthread_mutex_unlock(&test_case.fixture.port->fd_table_lock);
    if (!state_ok) {
        ep_set_errno(EIO);
        goto cleanup;
    }
    result = 0;

cleanup:
    udp_state_case_close(&test_case);
    return result;
}

static int test_udp_readless_park_submit_failure(void)
{
    static const uint64_t value = UINT64_C(0x7564707061726b66);
    const uint32_t user_events = EPOLLERR;
    const uint32_t terminal_events = ep_epoll_to_afd_events(user_events);
    const uint32_t hidden_events = terminal_events | AFD_POLL_RECEIVE;
    udp_state_case_t test_case;
    epoll_event_ex event;
    ep_sock_t *sock;
    int context;
    int state_ok;
    int result = -1;

    udp_state_case_init(&test_case);
    if (udp_state_case_open(&test_case, user_events, 0,
                            value, &context,
                            EP_UDP_READ_PROBE_READY) != 0) {
        goto cleanup;
    }
    sock = test_case.sock;
    udp_submit_result = STATUS_ACCESS_DENIED;
    if (udp_state_complete(&test_case, AFD_POLL_RECEIVE,
                           STATUS_SUCCESS, STATUS_SUCCESS) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&test_case.fixture.port->fd_table_lock);
    state_ok = sock->udp_readless_receive_parked != 0 &&
        sock->submitted_afd_events == hidden_events &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        test_case.fixture.port->pending_poll_count == 0 &&
        sock->needs_rearm != 0 &&
        test_case.fixture.port->needs_rearm_count == 1 &&
        sock->pending_events == 0 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        test_case.fixture.port->asynchronous_errors == 1 &&
        test_case.fixture.port->async_error == EACCES &&
        InterlockedCompareExchange(&udp_probe_calls, 0, 0) == 1 &&
        InterlockedCompareExchange(&udp_submit_calls, 0, 0) == 2 &&
        ep_port_worklists_valid_locked(test_case.fixture.port);
    pthread_mutex_unlock(&test_case.fixture.port->fd_table_lock);
    if (!state_ok) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    errno = 0;
    memset(&event, 0, sizeof(event));
    if (ep_port_wait(test_case.fixture.port, &event, 1, 0, NULL) != -1 ||
        errno != EACCES) {
        goto cleanup;
    }
    udp_submit_result = STATUS_PENDING;
    if (ep_port_wait(test_case.fixture.port, &event, 1, 0, NULL) != 0) {
        goto cleanup;
    }
    pthread_mutex_lock(&test_case.fixture.port->fd_table_lock);
    state_ok = sock->udp_readless_receive_parked != 0 &&
        sock->submitted_afd_events == terminal_events &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING &&
        test_case.fixture.port->pending_poll_count == 1 &&
        sock->needs_rearm == 0 &&
        InterlockedCompareExchange(&udp_submit_calls, 0, 0) == 3 &&
        udp_submit_events[2] == terminal_events &&
        ep_port_worklists_valid_locked(test_case.fixture.port);
    pthread_mutex_unlock(&test_case.fixture.port->fd_table_lock);
    if (!state_ok) {
        ep_set_errno(EIO);
        goto cleanup;
    }
    result = 0;

cleanup:
    udp_state_case_close(&test_case);
    return result;
}

static int test_udp_readless_park(void)
{
    if (run_udp_readless_park_case(EP_UDP_READ_PROBE_READY) != 0) {
        return -1;
    }
    if (run_udp_readless_park_case(EP_UDP_READ_PROBE_UNAVAILABLE) != 0) {
        return -1;
    }
    return test_udp_readless_park_submit_failure();
}

static int test_udp_readless_mod_rollback(void)
{
    static const uint64_t old_value = UINT64_C(0x7564706d6f643031);
    static const uint64_t new_value = UINT64_C(0x7564706d6f643032);
    const uint32_t user_events = EPOLLERR;
    const uint32_t terminal_events =
        ep_epoll_to_afd_events(user_events);
    udp_state_case_t test_case;
    epoll_event_ex event;
    ep_sock_t *sock;
    uint64_t old_generation;
    int old_context;
    int new_context;
    int modify_error;
    int modify_result;
    int state_ok;
    int result = -1;

    udp_state_case_init(&test_case);
    if (udp_state_case_open(&test_case, user_events, 0,
                            old_value, &old_context,
                            EP_UDP_READ_PROBE_READY) != 0) {
        goto cleanup;
    }
    sock = test_case.sock;
    if (udp_state_complete(&test_case, AFD_POLL_RECEIVE,
                           STATUS_SUCCESS, STATUS_SUCCESS) != 0 ||
        udp_state_complete(&test_case, AFD_POLL_ABORT,
                           STATUS_SUCCESS, STATUS_SUCCESS) != 0) {
        goto cleanup;
    }
    memset(&event, 0, sizeof(event));
    if (ep_port_wait(test_case.fixture.port, &event, 1, 0, NULL) != 1 ||
        event.events != EPOLLERR || event.data.u64 != old_value ||
        event.user_ctx != &old_context || event.flags != 0) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    pthread_mutex_lock(&test_case.fixture.port->fd_table_lock);
    old_generation = sock->generation;
    state_ok = sock->udp_readless_receive_parked != 0 &&
        sock->submitted_afd_events == terminal_events &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        sock->needs_rearm != 0 &&
        test_case.fixture.port->needs_rearm_count == 1 &&
        test_case.fixture.port->rearm_head == sock &&
        test_case.fixture.port->rearm_tail == sock &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        InterlockedCompareExchange(&udp_probe_calls, 0, 0) == 1 &&
        InterlockedCompareExchange(&udp_submit_calls, 0, 0) == 2 &&
        ep_port_worklists_valid_locked(test_case.fixture.port);
    pthread_mutex_unlock(&test_case.fixture.port->fd_table_lock);
    if (!state_ok) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    counted_submit_delegate = submit_failure_stub;
    InterlockedExchange(&counted_submit_calls, 0);
    g_ntdll.NtDeviceIoControlFile = counted_submit_stub;
    atomic_store_explicit(&test_case.fixture.port->waiter_active, 1,
                          memory_order_release);
    errno = 0;
    modify_result = modify_events(&test_case.fixture, user_events, 0,
                                  new_value, &new_context);
    modify_error = errno;
    atomic_store_explicit(&test_case.fixture.port->waiter_active, 0,
                          memory_order_release);
    g_ntdll.NtDeviceIoControlFile = udp_submit_capture_stub;

    pthread_mutex_lock(&test_case.fixture.port->fd_table_lock);
    state_ok = modify_result == -1 && modify_error == EACCES &&
        InterlockedCompareExchange(&counted_submit_calls, 0, 0) == 1 &&
        sock->udp_readless_receive_parked != 0 &&
        sock->generation == old_generation &&
        sock->user_events == user_events && sock->user_flags == 0 &&
        sock->user_data.u64 == old_value &&
        sock->user_ctx == &old_context &&
        sock->submitted_afd_events == terminal_events &&
        sock->pending_events == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        sock->needs_rearm != 0 &&
        test_case.fixture.port->needs_rearm_count == 1 &&
        test_case.fixture.port->rearm_head == sock &&
        test_case.fixture.port->rearm_tail == sock &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        ep_port_worklists_valid_locked(test_case.fixture.port);
    pthread_mutex_unlock(&test_case.fixture.port->fd_table_lock);
    if (!state_ok) {
        ep_set_errno(EIO);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (test_case.fixture.port != NULL) {
        atomic_store_explicit(&test_case.fixture.port->waiter_active, 0,
                              memory_order_release);
    }
    udp_state_case_close(&test_case);
    return result;
}

static int test_udp_readless_rearm_rollback(void)
{
    static const uint64_t value = UINT64_C(0x756470726561726d);
    const uint32_t user_events = EPOLLERR | EPOLLONESHOT;
    const uint32_t terminal_events =
        ep_epoll_to_afd_events(user_events);
    udp_state_case_t test_case;
    epoll_event_ex event;
    ep_sock_t *sock;
    uint64_t old_generation;
    int context;
    int rearm_error;
    int rearm_result;
    int state_ok;
    int result = -1;

    udp_state_case_init(&test_case);
    if (udp_state_case_open(&test_case, user_events, EPOLLONESHOT,
                            value, &context,
                            EP_UDP_READ_PROBE_READY) != 0) {
        goto cleanup;
    }
    sock = test_case.sock;
    if (udp_state_complete(&test_case, AFD_POLL_RECEIVE,
                           STATUS_SUCCESS, STATUS_SUCCESS) != 0 ||
        udp_state_complete(&test_case, AFD_POLL_ABORT,
                           STATUS_SUCCESS, STATUS_SUCCESS) != 0) {
        goto cleanup;
    }

    pthread_mutex_lock(&test_case.fixture.port->fd_table_lock);
    old_generation = sock->generation;
    state_ok = sock->udp_readless_receive_parked != 0 &&
        sock->submitted_afd_events == terminal_events &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        sock->pending_events == EPOLLERR && sock->oneshot_fired != 0 &&
        test_case.fixture.port->oneshot_fired_count == 1 &&
        test_case.fixture.port->oneshot_head == sock &&
        test_case.fixture.port->oneshot_tail == sock &&
        sock->needs_rearm == 0 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) != 0 &&
        InterlockedCompareExchange(&udp_probe_calls, 0, 0) == 1 &&
        InterlockedCompareExchange(&udp_submit_calls, 0, 0) == 2 &&
        ep_port_worklists_valid_locked(test_case.fixture.port);
    pthread_mutex_unlock(&test_case.fixture.port->fd_table_lock);
    if (!state_ok) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    counted_submit_delegate = submit_failure_stub;
    InterlockedExchange(&counted_submit_calls, 0);
    g_ntdll.NtDeviceIoControlFile = counted_submit_stub;
    atomic_store_explicit(&test_case.fixture.port->waiter_active, 1,
                          memory_order_release);
    errno = 0;
    rearm_result = ep_port_rearm(test_case.fixture.port,
                                 test_case.fixture.server);
    rearm_error = errno;
    atomic_store_explicit(&test_case.fixture.port->waiter_active, 0,
                          memory_order_release);
    g_ntdll.NtDeviceIoControlFile = udp_submit_capture_stub;

    pthread_mutex_lock(&test_case.fixture.port->fd_table_lock);
    state_ok = rearm_result == -1 && rearm_error == EACCES &&
        InterlockedCompareExchange(&counted_submit_calls, 0, 0) == 1 &&
        sock->udp_readless_receive_parked != 0 &&
        sock->generation == old_generation &&
        sock->user_events == user_events &&
        sock->user_flags == EPOLLONESHOT &&
        sock->user_data.u64 == value && sock->user_ctx == &context &&
        sock->submitted_afd_events == terminal_events &&
        sock->pending_events == EPOLLERR && sock->oneshot_fired != 0 &&
        test_case.fixture.port->oneshot_fired_count == 1 &&
        test_case.fixture.port->oneshot_head == sock &&
        test_case.fixture.port->oneshot_tail == sock &&
        sock->needs_rearm == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) != 0 &&
        ep_port_worklists_valid_locked(test_case.fixture.port);
    pthread_mutex_unlock(&test_case.fixture.port->fd_table_lock);
    if (!state_ok) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    memset(&event, 0, sizeof(event));
    if (ep_port_wait(test_case.fixture.port, &event, 1, 0, NULL) != 1 ||
        event.events != EPOLLERR || event.data.u64 != value ||
        event.user_ctx != &context ||
        event.flags != WEPOLL_FLAG_ONESHOT_FIRED) {
        ep_set_errno(EIO);
        goto cleanup;
    }
    pthread_mutex_lock(&test_case.fixture.port->fd_table_lock);
    state_ok = sock->udp_readless_receive_parked != 0 &&
        sock->oneshot_fired != 0 &&
        test_case.fixture.port->oneshot_fired_count == 1 &&
        sock->needs_rearm == 0 &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        ep_port_worklists_valid_locked(test_case.fixture.port);
    pthread_mutex_unlock(&test_case.fixture.port->fd_table_lock);
    if (!state_ok) {
        ep_set_errno(EIO);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (test_case.fixture.port != NULL) {
        atomic_store_explicit(&test_case.fixture.port->waiter_active, 0,
                              memory_order_release);
    }
    udp_state_case_close(&test_case);
    return result;
}

static int test_udp_readless_rollback(void)
{
    if (test_udp_readless_mod_rollback() != 0) return -1;
    return test_udp_readless_rearm_rollback();
}

static int test_queued_rearm(void)
{
    static const uint64_t value = UINT64_C(0x1111222233334444);
    static const size_t idle_count = 50000;
    state_fixture_t fixture;
    epoll_event_ex event;
    epoll_event_ex extra;
    ep_sock_t *idle_socks = NULL;
    ep_sock_t *sock;
    uint64_t oneshot_visits_before;
    uint64_t queued_generation;
    uint64_t rearm_visits_before;
    int wait_result;
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
        fixture.port->oneshot_head == sock &&
        fixture.port->oneshot_tail == sock &&
        fixture.port->rearm_head == NULL &&
        fixture.port->rearm_tail == NULL &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) != 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        fixture.port->needs_rearm_count == 0 &&
        ep_port_worklists_valid_locked(fixture.port) &&
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
        fixture.port->oneshot_head == NULL &&
        fixture.port->oneshot_tail == NULL &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        fixture.port->needs_rearm_count == 1 &&
        fixture.port->rearm_head == sock &&
        fixture.port->rearm_tail == sock &&
        ep_port_worklists_valid_locked(fixture.port);
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok) {
        goto cleanup;
    }

    idle_socks = attach_idle_socks(fixture.port, idle_count);
    if (idle_socks == NULL) goto cleanup;
    pthread_mutex_lock(&fixture.port->fd_table_lock);
    rearm_visits_before = fixture.port->rearm_work_visits;
    oneshot_visits_before = fixture.port->oneshot_probe_visits;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);

    memset(&event, 0, sizeof(event));
    wait_result = ep_port_wait(fixture.port, &event, 1, 2000, NULL);
    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = wait_result == 1 && event.data.u64 == value &&
        event.user_ctx == &context && (event.events & EPOLLIN) != 0 &&
        (event.flags & WEPOLL_FLAG_ONESHOT_FIRED) != 0 &&
        fixture.port->rearm_work_visits - rearm_visits_before == 1 &&
        fixture.port->oneshot_probe_visits - oneshot_visits_before == 0 &&
        fixture.port->needs_rearm_count == 0 &&
        fixture.port->oneshot_fired_count == 1 &&
        fixture.port->oneshot_head == sock &&
        fixture.port->oneshot_tail == sock &&
        ep_port_worklists_valid_locked(fixture.port);
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok || detach_idle_socks(fixture.port, idle_socks,
                                       idle_count) != 0) {
        goto cleanup;
    }
    idle_socks = NULL;

    memset(&extra, 0, sizeof(extra));
    if (ep_port_wait(fixture.port, &extra, 1, 0, NULL) != 0) goto cleanup;

    /* A fired oneshot remains on the probe list until rearmed, deleted, or
     * closed natively.  The latter must unlink and free it during a wait. */
    (void)closesocket(fixture.server);
    fixture.server = INVALID_SOCKET;
    for (int attempt = 0; attempt < 100; attempt++) {
        memset(&extra, 0, sizeof(extra));
        if (ep_port_wait(fixture.port, &extra, 1, 0, NULL) != 0) goto cleanup;
        pthread_mutex_lock(&fixture.port->fd_table_lock);
        state_ok = fixture.port->fd_table_count == 0;
        pthread_mutex_unlock(&fixture.port->fd_table_lock);
        if (state_ok) break;
        Sleep(1);
    }
    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = fixture.port->fd_table_count == 0 &&
        fixture.port->needs_rearm_count == 0 &&
        fixture.port->oneshot_fired_count == 0 &&
        fixture.port->rearm_head == NULL && fixture.port->rearm_tail == NULL &&
        fixture.port->oneshot_head == NULL &&
        fixture.port->oneshot_tail == NULL &&
        ep_port_worklists_valid_locked(fixture.port);
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok) goto cleanup;
    result = 0;

cleanup:
    if (idle_socks != NULL) {
        (void)detach_idle_socks(fixture.port, idle_socks, idle_count);
    }
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
        fixture.port->oneshot_head == sock &&
        fixture.port->oneshot_tail == sock &&
        ep_port_worklists_valid_locked(fixture.port) &&
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
        fixture.port->oneshot_head == NULL &&
        fixture.port->oneshot_tail == NULL &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        fixture.port->needs_rearm_count == 1 &&
        fixture.port->rearm_head == sock &&
        fixture.port->rearm_tail == sock &&
        ep_port_worklists_valid_locked(fixture.port);
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok ||
        expect_one_oneshot(fixture.port, new_value, &new_context) != 0) {
        goto cleanup;
    }
    if (ep_port_unregister(fixture.port, fixture.server) != 0) goto cleanup;
    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = fixture.port->fd_table_count == 0 &&
        fixture.port->needs_rearm_count == 0 &&
        fixture.port->oneshot_fired_count == 0 &&
        fixture.port->rearm_head == NULL && fixture.port->rearm_tail == NULL &&
        fixture.port->oneshot_head == NULL &&
        fixture.port->oneshot_tail == NULL &&
        ep_port_worklists_valid_locked(fixture.port);
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok) goto cleanup;
    result = 0;

cleanup:
    fixture_close(&fixture);
    return result;
}

static int test_socket_add_submit_failure(void)
{
    static const uint64_t value = UINT64_C(0x6164646661696c31);
    state_fixture_t fixture;
    PNtDeviceIoControlFile original_submit = NULL;
    epoll_data_t data;
    int context;
    int submit_stub_installed = 0;
    int register_result;
    int register_error;
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

    errno = 0;
    register_result = ep_port_register(
        fixture.port, fixture.server, EPOLLIN | EPOLLONESHOT,
        EPOLLONESHOT, data, &context);
    register_error = errno;

    g_ntdll.NtDeviceIoControlFile = original_submit;
    submit_stub_installed = 0;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = register_result == -1 && register_error == EACCES &&
        InterlockedCompareExchange(&counted_submit_calls, 0, 0) == 1 &&
        fixture.port->fd_table_count == 0 &&
        fixture.port->sock_list_head == NULL &&
        fixture.port->pending_poll_count == 0 &&
        fixture.port->needs_rearm_count == 0 &&
        fixture.port->oneshot_fired_count == 0 &&
        fixture.port->rearm_head == NULL &&
        fixture.port->rearm_tail == NULL &&
        fixture.port->oneshot_head == NULL &&
        fixture.port->oneshot_tail == NULL &&
        atomic_load_explicit(&fixture.port->afd_info_pool.in_use,
                             memory_order_relaxed) == 0 &&
        ep_port_worklists_valid_locked(fixture.port) &&
        atomic_load_explicit(&fixture.port->ready_queue.queued,
                             memory_order_relaxed) == 0;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok || ep_port_register(
            fixture.port, fixture.server, EPOLLIN | EPOLLONESHOT,
            EPOLLONESHOT, data, &context) != 0 ||
        ep_port_unregister(fixture.port, fixture.server) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (submit_stub_installed)
        g_ntdll.NtDeviceIoControlFile = original_submit;
    fixture_close(&fixture);
    return result;
}

static int test_socket_add_immediate_success(void)
{
    static const uint64_t value = UINT64_C(0x616464696d6d6564);
    state_fixture_t fixture;
    immediate_success_wait_context_t wait_context;
    PNtDeviceIoControlFile original_submit = NULL;
    ep_sock_t *sock;
    epoll_data_t data;
    HANDLE wait_thread = NULL;
    DWORD wait_thread_status;
    OVERLAPPED_ENTRY extra_entry;
    ULONG extra_removed = 0;
    DWORD extra_error;
    BOOL extra_ok;
    int context;
    int dequeue_replaced = 0;
    int submit_replaced = 0;
    int register_result;
    int state_ok;
    int result = -1;

    memset(&wait_context, 0, sizeof(wait_context));
    if (fixture_open(&fixture) != 0) return -1;
    immediate_success_wait_entered =
        CreateEventW(NULL, TRUE, FALSE, NULL);
    immediate_success_packet_dequeued =
        CreateEventW(NULL, TRUE, FALSE, NULL);
    if (immediate_success_wait_entered == NULL ||
        immediate_success_packet_dequeued == NULL) {
        goto cleanup;
    }

    immediate_success_port = fixture.port;
    immediate_success_dequeue_delegate =
        fixture.port->get_queued_completion_status_ex;
    fixture.port->get_queued_completion_status_ex =
        immediate_success_dequeue_stub;
    dequeue_replaced = 1;
    original_submit = g_ntdll.NtDeviceIoControlFile;
    InterlockedExchange(&immediate_success_submit_calls, 0);
    InterlockedExchange(&immediate_success_invalid, 0);
    (void)InterlockedExchangePointer(
        &immediate_success_expected_overlapped, NULL);
    g_ntdll.NtDeviceIoControlFile = immediate_success_submit_stub;
    submit_replaced = 1;

    wait_context.port = fixture.port;
    wait_thread = CreateThread(
        NULL, 0, immediate_success_wait_thread, &wait_context, 0, NULL);
    if (wait_thread == NULL ||
        WaitForSingleObject(immediate_success_wait_entered, 2000) !=
            WAIT_OBJECT_0) {
        goto cleanup;
    }

    memset(&data, 0, sizeof(data));
    data.u64 = value;
    register_result = ep_port_register(
        fixture.port, fixture.server, EPOLLIN | EPOLLONESHOT,
        EPOLLONESHOT, data, &context);
    g_ntdll.NtDeviceIoControlFile = original_submit;
    submit_replaced = 0;
    if (register_result != 0 ||
        WaitForSingleObject(wait_thread, 5000) != WAIT_OBJECT_0) {
        goto cleanup;
    }
    fixture.port->get_queued_completion_status_ex =
        immediate_success_dequeue_delegate;
    dequeue_replaced = 0;

    sock = fixture_sock(&fixture);
    if (sock == NULL) goto cleanup;
    memset(&extra_entry, 0, sizeof(extra_entry));
    SetLastError(ERROR_SUCCESS);
    extra_ok = fixture.port->get_queued_completion_status_ex(
        fixture.port->iocp, &extra_entry, 1, &extra_removed, 0, FALSE);
    extra_error = GetLastError();
    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok =
        InterlockedCompareExchange(&immediate_success_submit_calls, 0, 0) ==
            1 &&
        InterlockedCompareExchange(&immediate_success_invalid, 0, 0) == 0 &&
        fixture.port->fd_table_count == 1 &&
        fixture.port->pending_poll_count == 0 &&
        fixture.port->async_error == 0 &&
        fixture.port->needs_rearm_count == 0 &&
        fixture.port->oneshot_fired_count == 1 &&
        fixture.port->oneshot_head == sock &&
        fixture.port->oneshot_tail == sock &&
        atomic_load_explicit(&fixture.port->ready_queue.queued,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->state,
                             memory_order_relaxed) == EP_SOCK_REGISTERED &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        sock->oneshot_fired != 0 && sock->needs_rearm == 0 &&
        sock->afd_poll_key_owned == 0 && sock->afd_poll_target == NULL &&
        sock->afd_poll_key_reservation == NULL &&
        !extra_ok && extra_error == WAIT_TIMEOUT &&
        WaitForSingleObject(immediate_success_packet_dequeued, 0) ==
            WAIT_OBJECT_0 &&
        ep_port_worklists_valid_locked(fixture.port);
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok) {
        fprintf(stderr,
                "immediate state: calls=%ld invalid=%ld fd=%zu pending=%zu "
                "async=%d rearm=%zu oneshot=%zu queued=%zu state=%lu "
                "poll=%lu ready=%lu fired=%u needs=%u key=%u extra=%d/%lu/%lu "
                "dequeued=%lu\n",
                InterlockedCompareExchange(
                    &immediate_success_submit_calls, 0, 0),
                InterlockedCompareExchange(&immediate_success_invalid, 0, 0),
                fixture.port->fd_table_count,
                fixture.port->pending_poll_count, fixture.port->async_error,
                fixture.port->needs_rearm_count,
                fixture.port->oneshot_fired_count,
                atomic_load_explicit(&fixture.port->ready_queue.queued,
                                     memory_order_relaxed),
                (unsigned long)atomic_load_explicit(
                    &sock->state, memory_order_relaxed),
                (unsigned long)atomic_load_explicit(
                    &sock->poll_status, memory_order_relaxed),
                (unsigned long)atomic_load_explicit(
                    &sock->ready_queued, memory_order_relaxed),
                (unsigned)sock->oneshot_fired, (unsigned)sock->needs_rearm,
                (unsigned)sock->afd_poll_key_owned, extra_ok != FALSE,
                (unsigned long)extra_removed, (unsigned long)extra_error,
                (unsigned long)WaitForSingleObject(
                    immediate_success_packet_dequeued, 0));
    }
    if (!state_ok || wait_context.wait_result != 1 ||
        wait_context.event.events != EPOLLIN ||
        wait_context.event.data.u64 != value ||
        wait_context.event.user_ctx != &context ||
        wait_context.event.flags != WEPOLL_FLAG_ONESHOT_FIRED ||
        wait_context.event.timestamp == 0 ||
        ep_port_unregister(fixture.port, fixture.server) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (submit_replaced) {
        g_ntdll.NtDeviceIoControlFile = original_submit;
    }
    if (wait_thread != NULL) {
        wait_thread_status = WaitForSingleObject(wait_thread, 6000);
        if (wait_thread_status != WAIT_OBJECT_0) {
            if (fixture.port != NULL) ep_port_begin_close(fixture.port);
            wait_thread_status = WaitForSingleObject(wait_thread, 2000);
            result = -1;
        }
        if (wait_thread_status != WAIT_OBJECT_0) {
            /* Do not tear down objects still reachable by the wedged waiter.
             * This mode runs in its own process, which exits immediately after
             * reporting the failure and reclaims the intentionally leaked
             * test state safely. */
            CloseHandle(wait_thread);
            ep_set_errno(ETIMEDOUT);
            return -1;
        }
    }
    if (dequeue_replaced && fixture.port != NULL) {
        fixture.port->get_queued_completion_status_ex =
            immediate_success_dequeue_delegate;
    }
    if (wait_thread != NULL) CloseHandle(wait_thread);
    immediate_success_port = NULL;
    immediate_success_dequeue_delegate = NULL;
    (void)InterlockedExchangePointer(
        &immediate_success_expected_overlapped, NULL);
    if (immediate_success_packet_dequeued != NULL) {
        CloseHandle(immediate_success_packet_dequeued);
        immediate_success_packet_dequeued = NULL;
    }
    if (immediate_success_wait_entered != NULL) {
        CloseHandle(immediate_success_wait_entered);
        immediate_success_wait_entered = NULL;
    }
    fixture_close(&fixture);
    return result;
}

static int test_aux_add_lazy(void)
{
    static const char byte = 'a';
    ep_port_t *port = NULL;
    ep_sock_t *sock;
    HANDLE semaphore = NULL;
    HANDLE pipe_read = NULL;
    HANDLE pipe_write = NULL;
    epoll_data_t data;
    DWORD available = 0;
    DWORD transferred = 0;
    int state_ok;
    int result = -1;

    semaphore = CreateSemaphoreW(NULL, 1, 1, NULL);
    if (semaphore == NULL || ep_port_create(0, 0, &port) != 0) {
        goto cleanup;
    }
    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0x6175785f73656d31);
    if (ep_port_register(port, (SOCKET)(uintptr_t)semaphore,
                         EPOLLIN, 0, data, NULL) != 0) {
        goto cleanup;
    }
    pthread_mutex_lock(&port->fd_table_lock);
    sock = port->sock_list_head;
    state_ok = sock != NULL && sock->next == NULL &&
        sock->kind == EP_REG_WAITABLE && sock->needs_rearm != 0 &&
        sock->wait_registration == NULL &&
        atomic_load_explicit(&sock->state,
                             memory_order_relaxed) == EP_SOCK_REGISTERED &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->callback_active,
                             memory_order_relaxed) == 0 &&
        atomic_load_explicit(&sock->completion_posted,
                             memory_order_relaxed) == 0 &&
        port->pending_poll_count == 0 && port->needs_rearm_count == 1 &&
        port->rearm_head == sock && port->rearm_tail == sock &&
        ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok ||
        WaitForSingleObject(semaphore, 0) != WAIT_OBJECT_0) {
        goto cleanup;
    }
    {
        ep_port_t *destroy_port = port;

        port = NULL;
        if (ep_port_destroy(destroy_port) != 0) goto cleanup;
    }
    CloseHandle(semaphore);
    semaphore = NULL;

    if (!CreatePipe(&pipe_read, &pipe_write, NULL, 0) ||
        !WriteFile(pipe_write, &byte, 1, &transferred, NULL) ||
        transferred != 1 || ep_port_create(0, 0, &port) != 0) {
        goto cleanup;
    }
    data.u64 = UINT64_C(0x6175785f70697031);
    if (ep_port_register(port, (SOCKET)(uintptr_t)pipe_read,
                         EPOLLIN, 0, data, NULL) != 0) {
        goto cleanup;
    }
    pthread_mutex_lock(&port->fd_table_lock);
    sock = port->sock_list_head;
    state_ok = sock != NULL && sock->next == NULL &&
        sock->kind == EP_REG_PIPE && sock->needs_rearm != 0 &&
        sock->wait_registration == NULL && sock->pending_events == 0 &&
        atomic_load_explicit(&sock->state,
                             memory_order_relaxed) == EP_SOCK_REGISTERED &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->completion_posted,
                             memory_order_relaxed) == 0 &&
        port->pending_poll_count == 0 && port->needs_rearm_count == 1 &&
        port->rearm_head == sock && port->rearm_tail == sock &&
        ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok || !PeekNamedPipe(pipe_read, NULL, 0, NULL,
                                    &available, NULL) || available != 1) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (port != NULL) {
        ep_port_t *destroy_port = port;

        port = NULL;
        if (ep_port_destroy(destroy_port) != 0) result = -1;
    }
    if (pipe_write != NULL) CloseHandle(pipe_write);
    if (pipe_read != NULL) CloseHandle(pipe_read);
    if (semaphore != NULL) CloseHandle(semaphore);
    return result;
}

static int test_failed_mod_rollback(void)
{
    static const uint64_t old_value = UINT64_C(0x7172737475767778);
    static const uint64_t new_value = UINT64_C(0x8182838485868788);
    static const uint32_t old_observed_events = EPOLLIN | EPOLLHUP;
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
    sock->observed_events = old_observed_events;
    sock->et_holdoff = 1;
    sock->pipe_terminal_delivered = 1;
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
        sock->observed_events == old_observed_events &&
        sock->et_holdoff != 0 && sock->pipe_terminal_delivered != 0 &&
        sock->oneshot_fired != 0 &&
        fixture.port->oneshot_fired_count == 1 &&
        fixture.port->oneshot_head == sock &&
        fixture.port->oneshot_tail == sock &&
        fixture.port->rearm_head == NULL &&
        fixture.port->rearm_tail == NULL &&
        ep_port_worklists_valid_locked(fixture.port) &&
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
    static const uint32_t old_observed_events = EPOLLIN | EPOLLERR;
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
    sock->observed_events = old_observed_events;
    sock->et_holdoff = 1;
    sock->pipe_terminal_delivered = 1;
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
        sock->generation == old_generation &&
        sock->observed_events == old_observed_events &&
        sock->et_holdoff != 0 && sock->pipe_terminal_delivered != 0 &&
        sock->oneshot_fired != 0 &&
        fixture.port->oneshot_fired_count == 1 &&
        fixture.port->oneshot_head == sock &&
        fixture.port->oneshot_tail == sock &&
        fixture.port->rearm_head == NULL &&
        fixture.port->rearm_tail == NULL &&
        ep_port_worklists_valid_locked(fixture.port) &&
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
        fixture.port->needs_rearm_count == 0 &&
        fixture.port->oneshot_fired_count == 0 &&
        ep_port_worklists_valid_locked(fixture.port);
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
        fixture.port->needs_rearm_count == 0 &&
        fixture.port->oneshot_fired_count == 0 &&
        ep_port_worklists_valid_locked(fixture.port);
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
        fixture.port->needs_rearm_count == 0 &&
        fixture.port->oneshot_fired_count == 0 &&
        ep_port_worklists_valid_locked(fixture.port);
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
        fixture.port->needs_rearm_count == 1 &&
        fixture.port->rearm_head == sock &&
        fixture.port->rearm_tail == sock &&
        ep_port_worklists_valid_locked(fixture.port);
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
        fixture.port->needs_rearm_count == 1 &&
        fixture.port->rearm_head == sock &&
        fixture.port->rearm_tail == sock &&
        ep_port_worklists_valid_locked(fixture.port);
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok || send_byte(fixture.client) != 0 ||
        expect_one_oneshot(fixture.port, latest_value, &latest_context) != 0)
        goto cleanup;
    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = fixture.port->needs_rearm_count == 0 &&
        fixture.port->oneshot_fired_count == 1 &&
        fixture.port->oneshot_head == sock &&
        fixture.port->oneshot_tail == sock &&
        ep_port_worklists_valid_locked(fixture.port);
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

static int test_pending_expansion_ready_race(void)
{
    static const uint64_t old_value = UINT64_C(0xb1b2b3b4b5b6b7b8);
    static const uint64_t new_value = UINT64_C(0xc1c2c3c4c5c6c7c8);
    const uint32_t old_events = EPOLLOUT | EPOLLONESHOT;
    const uint32_t new_events = EPOLLIN | EPOLLOUT | EPOLLONESHOT;
    state_fixture_t fixture;
    PNtCancelIoFileEx original_cancel = NULL;
    PNtDeviceIoControlFile original_submit = NULL;
    ep_sock_t *sock = NULL;
    epoll_event_ex event;
    uint64_t old_generation = 0;
    int old_context;
    int new_context;
    int cancel_installed = 0;
    int submit_installed = 0;
    int registered = 0;
    int state_ok;
    int result = -1;

    if (fixture_open(&fixture) != 0) return -1;

    expansion_refresh_port = fixture.port;
    InterlockedExchange(&expansion_refresh_cancel_calls, 0);
    InterlockedExchange(&expansion_refresh_submit_calls, 0);
    InterlockedExchange(&expansion_refresh_invalid, 0);
    memset(expansion_refresh_masks, 0, sizeof(expansion_refresh_masks));
    original_submit = g_ntdll.NtDeviceIoControlFile;
    g_ntdll.NtDeviceIoControlFile = expansion_refresh_submit_stub;
    submit_installed = 1;
    atomic_store_explicit(&fixture.port->active_wait_epoch, 1,
                          memory_order_release);
    atomic_store_explicit(&fixture.port->waiter_active, 1,
                          memory_order_release);

    if (register_events(&fixture, old_events, EPOLLONESHOT,
                        old_value, &old_context) != 0) {
        goto cleanup;
    }
    registered = 1;
    sock = fixture_sock(&fixture);
    if (sock == NULL) goto cleanup;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    old_generation = sock->generation;
    /* Disable the TCP select merge so both delivered classes must come from
     * the fresh full-mask AFD snapshot, not a test-host level sample. */
    sock->socket_protocol = EP_SOCKET_PROTOCOL_UNKNOWN;
    state_ok =
        InterlockedCompareExchange(&expansion_refresh_submit_calls, 0, 0) ==
            1 &&
        expansion_refresh_masks[0] == ep_epoll_to_afd_events(old_events) &&
        sock->submitted_wait_epoch == 1 &&
        sock->submitted_afd_events == ep_epoll_to_afd_events(old_events) &&
        fixture.port->pending_poll_count == 1 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok) goto cleanup;

    original_cancel = g_ntdll.NtCancelIoFileEx;
    g_ntdll.NtCancelIoFileEx = expansion_refresh_cancel_stub;
    cancel_installed = 1;
    if (modify_events(&fixture, new_events, EPOLLONESHOT,
                      new_value, &new_context) != 0) {
        goto cleanup;
    }
    g_ntdll.NtCancelIoFileEx = original_cancel;
    cancel_installed = 0;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok =
        InterlockedCompareExchange(&expansion_refresh_cancel_calls, 0, 0) ==
            1 &&
        InterlockedCompareExchange(&expansion_refresh_submit_calls, 0, 0) ==
            1 &&
        sock->generation != old_generation &&
        sock->user_events == new_events &&
        sock->user_data.u64 == new_value && sock->user_ctx == &new_context &&
        sock->submitted_wait_epoch == 1 &&
        sock->submitted_afd_events == ep_epoll_to_afd_events(old_events) &&
        sock->needs_rearm && fixture.port->needs_rearm_count == 1 &&
        fixture.port->rearm_head == sock && fixture.port->rearm_tail == sock &&
        fixture.port->pending_poll_count == 1 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING &&
        ep_port_worklists_valid_locked(fixture.port);
    if (state_ok) {
        sock->afd_info->NumberOfHandles = 1;
        sock->afd_info->Handles[0].Events = AFD_POLL_SEND;
        sock->afd_info->Handles[0].Status = STATUS_SUCCESS;
        sock->io_status_block.Status = STATUS_SUCCESS;
        sock->io_status_block.Information = sizeof(*sock->afd_info);
    }
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok) goto cleanup;

    ep_sock_handle_completion(sock, 0, STATUS_SUCCESS);

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok =
        InterlockedCompareExchange(&expansion_refresh_cancel_calls, 0, 0) ==
            1 &&
        InterlockedCompareExchange(&expansion_refresh_submit_calls, 0, 0) ==
            2 &&
        InterlockedCompareExchange(&expansion_refresh_invalid, 0, 0) == 0 &&
        expansion_refresh_masks[1] == ep_epoll_to_afd_events(new_events) &&
        sock->submitted_wait_epoch == 1 &&
        sock->submitted_afd_events == ep_epoll_to_afd_events(new_events) &&
        sock->pending_events == (EPOLLIN | EPOLLOUT) &&
        sock->user_data.u64 == new_value && sock->user_ctx == &new_context &&
        !sock->needs_rearm && sock->oneshot_fired &&
        fixture.port->pending_poll_count == 0 &&
        fixture.port->needs_rearm_count == 0 &&
        fixture.port->oneshot_fired_count == 1 &&
        fixture.port->oneshot_head == sock &&
        fixture.port->oneshot_tail == sock &&
        fixture.port->async_error == 0 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->state,
                             memory_order_relaxed) == EP_SOCK_READY &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 1 &&
        atomic_load_explicit(&fixture.port->ready_queue.queued,
                             memory_order_relaxed) == 1 &&
        sock->afd_poll_key_owned == 0 && sock->afd_poll_target == NULL &&
        sock->afd_poll_key_reservation == NULL &&
        ep_port_worklists_valid_locked(fixture.port);
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok) goto cleanup;

    g_ntdll.NtDeviceIoControlFile = original_submit;
    submit_installed = 0;
    atomic_store_explicit(&fixture.port->waiter_active, 0,
                          memory_order_release);
    atomic_store_explicit(&fixture.port->active_wait_epoch, 0,
                          memory_order_release);
    memset(&event, 0, sizeof(event));
    if (ep_port_wait(fixture.port, &event, 1, 0, NULL) != 1 ||
        event.events != (EPOLLIN | EPOLLOUT) ||
        event.data.u64 != new_value || event.user_ctx != &new_context ||
        event.flags != WEPOLL_FLAG_ONESHOT_FIRED || event.timestamp == 0) {
        goto cleanup;
    }
    if (ep_port_unregister(fixture.port, fixture.server) != 0) {
        goto cleanup;
    }
    registered = 0;
    result = 0;

cleanup:
    if (cancel_installed) {
        g_ntdll.NtCancelIoFileEx = original_cancel;
    }
    if (submit_installed) {
        g_ntdll.NtDeviceIoControlFile = original_submit;
    }
    if (fixture.port != NULL) {
        int pending = 0;

        atomic_store_explicit(&fixture.port->waiter_active, 0,
                              memory_order_release);
        atomic_store_explicit(&fixture.port->active_wait_epoch, 0,
                              memory_order_release);
        if (registered && sock != NULL) {
            pending = atomic_load_explicit(
                &sock->poll_status, memory_order_relaxed) != EP_POLL_IDLE;
        }
        if (registered) {
            (void)ep_port_unregister(fixture.port, fixture.server);
            registered = 0;
        }
        if (pending) {
            sock->io_status_block.Status = STATUS_CANCELLED;
            ep_sock_handle_completion(sock, 0, STATUS_CANCELLED);
        }
    }
    expansion_refresh_port = NULL;
    fixture_close(&fixture);
    return result;
}

static int run_tcp_current_rdhup_case(ULONG tcp_state,
                                      uint32_t expected_events,
                                      int half_close_peer,
                                      int live_query)
{
    static const uint64_t value = UINT64_C(0xd1d2d3d4d5d6d7d8);
    const uint32_t user_events =
        EPOLLOUT | EPOLLRDHUP | EPOLLONESHOT;
    state_fixture_t fixture;
    PNtDeviceIoControlFile original_submit = NULL;
    ep_tcp_state_query_fn original_query = NULL;
    ep_sock_t *sock = NULL;
    epoll_event_ex event;
    fd_set read_set;
    struct timeval timeout;
    ULONG live_state = 0;
    u_long available = 0;
    ULONGLONG deadline;
    int context;
    int registered = 0;
    int submit_installed = 0;
    int pending;
    int state_ok;
    int result = -1;

    if (fixture_open(&fixture) != 0) return -1;

    tcp_current_port = fixture.port;
    tcp_current_expected_sock = NULL;
    tcp_current_query_state = tcp_state;
    tcp_current_submit_mask = 0;
    InterlockedExchange(&tcp_current_submit_calls, 0);
    InterlockedExchange(&tcp_current_submit_invalid, 0);
    InterlockedExchange(&tcp_current_query_calls, 0);
    InterlockedExchange(&tcp_current_query_mismatch, 0);
    original_submit = g_ntdll.NtDeviceIoControlFile;
    original_query = fixture.port->tcp_state_query;
    g_ntdll.NtDeviceIoControlFile = tcp_current_submit_stub;
    if (!live_query) {
        fixture.port->tcp_state_query = tcp_current_state_query_stub;
    }
    submit_installed = 1;
    atomic_store_explicit(&fixture.port->active_wait_epoch, 1,
                          memory_order_release);
    atomic_store_explicit(&fixture.port->waiter_active, 1,
                          memory_order_release);

    if (register_events(&fixture, user_events, EPOLLONESHOT,
                        value, &context) != 0) {
        goto cleanup;
    }
    registered = 1;
    sock = fixture_sock(&fixture);
    tcp_current_expected_sock = sock;
    if (sock == NULL) goto cleanup;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok =
        InterlockedCompareExchange(&tcp_current_submit_calls, 0, 0) == 1 &&
        InterlockedCompareExchange(&tcp_current_submit_invalid, 0, 0) == 0 &&
        tcp_current_submit_mask == ep_epoll_to_afd_events(user_events) &&
        sock->submitted_wait_epoch == 1 &&
        sock->submitted_afd_events ==
            ep_epoll_to_afd_events(user_events) &&
        fixture.port->pending_poll_count == 1 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_PENDING;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok || send_byte(fixture.client) != 0 ||
        (half_close_peer &&
         shutdown(fixture.client, SD_SEND) == SOCKET_ERROR)) {
        goto cleanup;
    }

    FD_ZERO(&read_set);
    FD_SET(fixture.server, &read_set);
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    if (select(0, &read_set, NULL, NULL, &timeout) != 1 ||
        !FD_ISSET(fixture.server, &read_set)) {
        goto cleanup;
    }
    if (live_query) {
        int remote_fin = 0;

        deadline = GetTickCount64() + 2000;
        do {
            if (original_query != NULL &&
                original_query(sock, &live_state) > 0 &&
                (live_state == TEST_TCP_STATE_CLOSE_WAIT ||
                 live_state == TEST_TCP_STATE_CLOSING ||
                 live_state == TEST_TCP_STATE_LAST_ACK ||
                 live_state == TEST_TCP_STATE_TIME_WAIT)) {
                remote_fin = 1;
                break;
            }
            if (sock->tcp_info_capability ==
                    EP_SOCKET_TCP_INFO_UNAVAILABLE) {
                result = 77;
                goto cleanup;
            }
            Sleep(1);
        } while (GetTickCount64() < deadline);
        if (!remote_fin ||
            ioctlsocket(fixture.server, FIONREAD, &available) == SOCKET_ERROR ||
            available == 0) {
            goto cleanup;
        }
    }

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    sock->afd_info->NumberOfHandles = 1;
    sock->afd_info->Handles[0].Events = AFD_POLL_SEND;
    sock->afd_info->Handles[0].Status = STATUS_SUCCESS;
    sock->io_status_block.Status = STATUS_SUCCESS;
    sock->io_status_block.Information = sizeof(*sock->afd_info);
    pthread_mutex_unlock(&fixture.port->fd_table_lock);

    ep_sock_handle_completion(sock, 0, STATUS_SUCCESS);

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok =
        (live_query ||
         (InterlockedCompareExchange(&tcp_current_query_calls, 0, 0) == 1 &&
          InterlockedCompareExchange(
              &tcp_current_query_mismatch, 0, 0) == 0)) &&
        sock->pending_events == expected_events &&
        sock->user_data.u64 == value && sock->user_ctx == &context &&
        !sock->needs_rearm && sock->oneshot_fired &&
        fixture.port->pending_poll_count == 0 &&
        fixture.port->needs_rearm_count == 0 &&
        fixture.port->oneshot_fired_count == 1 &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->state,
                             memory_order_relaxed) == EP_SOCK_READY &&
        atomic_load_explicit(&sock->ready_queued,
                             memory_order_relaxed) == 1 &&
        atomic_load_explicit(&fixture.port->ready_queue.queued,
                             memory_order_relaxed) == 1 &&
        sock->afd_poll_key_owned == 0 && sock->afd_poll_target == NULL &&
        sock->afd_poll_key_reservation == NULL &&
        ep_port_worklists_valid_locked(fixture.port);
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok) goto cleanup;

    g_ntdll.NtDeviceIoControlFile = original_submit;
    submit_installed = 0;
    fixture.port->tcp_state_query = original_query;
    atomic_store_explicit(&fixture.port->waiter_active, 0,
                          memory_order_release);
    atomic_store_explicit(&fixture.port->active_wait_epoch, 0,
                          memory_order_release);
    memset(&event, 0, sizeof(event));
    if (ep_port_wait(fixture.port, &event, 1, 0, NULL) != 1 ||
        event.events != expected_events || event.data.u64 != value ||
        event.user_ctx != &context ||
        event.flags != WEPOLL_FLAG_ONESHOT_FIRED || event.timestamp == 0) {
        goto cleanup;
    }
    if (ep_port_unregister(fixture.port, fixture.server) != 0) {
        goto cleanup;
    }
    registered = 0;
    result = 0;

cleanup:
    if (submit_installed) {
        g_ntdll.NtDeviceIoControlFile = original_submit;
    }
    if (fixture.port != NULL) {
        fixture.port->tcp_state_query = original_query;
        atomic_store_explicit(&fixture.port->waiter_active, 0,
                              memory_order_release);
        atomic_store_explicit(&fixture.port->active_wait_epoch, 0,
                              memory_order_release);
        pending = registered && sock != NULL &&
            atomic_load_explicit(&sock->poll_status,
                                 memory_order_relaxed) != EP_POLL_IDLE;
        if (registered) {
            (void)ep_port_unregister(fixture.port, fixture.server);
            registered = 0;
        }
        if (pending) {
            sock->io_status_block.Status = STATUS_CANCELLED;
            ep_sock_handle_completion(sock, 0, STATUS_CANCELLED);
        }
    }
    tcp_current_port = NULL;
    tcp_current_expected_sock = NULL;
    fixture_close(&fixture);
    return result;
}

static int test_tcp_current_rdhup(void)
{
    if (run_tcp_current_rdhup_case(
            TEST_TCP_STATE_ESTABLISHED, EPOLLOUT, 0, 0) != 0 ||
        run_tcp_current_rdhup_case(
            TEST_TCP_STATE_CLOSE_WAIT,
            EPOLLOUT | EPOLLRDHUP, 1, 0) != 0) {
        return -1;
    }
    return 0;
}

static int test_tcp_info_runtime(void)
{
    return run_tcp_current_rdhup_case(
        TEST_TCP_STATE_CLOSE_WAIT, EPOLLOUT | EPOLLRDHUP, 1, 1);
}

static int test_transitional_idle(void)
{
    state_fixture_t fixture;
    PNtDeviceIoControlFile original_submit;
    epoll_data_t data;
    epoll_event_ex event;
    size_t fd_count;
    LONG add_submit_calls;
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
    add_submit_calls = InterlockedCompareExchange(
        &counted_submit_calls, 0, 0);
    memset(&event, 0, sizeof(event));
    wait_result = ep_port_wait(fixture.port, &event, 1, 200, NULL);
    g_ntdll.NtDeviceIoControlFile = original_submit;

    pthread_mutex_lock(&fixture.port->fd_table_lock);
    fd_count = fixture.port->fd_table_count;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);

    /* Every lifetime mode submits the first request from ADD.  The following
     * idle wait must reuse that request rather than arm a second one. */
    if (add_submit_calls == 1 && wait_result == 0 &&
        InterlockedCompareExchange(&counted_submit_calls, 0, 0) == 1 &&
        fd_count == 1) {
        result = 0;
    }

cleanup:
    fixture_close(&fixture);
    return result;
}

static int run_pipe_invalid_et_case(pipe_query_injection_t injection)
{
    static const char byte = 'p';
    ep_port_t *port = NULL;
    HANDLE read_handle = NULL;
    HANDLE write_handle = NULL;
    PNtQueryInformationFileFn original_query = NULL;
    epoll_data_t data;
    epoll_event_ex event;
    DWORD transferred = 0;
    int query_stub_installed = 0;
    int result = -1;

    if (!CreatePipe(&read_handle, &write_handle, NULL, 0) ||
        !WriteFile(write_handle, &byte, 1, &transferred, NULL) ||
        transferred != 1 || ep_port_create(0, 0, &port) != 0) {
        goto cleanup;
    }

    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0x7071727374757677);
    if (ep_port_register(port, (SOCKET)(uintptr_t)read_handle,
                         EPOLLIN | EPOLLET, EPOLLET, data, NULL) != 0) {
        goto cleanup;
    }

    original_query = g_ntdll.NtQueryInformationFile;
    if (original_query == NULL) {
        ep_set_errno(EOPNOTSUPP);
        goto cleanup;
    }
    pipe_query_delegate = original_query;
    pipe_query_target = read_handle;
    pipe_query_injection_mode = injection;
    InterlockedExchange(&pipe_query_calls, 0);
    InterlockedExchange(&pipe_query_injected, 0);
    g_ntdll.NtQueryInformationFile = pipe_query_transient_stub;
    query_stub_installed = 1;

    memset(&event, 0, sizeof(event));
    if (ep_port_wait(port, &event, 1, 2000, NULL) != 1 ||
        event.data.u64 != data.u64 || (event.events & EPOLLIN) == 0 ||
        InterlockedCompareExchange(&pipe_query_injected, 0, 0) != 1) {
        goto cleanup;
    }

    /* The pipe remains continuously readable.  The third metadata query is
     * transiently invalid, and at least one later query observes the same
     * stable readable level.  That invalid middle sample must not reopen the
     * ET latch and manufacture a duplicate EPOLLIN edge. */
    memset(&event, 0, sizeof(event));
    if (ep_port_wait(port, &event, 1, 50, NULL) != 0 ||
        InterlockedCompareExchange(&pipe_query_calls, 0, 0) < 4) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (query_stub_installed) {
        g_ntdll.NtQueryInformationFile = original_query;
    }
    pipe_query_delegate = NULL;
    pipe_query_target = NULL;
    pipe_query_injection_mode = 0;
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    if (write_handle != NULL) CloseHandle(write_handle);
    if (read_handle != NULL) CloseHandle(read_handle);
    return result;
}

static int run_pipe_terminal_invalid_et_case(void)
{
    wchar_t name[160];
    ep_port_t *port = NULL;
    ep_sock_t *sock;
    HANDLE server = INVALID_HANDLE_VALUE;
    HANDLE client = INVALID_HANDLE_VALUE;
    PNtQueryInformationFileFn original_query = NULL;
    epoll_data_t data;
    epoll_event_ex event;
    uint64_t rearm_visits;
    LONG query_count;
    LONG serial;
    int query_stub_installed = 0;
    int state_ok;
    int result = -1;

    serial = InterlockedIncrement(&pipe_name_serial);
    _snwprintf(name, sizeof(name) / sizeof(name[0]),
               L"\\\\.\\pipe\\wepoll-ex-state-terminal-%lu-%llu-%ld",
               (unsigned long)GetCurrentProcessId(),
               (unsigned long long)GetTickCount64(), (long)serial);
    name[(sizeof(name) / sizeof(name[0])) - 1] = L'\0';

    server = CreateNamedPipeW(
        name, PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 0, NULL);
    if (server == INVALID_HANDLE_VALUE) {
        goto cleanup;
    }
    client = CreateFileW(name, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (client == INVALID_HANDLE_VALUE ||
        ep_port_create(0, 0, &port) != 0) {
        goto cleanup;
    }

    memset(&data, 0, sizeof(data));
    data.u64 = UINT64_C(0x8081828384858687);
    if (ep_port_register(port, (SOCKET)(uintptr_t)client,
                         EPOLLIN | EPOLLET, EPOLLET, data, NULL) != 0) {
        goto cleanup;
    }

    original_query = g_ntdll.NtQueryInformationFile;
    if (original_query == NULL) {
        ep_set_errno(EOPNOTSUPP);
        goto cleanup;
    }
    pipe_query_delegate = original_query;
    pipe_query_target = client;
    pipe_query_injection_mode = PIPE_QUERY_INJECT_INVALID_LENGTH;
    InterlockedExchange(&pipe_query_calls, 0);
    InterlockedExchange(&pipe_query_injected, 0);
    g_ntdll.NtQueryInformationFile = pipe_query_transient_stub;
    query_stub_installed = 1;

    CloseHandle(server);
    server = INVALID_HANDLE_VALUE;

    memset(&event, 0, sizeof(event));
    if (ep_port_wait(port, &event, 1, 2000, NULL) != 1 ||
        event.data.u64 != data.u64 || event.events != EPOLLHUP ||
        InterlockedCompareExchange(&pipe_query_injected, 0, 0) != 1) {
        goto cleanup;
    }

    /* The ready-drain confirmation was invalid, so the client endpoint must
     * remain eligible for one native retry.  A later valid terminal snapshot
     * confirms that this HANDLE cannot transition again; it must then become
     * truly idle without delivering a duplicate terminal edge. */
    memset(&event, 0, sizeof(event));
    if (ep_port_wait(port, &event, 1, 100, NULL) != 0 ||
        InterlockedCompareExchange(&pipe_query_calls, 0, 0) < 4) {
        goto cleanup;
    }

    pthread_mutex_lock(&port->fd_table_lock);
    sock = port->sock_list_head;
    query_count = InterlockedCompareExchange(&pipe_query_calls, 0, 0);
    rearm_visits = port->rearm_work_visits;
    state_ok = sock != NULL && sock->next == NULL &&
        sock->kind == EP_REG_PIPE &&
        atomic_load_explicit(&sock->poll_status,
                             memory_order_relaxed) == EP_POLL_IDLE &&
        atomic_load_explicit(&sock->state,
                             memory_order_relaxed) == EP_SOCK_REGISTERED &&
        sock->wait_registration == NULL && !sock->needs_rearm &&
        !sock->et_holdoff && port->pending_poll_count == 0 &&
        port->needs_rearm_count == 0 && port->rearm_head == NULL &&
        port->rearm_tail == NULL && ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok) {
        goto cleanup;
    }

    memset(&event, 0, sizeof(event));
    if (ep_port_wait(port, &event, 1, 50, NULL) != 0) {
        goto cleanup;
    }
    pthread_mutex_lock(&port->fd_table_lock);
    state_ok = port->rearm_work_visits == rearm_visits &&
        port->pending_poll_count == 0 && port->needs_rearm_count == 0 &&
        ep_port_worklists_valid_locked(port);
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!state_ok ||
        InterlockedCompareExchange(&pipe_query_calls, 0, 0) != query_count) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (query_stub_installed) {
        g_ntdll.NtQueryInformationFile = original_query;
    }
    pipe_query_delegate = NULL;
    pipe_query_target = NULL;
    pipe_query_injection_mode = 0;
    if (port != NULL && ep_port_destroy(port) != 0) result = -1;
    if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
    return result;
}

static int test_pipe_invalid_et_snapshot(void)
{
    if (run_pipe_invalid_et_case(PIPE_QUERY_INJECT_UNKNOWN_FAILURE) != 0) {
        return -1;
    }
    if (run_pipe_invalid_et_case(PIPE_QUERY_INJECT_INVALID_LENGTH) != 0) {
        return -1;
    }

    return run_pipe_terminal_invalid_et_case();
}

#define LARGE_WAIT_EVENT_COUNT 4097U
#define LARGE_WAIT_FEED_CHUNK 64U
#define LARGE_WAIT_BUDGET_DEQUEUES 64U
#define LARGE_WAIT_CONTROL_INITIAL 2U
#define LARGE_WAIT_FD_BASE ((uintptr_t)UINT32_C(0x100000))
#define LARGE_WAIT_TIMESTAMP_TAG UINT64_C(0x6c61726700000000)

typedef struct large_wait_control_context {
    ep_port_t *port;
    ep_sock_t *modify_sock;
    ep_sock_t *rearm_sock;
    HANDLE start_event;
    HANDLE done_event;
    epoll_data_t modify_data;
    void *modify_user_ctx;
    _Atomic int triggered;
    _Atomic size_t feed_calls_at_trigger;
    _Atomic int worker_error;
    _Atomic int saw_waiter_active;
    _Atomic int saw_waiter_coalescing;
    _Atomic int saw_targets_drained;
    _Atomic int modify_result;
    _Atomic int modify_error;
    _Atomic int rearm_result;
    _Atomic int rearm_error;
} large_wait_control_context_t;

typedef struct large_wait_fixture {
    ep_port_t *port;
    ep_sock_t **socks;
    ep_ready_node_t **pending_nodes;
    size_t count;
    size_t feed_index;
    size_t feed_calls;
    size_t feed_chunk;
    DWORD feed_delay_ms;
    large_wait_control_context_t *control;
    int feed_invalid;
    int hook_installed;
    PGetQueuedCompletionStatusEx original_dequeue;
} large_wait_fixture_t;

static large_wait_fixture_t *large_wait_feed_fixture;

static void large_wait_fixture_reset(large_wait_fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
}

static void large_wait_uninstall_feed(large_wait_fixture_t *fixture)
{
    if (fixture->hook_installed && fixture->port != NULL) {
        fixture->port->get_queued_completion_status_ex =
            fixture->original_dequeue;
    }
    if (large_wait_feed_fixture == fixture) {
        large_wait_feed_fixture = NULL;
    }
    fixture->hook_installed = 0;
    fixture->original_dequeue = NULL;
}

static void large_wait_fixture_close(large_wait_fixture_t *fixture)
{
    ep_ready_node_t *chain;

    large_wait_uninstall_feed(fixture);
    if (fixture->port != NULL) {
        if (fixture->pending_nodes != NULL) {
            for (size_t i = 0; i < fixture->count; i++) {
                if (fixture->pending_nodes[i] != NULL) {
                    ep_ready_node_free(fixture->port,
                                       fixture->pending_nodes[i]);
                    fixture->pending_nodes[i] = NULL;
                }
            }
        }
        chain = ep_ready_drain(&fixture->port->ready_queue,
                               (int)LARGE_WAIT_EVENT_COUNT);
        while (chain != NULL) {
            ep_ready_node_t *next = atomic_load_explicit(
                &chain->next, memory_order_relaxed);

            ep_ready_node_free(fixture->port, chain);
            chain = next;
        }
        (void)ep_port_destroy(fixture->port);
        fixture->port = NULL;
    }
    free(fixture->pending_nodes);
    fixture->pending_nodes = NULL;
    free(fixture->socks);
    fixture->socks = NULL;
    fixture->count = 0;
}

static int large_wait_table_insert(ep_port_t *port, ep_sock_t *sock)
{
    size_t slot;

    if (port->fd_table_size == 0) {
        return -1;
    }
    slot = (size_t)sock->fd % port->fd_table_size;
    for (size_t probes = 0; probes < port->fd_table_size; probes++) {
        if (port->fd_table[slot] == NULL) {
            port->fd_table[slot] = sock;
            port->fd_table_count++;
            return 0;
        }
        slot = (slot + 1) % port->fd_table_size;
    }
    return -1;
}

static int large_wait_fixture_open(large_wait_fixture_t *fixture)
{
    ep_sock_t *previous = NULL;
    ep_sock_t *previous_oneshot = NULL;

    large_wait_fixture_reset(fixture);
    fixture->count = LARGE_WAIT_EVENT_COUNT;
    fixture->socks = (ep_sock_t **)calloc(
        fixture->count, sizeof(*fixture->socks));
    fixture->pending_nodes = (ep_ready_node_t **)calloc(
        fixture->count, sizeof(*fixture->pending_nodes));
    if (fixture->socks == NULL || fixture->pending_nodes == NULL ||
        ep_port_create((int)fixture->count, 0, &fixture->port) != 0) {
        large_wait_fixture_close(fixture);
        return -1;
    }

    pthread_mutex_lock(&fixture->port->fd_table_lock);
    for (size_t i = 0; i < fixture->count; i++) {
        ep_sock_t *sock = (ep_sock_t *)calloc(1, sizeof(*sock));

        if (sock == NULL) {
            pthread_mutex_unlock(&fixture->port->fd_table_lock);
            large_wait_fixture_close(fixture);
            return -1;
        }
        fixture->socks[i] = sock;
        sock->fd = (SOCKET)(LARGE_WAIT_FD_BASE + i);
        sock->base_socket = sock->fd;
        sock->kind = EP_REG_WAITABLE;
        sock->waitable_semantics = EP_WAITABLE_PERSISTENT;
        sock->user_events = EPOLLIN;
        sock->user_flags = EPOLLONESHOT;
        sock->port = fixture->port;
        sock->generation = i + 1;
        sock->exclusive_claim_base = INVALID_SOCKET;
        atomic_init(&sock->state, EP_SOCK_REGISTERED);
        atomic_init(&sock->poll_status, EP_POLL_IDLE);
        atomic_init(&sock->delete_pending, 0);
        atomic_init(&sock->ready_queued, 0);
        atomic_init(&sock->callback_active, 0);
        atomic_init(&sock->completion_posted, 0);
        atomic_init(&sock->waitable_notification_owned, 0);
        sock->oneshot_fired = 1;

        if (large_wait_table_insert(fixture->port, sock) != 0) {
            free(sock);
            fixture->socks[i] = NULL;
            pthread_mutex_unlock(&fixture->port->fd_table_lock);
            large_wait_fixture_close(fixture);
            ep_set_errno(EIO);
            return -1;
        }

        sock->prev = previous;
        if (previous != NULL) {
            previous->next = sock;
        } else {
            fixture->port->sock_list_head = sock;
        }
        previous = sock;

        sock->oneshot_prev = previous_oneshot;
        if (previous_oneshot != NULL) {
            previous_oneshot->oneshot_next = sock;
        } else {
            fixture->port->oneshot_head = sock;
        }
        previous_oneshot = sock;
        fixture->port->oneshot_tail = sock;
        fixture->port->oneshot_fired_count++;
    }
    fixture->port->next_sock_generation = fixture->count;
    if (!ep_port_worklists_valid_locked(fixture->port)) {
        pthread_mutex_unlock(&fixture->port->fd_table_lock);
        large_wait_fixture_close(fixture);
        ep_set_errno(EIO);
        return -1;
    }
    pthread_mutex_unlock(&fixture->port->fd_table_lock);
    return 0;
}

static uint64_t large_wait_timestamp(size_t index)
{
    return LARGE_WAIT_TIMESTAMP_TAG | (uint64_t)index;
}

static int large_wait_prepare(large_wait_fixture_t *fixture,
                              uint64_t data_base,
                              size_t initial_publish)
{
    if (initial_publish > fixture->count ||
        atomic_load_explicit(&fixture->port->ready_queue.queued,
                             memory_order_relaxed) != 0) {
        ep_set_errno(EIO);
        return -1;
    }
    for (size_t i = 0; i < fixture->count; i++) {
        ep_ready_node_t *node;

        if (fixture->pending_nodes[i] != NULL) {
            ep_set_errno(EIO);
            return -1;
        }
        node = ep_ready_node_alloc(fixture->port);
        if (node == NULL) {
            return -1;
        }
        fixture->pending_nodes[i] = node;
        node->data.u64 = data_base + i;
        node->user_ctx = fixture->socks[i];
        node->fd = fixture->socks[i]->fd;
        node->sock_generation = fixture->socks[i]->generation;
        node->events = EPOLLIN;
        node->flags = WEPOLL_FLAG_ONESHOT_FIRED;
        node->timestamp = large_wait_timestamp(i);
    }

    pthread_mutex_lock(&fixture->port->fd_table_lock);
    for (size_t i = 0; i < fixture->count; i++) {
        ep_sock_t *sock = fixture->socks[i];

        sock->user_data.u64 = data_base + i;
        sock->user_ctx = sock;
        sock->pending_events = EPOLLIN;
        sock->needs_rearm = 0;
        atomic_store_explicit(&sock->state, EP_SOCK_READY,
                              memory_order_relaxed);
        atomic_store_explicit(&sock->ready_queued, 1,
                              memory_order_relaxed);
    }
    pthread_mutex_unlock(&fixture->port->fd_table_lock);

    for (size_t i = 0; i < initial_publish; i++) {
        ep_ready_push(&fixture->port->ready_queue,
                      fixture->pending_nodes[i]);
        fixture->pending_nodes[i] = NULL;
    }
    fixture->feed_index = initial_publish;
    fixture->feed_calls = 0;
    fixture->feed_chunk = LARGE_WAIT_FEED_CHUNK;
    fixture->feed_delay_ms = 0;
    fixture->control = NULL;
    fixture->feed_invalid = 0;
    return 0;
}

static DWORD WINAPI large_wait_control_thread(void *parameter)
{
    large_wait_control_context_t *context =
        (large_wait_control_context_t *)parameter;
    DWORD wait_result;
    int targets_drained;
    int operation_result;

    wait_result = WaitForSingleObject(context->start_event, 5000);
    if (wait_result != WAIT_OBJECT_0) {
        atomic_store_explicit(&context->worker_error, 1,
                              memory_order_release);
        (void)SetEvent(context->done_event);
        return 1;
    }

    atomic_store_explicit(
        &context->saw_waiter_active,
        atomic_load_explicit(&context->port->waiter_active,
                             memory_order_acquire) != 0,
        memory_order_release);
    atomic_store_explicit(
        &context->saw_waiter_coalescing,
        atomic_load_explicit(&context->port->waiter_coalescing,
                             memory_order_acquire) != 0,
        memory_order_release);

    pthread_mutex_lock(&context->port->fd_table_lock);
    targets_drained =
        atomic_load_explicit(&context->modify_sock->state,
                             memory_order_relaxed) == EP_SOCK_REGISTERED &&
        atomic_load_explicit(&context->modify_sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        context->modify_sock->pending_events == 0 &&
        context->modify_sock->oneshot_fired != 0 &&
        atomic_load_explicit(&context->rearm_sock->state,
                             memory_order_relaxed) == EP_SOCK_REGISTERED &&
        atomic_load_explicit(&context->rearm_sock->ready_queued,
                             memory_order_relaxed) == 0 &&
        context->rearm_sock->pending_events == 0 &&
        context->rearm_sock->oneshot_fired != 0;
    pthread_mutex_unlock(&context->port->fd_table_lock);
    atomic_store_explicit(&context->saw_targets_drained, targets_drained,
                          memory_order_release);

    operation_result = ep_port_modify(
        context->port, context->modify_sock->fd,
        EPOLLIN, EPOLLONESHOT,
        context->modify_data, context->modify_user_ctx);
    atomic_store_explicit(&context->modify_result, operation_result,
                          memory_order_release);
    atomic_store_explicit(&context->modify_error,
                          operation_result == 0 ? 0 : ep_last_err(),
                          memory_order_release);

    operation_result = ep_port_rearm(context->port,
                                     context->rearm_sock->fd);
    atomic_store_explicit(&context->rearm_result, operation_result,
                          memory_order_release);
    atomic_store_explicit(&context->rearm_error,
                          operation_result == 0 ? 0 : ep_last_err(),
                          memory_order_release);

    if (!SetEvent(context->done_event)) {
        atomic_store_explicit(&context->worker_error, 1,
                              memory_order_release);
        return 1;
    }
    return 0;
}

static BOOL WINAPI large_wait_dequeue_stub(
    HANDLE completion_port,
    OVERLAPPED_ENTRY *entries,
    ULONG count,
    PULONG removed,
    DWORD milliseconds,
    BOOL alertable)
{
    large_wait_fixture_t *fixture = large_wait_feed_fixture;
    size_t end;

    if (fixture == NULL || fixture->port == NULL ||
        completion_port != fixture->port->iocp || entries == NULL ||
        count == 0 || removed == NULL || milliseconds != 0 || alertable ||
        fixture->feed_index >= fixture->count || fixture->feed_chunk == 0) {
        if (fixture != NULL) fixture->feed_invalid = 1;
        if (removed != NULL) *removed = 0;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (fixture->control != NULL &&
        atomic_exchange_explicit(&fixture->control->triggered, 1,
                                 memory_order_acq_rel) == 0) {
        atomic_store_explicit(&fixture->control->feed_calls_at_trigger,
                              fixture->feed_calls,
                              memory_order_release);
        if (!atomic_load_explicit(&fixture->port->waiter_coalescing,
                                  memory_order_acquire) ||
            !SetEvent(fixture->control->start_event) ||
            WaitForSingleObject(fixture->control->done_event, 5000) !=
                WAIT_OBJECT_0) {
            fixture->feed_invalid = 1;
            *removed = 0;
            SetLastError(ERROR_TIMEOUT);
            return FALSE;
        }
    }

    end = fixture->feed_index + fixture->feed_chunk;
    if (end > fixture->count) end = fixture->count;
    for (size_t i = fixture->feed_index; i < end; i++) {
        if (fixture->pending_nodes[i] == NULL) {
            fixture->feed_invalid = 1;
            *removed = 0;
            SetLastError(ERROR_INVALID_DATA);
            return FALSE;
        }
    }
    for (size_t i = fixture->feed_index; i < end; i++) {
        ep_ready_push(&fixture->port->ready_queue,
                      fixture->pending_nodes[i]);
        fixture->pending_nodes[i] = NULL;
    }
    fixture->feed_index = end;
    fixture->feed_calls++;
    if (fixture->feed_delay_ms != 0) {
        Sleep(fixture->feed_delay_ms);
    }
    memset(&entries[0], 0, sizeof(entries[0]));
    *removed = 1;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

static int large_wait_install_feed(large_wait_fixture_t *fixture)
{
    if (large_wait_feed_fixture != NULL || fixture->hook_installed ||
        fixture->feed_index >= fixture->count) {
        ep_set_errno(EIO);
        return -1;
    }
    fixture->original_dequeue =
        fixture->port->get_queued_completion_status_ex;
    fixture->port->get_queued_completion_status_ex =
        large_wait_dequeue_stub;
    fixture->hook_installed = 1;
    large_wait_feed_fixture = fixture;
    return 0;
}

static int large_wait_publish_pending(large_wait_fixture_t *fixture)
{
    for (size_t i = fixture->feed_index; i < fixture->count; i++) {
        if (fixture->pending_nodes[i] == NULL) {
            ep_set_errno(EIO);
            return -1;
        }
    }
    for (size_t i = fixture->feed_index; i < fixture->count; i++) {
        ep_ready_push(&fixture->port->ready_queue,
                      fixture->pending_nodes[i]);
        fixture->pending_nodes[i] = NULL;
    }
    fixture->feed_index = fixture->count;
    return 0;
}

static int large_wait_validate_extended(
    const large_wait_fixture_t *fixture,
    const epoll_event_ex *events,
    size_t event_count,
    uint64_t data_base)
{
    unsigned char *seen = (unsigned char *)calloc(fixture->count, 1);
    int result = -1;

    if (seen == NULL) return -1;
    for (size_t i = 0; i < event_count; i++) {
        uint64_t value = events[i].data.u64;
        size_t index;

        if (value < data_base || value - data_base >= fixture->count) {
            goto cleanup;
        }
        index = (size_t)(value - data_base);
        if (seen[index] || events[i].events != EPOLLIN ||
            events[i].flags != WEPOLL_FLAG_ONESHOT_FIRED ||
            events[i].timestamp != large_wait_timestamp(index) ||
            events[i].user_ctx != fixture->socks[index]) {
            goto cleanup;
        }
        seen[index] = 1;
    }
    result = 0;

cleanup:
    free(seen);
    if (result != 0) ep_set_errno(EIO);
    return result;
}

static int large_wait_all_drained(large_wait_fixture_t *fixture)
{
    int valid = atomic_load_explicit(&fixture->port->ready_queue.queued,
                                     memory_order_relaxed) == 0;

    pthread_mutex_lock(&fixture->port->fd_table_lock);
    for (size_t i = 0; valid && i < fixture->count; i++) {
        ep_sock_t *sock = fixture->socks[i];

        valid = atomic_load_explicit(&sock->state,
                                     memory_order_relaxed) ==
                    EP_SOCK_REGISTERED &&
            atomic_load_explicit(&sock->ready_queued,
                                 memory_order_relaxed) == 0 &&
            sock->pending_events == 0 && sock->needs_rearm == 0 &&
            sock->oneshot_fired != 0;
    }
    valid = valid && ep_port_worklists_valid_locked(fixture->port);
    pthread_mutex_unlock(&fixture->port->fd_table_lock);
    return valid;
}

static int large_wait_guard_unchanged(const struct epoll_event *guard)
{
    const unsigned char *bytes = (const unsigned char *)guard;

    for (size_t i = 0; i < sizeof(*guard); i++) {
        if (bytes[i] != 0xa5U) return 0;
    }
    return 1;
}

static int test_large_wait(void)
{
    static const uint64_t coalesced_base =
        UINT64_C(0x4c57010000000000);
    static const uint64_t residue_base =
        UINT64_C(0x4c57020000000000);
    static const uint64_t basic_base =
        UINT64_C(0x4c57030000000000);
    static const uint64_t budget_base =
        UINT64_C(0x4c57040000000000);
    static const uint64_t control_base =
        UINT64_C(0x4c57050000000000);
    static const uint64_t modified_data =
        UINT64_C(0x4c57f00000000000);
    large_wait_fixture_t fixture;
    large_wait_control_context_t control;
    epoll_event_ex *extended = NULL;
    struct epoll_event *basic_storage = NULL;
    struct epoll_event *basic;
    ep_wait_timeout_t timeout;
    HANDLE control_thread = NULL;
    DWORD control_thread_exit = 1;
    uint64_t modify_generation;
    uint64_t rearm_generation;
    uint64_t rearm_visits;
    uint64_t oneshot_visits;
    size_t queued;
    size_t budget_first_count;
    int state_ok;
    int wait_result;
    int result = -1;

    large_wait_fixture_reset(&fixture);
    memset(&control, 0, sizeof(control));
    atomic_init(&control.triggered, 0);
    atomic_init(&control.feed_calls_at_trigger, 0);
    atomic_init(&control.worker_error, 0);
    atomic_init(&control.saw_waiter_active, 0);
    atomic_init(&control.saw_waiter_coalescing, 0);
    atomic_init(&control.saw_targets_drained, 0);
    atomic_init(&control.modify_result, -2);
    atomic_init(&control.modify_error, 0);
    atomic_init(&control.rearm_result, -2);
    atomic_init(&control.rearm_error, 0);
    extended = (epoll_event_ex *)calloc(
        LARGE_WAIT_EVENT_COUNT, sizeof(*extended));
    basic_storage = (struct epoll_event *)malloc(
        (LARGE_WAIT_EVENT_COUNT + 2U) * sizeof(*basic_storage));
    if (extended == NULL || basic_storage == NULL ||
        large_wait_fixture_open(&fixture) != 0) {
        goto cleanup;
    }

    if (large_wait_prepare(&fixture, coalesced_base,
                           LARGE_WAIT_FEED_CHUNK) != 0 ||
        large_wait_install_feed(&fixture) != 0) {
        goto cleanup;
    }
    memset(extended, 0,
           LARGE_WAIT_EVENT_COUNT * sizeof(*extended));
    wait_result = ep_port_wait(fixture.port, extended,
                               (int)LARGE_WAIT_EVENT_COUNT, 2000, NULL);
    large_wait_uninstall_feed(&fixture);
    if (wait_result != (int)LARGE_WAIT_EVENT_COUNT ||
        fixture.feed_index != fixture.count || fixture.feed_calls < 2 ||
        fixture.feed_invalid ||
        large_wait_validate_extended(&fixture, extended,
                                     fixture.count,
                                     coalesced_base) != 0 ||
        !large_wait_all_drained(&fixture)) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    if (large_wait_prepare(&fixture, residue_base,
                           fixture.count) != 0) {
        goto cleanup;
    }
    memset(extended, 0,
           LARGE_WAIT_EVENT_COUNT * sizeof(*extended));
    wait_result = ep_port_wait(fixture.port, extended, 4096, 0, NULL);
    queued = atomic_load_explicit(&fixture.port->ready_queue.queued,
                                  memory_order_relaxed);
    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = queued == 1;
    for (size_t i = 0; state_ok && i < fixture.count; i++) {
        int expected_ready = i == fixture.count - 1;
        ep_sock_t *sock = fixture.socks[i];

        state_ok =
            (atomic_load_explicit(&sock->ready_queued,
                                  memory_order_relaxed) != 0) ==
                expected_ready &&
            atomic_load_explicit(&sock->state,
                                 memory_order_relaxed) ==
                (expected_ready ? EP_SOCK_READY : EP_SOCK_REGISTERED) &&
            sock->pending_events ==
                (expected_ready ? EPOLLIN : 0);
    }
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (wait_result != 4096 || !state_ok ||
        large_wait_validate_extended(&fixture, extended, 4096,
                                     residue_base) != 0) {
        ep_set_errno(EIO);
        goto cleanup;
    }
    memset(&extended[0], 0, sizeof(extended[0]));
    if (ep_port_wait(fixture.port, &extended[0], 1, 0, NULL) != 1 ||
        extended[0].events != EPOLLIN ||
        extended[0].data.u64 !=
            residue_base + LARGE_WAIT_EVENT_COUNT - 1U ||
        extended[0].flags != WEPOLL_FLAG_ONESHOT_FIRED ||
        extended[0].timestamp !=
            large_wait_timestamp(LARGE_WAIT_EVENT_COUNT - 1U) ||
        extended[0].user_ctx !=
            fixture.socks[LARGE_WAIT_EVENT_COUNT - 1U] ||
        !large_wait_all_drained(&fixture)) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    if (large_wait_prepare(&fixture, budget_base, 1) != 0) {
        goto cleanup;
    }
    fixture.feed_chunk = 1;
    fixture.feed_delay_ms = 1;
    if (large_wait_install_feed(&fixture) != 0) {
        goto cleanup;
    }
    memset(extended, 0,
           LARGE_WAIT_EVENT_COUNT * sizeof(*extended));
    wait_result = ep_port_wait(fixture.port, extended,
                               (int)LARGE_WAIT_EVENT_COUNT, 2000, NULL);
    large_wait_uninstall_feed(&fixture);
    if (wait_result != (int)(1U + LARGE_WAIT_BUDGET_DEQUEUES) ||
        fixture.feed_calls != LARGE_WAIT_BUDGET_DEQUEUES ||
        fixture.feed_index != 1U + LARGE_WAIT_BUDGET_DEQUEUES ||
        fixture.feed_invalid ||
        large_wait_validate_extended(&fixture, extended,
                                     (size_t)wait_result,
                                     budget_base) != 0) {
        ep_set_errno(EIO);
        goto cleanup;
    }
    budget_first_count = (size_t)wait_result;
    if (large_wait_publish_pending(&fixture) != 0) {
        goto cleanup;
    }
    wait_result = ep_port_wait(
        fixture.port, &extended[budget_first_count],
        (int)(fixture.count - budget_first_count), 0, NULL);
    if (wait_result != (int)(fixture.count - budget_first_count) ||
        large_wait_validate_extended(&fixture, extended, fixture.count,
                                     budget_base) != 0 ||
        !large_wait_all_drained(&fixture)) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    if (sizeof(struct epoll_event) != 12U ||
        offsetof(struct epoll_event, data) != 4U) {
        ep_set_errno(EIO);
        goto cleanup;
    }
    if (large_wait_prepare(&fixture, basic_base,
                           LARGE_WAIT_FEED_CHUNK) != 0 ||
        large_wait_install_feed(&fixture) != 0) {
        goto cleanup;
    }
    memset(basic_storage, 0xa5,
           (LARGE_WAIT_EVENT_COUNT + 2U) * sizeof(*basic_storage));
    basic = &basic_storage[1];
    ep_wait_timeout_from_milliseconds(2000, &timeout);
    wait_result = ep_port_wait_basic_timeout(
        fixture.port, basic, (int)LARGE_WAIT_EVENT_COUNT, &timeout, NULL);
    large_wait_uninstall_feed(&fixture);
    if (wait_result != (int)LARGE_WAIT_EVENT_COUNT ||
        fixture.feed_index != fixture.count || fixture.feed_calls < 2 ||
        fixture.feed_invalid ||
        !large_wait_guard_unchanged(&basic_storage[0]) ||
        !large_wait_guard_unchanged(
            &basic_storage[LARGE_WAIT_EVENT_COUNT + 1U]) ||
        basic[4095].events != EPOLLIN ||
        basic[4095].data.u64 != basic_base + 4095U ||
        basic[4096].events != EPOLLIN ||
        basic[4096].data.u64 != basic_base + 4096U ||
        !large_wait_all_drained(&fixture)) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    if (large_wait_prepare(&fixture, control_base,
                           LARGE_WAIT_CONTROL_INITIAL) != 0) {
        goto cleanup;
    }
    control.port = fixture.port;
    control.modify_sock = fixture.socks[0];
    control.rearm_sock = fixture.socks[1];
    control.modify_data.u64 = modified_data;
    control.modify_user_ctx = &control;
    control.start_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    control.done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (control.start_event == NULL || control.done_event == NULL) {
        goto cleanup;
    }
    pthread_mutex_lock(&fixture.port->fd_table_lock);
    modify_generation = control.modify_sock->generation;
    rearm_generation = control.rearm_sock->generation;
    rearm_visits = fixture.port->rearm_work_visits;
    oneshot_visits = fixture.port->oneshot_probe_visits;
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    control_thread = CreateThread(NULL, 0, large_wait_control_thread,
                                  &control, 0, NULL);
    if (control_thread == NULL) {
        goto cleanup;
    }
    fixture.control = &control;
    if (large_wait_install_feed(&fixture) != 0) {
        goto cleanup;
    }
    memset(extended, 0,
           LARGE_WAIT_EVENT_COUNT * sizeof(*extended));
    wait_result = ep_port_wait(fixture.port, extended,
                               (int)LARGE_WAIT_EVENT_COUNT, 2000, NULL);
    large_wait_uninstall_feed(&fixture);
    fixture.control = NULL;
    if (WaitForSingleObject(control_thread, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(control_thread, &control_thread_exit)) {
        ep_set_errno(EIO);
        goto cleanup;
    }
    CloseHandle(control_thread);
    control_thread = NULL;
    CloseHandle(control.done_event);
    control.done_event = NULL;
    CloseHandle(control.start_event);
    control.start_event = NULL;

    if (wait_result != (int)fixture.count ||
        fixture.feed_index != fixture.count || fixture.feed_calls < 2 ||
        fixture.feed_invalid || control_thread_exit != 0 ||
        atomic_load_explicit(&control.triggered,
                             memory_order_acquire) != 1 ||
        atomic_load_explicit(&control.feed_calls_at_trigger,
                             memory_order_acquire) != 0 ||
        atomic_load_explicit(&control.worker_error,
                             memory_order_acquire) != 0 ||
        atomic_load_explicit(&control.saw_waiter_active,
                             memory_order_acquire) == 0 ||
        atomic_load_explicit(&control.saw_waiter_coalescing,
                             memory_order_acquire) == 0 ||
        atomic_load_explicit(&control.saw_targets_drained,
                             memory_order_acquire) == 0 ||
        atomic_load_explicit(&control.modify_result,
                             memory_order_acquire) != 0 ||
        atomic_load_explicit(&control.modify_error,
                             memory_order_acquire) != 0 ||
        atomic_load_explicit(&control.rearm_result,
                             memory_order_acquire) != 0 ||
        atomic_load_explicit(&control.rearm_error,
                             memory_order_acquire) != 0 ||
        large_wait_validate_extended(&fixture, extended, fixture.count,
                                     control_base) != 0) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    queued = atomic_load_explicit(&fixture.port->ready_queue.queued,
                                  memory_order_relaxed);
    pthread_mutex_lock(&fixture.port->fd_table_lock);
    state_ok = queued == 0 && fixture.port->pending_poll_count == 0 &&
        fixture.port->needs_rearm_count == 2 &&
        fixture.port->rearm_head == control.modify_sock &&
        fixture.port->rearm_tail == control.rearm_sock &&
        control.modify_sock->rearm_prev == NULL &&
        control.modify_sock->rearm_next == control.rearm_sock &&
        control.rearm_sock->rearm_prev == control.modify_sock &&
        control.rearm_sock->rearm_next == NULL &&
        fixture.port->oneshot_fired_count == fixture.count - 2U &&
        fixture.port->rearm_work_visits == rearm_visits &&
        fixture.port->oneshot_probe_visits == oneshot_visits &&
        control.modify_sock->generation != modify_generation &&
        control.rearm_sock->generation != rearm_generation &&
        control.modify_sock->user_events == EPOLLIN &&
        control.modify_sock->user_flags == EPOLLONESHOT &&
        control.modify_sock->user_data.u64 == modified_data &&
        control.modify_sock->user_ctx == &control;
    for (size_t i = 0; state_ok && i < fixture.count; i++) {
        ep_sock_t *sock = fixture.socks[i];
        int controlled = i < LARGE_WAIT_CONTROL_INITIAL;

        state_ok =
            atomic_load_explicit(&sock->state,
                                 memory_order_relaxed) ==
                EP_SOCK_REGISTERED &&
            atomic_load_explicit(&sock->poll_status,
                                 memory_order_relaxed) == EP_POLL_IDLE &&
            atomic_load_explicit(&sock->ready_queued,
                                 memory_order_relaxed) == 0 &&
            atomic_load_explicit(&sock->callback_active,
                                 memory_order_relaxed) == 0 &&
            atomic_load_explicit(&sock->completion_posted,
                                 memory_order_relaxed) == 0 &&
            sock->wait_registration == NULL && sock->pending_events == 0 &&
            (sock->needs_rearm != 0) == controlled &&
            (sock->oneshot_fired == 0) == controlled;
    }
    state_ok = state_ok && ep_port_worklists_valid_locked(fixture.port);
    pthread_mutex_unlock(&fixture.port->fd_table_lock);
    if (!state_ok ||
        atomic_load_explicit(&fixture.port->waiter_active,
                             memory_order_acquire) != 0 ||
        atomic_load_explicit(&fixture.port->waiter_coalescing,
                             memory_order_acquire) != 0) {
        ep_set_errno(EIO);
        goto cleanup;
    }

    result = 0;

cleanup:
    fixture.control = NULL;
    large_wait_uninstall_feed(&fixture);
    if (control.start_event != NULL) {
        (void)SetEvent(control.start_event);
    }
    if (control_thread != NULL) {
        if (WaitForSingleObject(control_thread, 5000) != WAIT_OBJECT_0) {
            (void)TerminateThread(control_thread, 1);
            (void)WaitForSingleObject(control_thread, 5000);
            result = -1;
        }
        CloseHandle(control_thread);
    }
    if (control.done_event != NULL) CloseHandle(control.done_event);
    if (control.start_event != NULL) CloseHandle(control.start_event);
    large_wait_fixture_close(&fixture);
    free(basic_storage);
    free(extended);
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
    } else if (strcmp(argv[1], "udp-readless-error") == 0) {
        result = test_udp_readless_error();
    } else if (strcmp(argv[1], "udp-readless-park") == 0) {
        result = test_udp_readless_park();
    } else if (strcmp(argv[1], "udp-readless-rollback") == 0) {
        result = test_udp_readless_rollback();
    } else if (strcmp(argv[1], "socket-add-submit-failure") == 0) {
        result = test_socket_add_submit_failure();
    } else if (strcmp(argv[1], "socket-add-immediate-success") == 0) {
        result = test_socket_add_immediate_success();
    } else if (strcmp(argv[1], "aux-add-lazy") == 0) {
        result = test_aux_add_lazy();
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
    } else if (strcmp(argv[1], "pending-expansion-ready-race") == 0) {
        result = test_pending_expansion_ready_race();
    } else if (strcmp(argv[1], "tcp-current-rdhup") == 0) {
        result = test_tcp_current_rdhup();
    } else if (strcmp(argv[1], "tcp-info-runtime") == 0) {
        result = test_tcp_info_runtime();
    } else if (strcmp(argv[1], "transitional-idle") == 0) {
        result = test_transitional_idle();
    } else if (strcmp(argv[1], "aux-posted-cancel") == 0) {
        result = test_aux_posted_cancel();
    } else if (strcmp(argv[1], "waitable-zero-callback") == 0) {
        result = test_waitable_zero_callback();
    } else if (strcmp(argv[1], "waitable-zero-ready") == 0) {
        result = test_waitable_zero_ready();
    } else if (strcmp(argv[1], "waitable-queued-rearm") == 0) {
        result = test_waitable_queued_rearm();
    } else if (strcmp(argv[1], "aux-post-close-lease") == 0) {
        result = test_aux_post_close_lease();
    } else if (strcmp(argv[1], "pipe-invalid-et") == 0) {
        result = test_pipe_invalid_et_snapshot();
    } else if (strcmp(argv[1], "large-wait") == 0) {
        result = test_large_wait();
    } else if (strcmp(argv[1], "exclusive-mixed-submit") == 0) {
        result = test_exclusive_mixed_submit();
    }
    (void)WSACleanup();

    if (result == 77) {
        return 77;
    }
    if (result != 0) {
        fprintf(stderr, "state mode failed: %s (errno=%d)\n",
                argc > 1 ? argv[1] : "missing", errno);
        return 1;
    }
    return 0;
}
