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

#define TEST_FILE_PIPE_LOCAL_INFORMATION_CLASS 24U
#define TEST_STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)

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
    if (ep_afd_poll_submit(&sock, AFD_POLL_RECEIVE, NULL) == 0 &&
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
        fixture.port->rearm_head == sock &&
        fixture.port->rearm_tail == sock &&
        ep_port_worklists_valid_locked(fixture.port) &&
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
    }
    (void)WSACleanup();

    if (result != 0) {
        fprintf(stderr, "state mode failed: %s (errno=%d)\n",
                argc > 1 ? argv[1] : "missing", errno);
        return 1;
    }
    return 0;
}
