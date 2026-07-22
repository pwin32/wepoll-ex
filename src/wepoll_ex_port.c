/*
 * wepoll_ex_port.c -- Windows IOCP/AFD port and socket lifecycle.
 *
 * All mutable socket state is serialized by fd_table_lock.  AFD requests
 * keep ep_sock_t storage alive until their IOCP completion is observed;
 * deletion therefore removes a socket from public lookup immediately but
 * defers reclamation while a poll is pending.  Ready nodes contain immutable
 * registration snapshots plus a socket generation, never a reclaimable raw
 * socket pointer.
 */
#include "wepoll_ex_internal.h"

#include <stdlib.h>
#include <string.h>

#define EP_CLOSE_DRAIN_TIMEOUT_MS UINT64_C(5000)
#define EP_CLOSE_DRAIN_SLICE_MS   100U

/* ------------------------------------------------------------------------- */
/* Time and fd-table helpers.                                                */
/* ------------------------------------------------------------------------- */

static uint64_t ep_now_ns(void)
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    uint64_t seconds;
    uint64_t remainder;

    if (!QueryPerformanceFrequency(&frequency)) {
        return 0;
    }
    if (!QueryPerformanceCounter(&counter) || frequency.QuadPart <= 0) {
        return 0;
    }

    seconds = (uint64_t)(counter.QuadPart / frequency.QuadPart);
    remainder = (uint64_t)(counter.QuadPart % frequency.QuadPart);
    return seconds * UINT64_C(1000000000) +
           (remainder * UINT64_C(1000000000)) /
               (uint64_t)frequency.QuadPart;
}

static int ep_fd_table_grow(ep_port_t *port, size_t new_size)
{
    ep_sock_t **new_table;

    if (new_size == 0 || new_size > SIZE_MAX / sizeof(*new_table)) {
        ep_set_errno(ENOMEM);
        return -1;
    }
    new_table = (ep_sock_t **)calloc(new_size, sizeof(*new_table));
    if (new_table == NULL) {
        ep_set_errno(ENOMEM);
        return -1;
    }

    for (size_t i = 0; i < port->fd_table_size; i++) {
        ep_sock_t *sock = port->fd_table[i];
        if (sock != NULL) {
            size_t slot = (size_t)sock->fd % new_size;
            while (new_table[slot] != NULL) {
                slot = (slot + 1) % new_size;
            }
            new_table[slot] = sock;
        }
    }

    free(port->fd_table);
    port->fd_table = new_table;
    port->fd_table_size = new_size;
    return 0;
}

static ep_sock_t *ep_fd_table_lookup(ep_port_t *port, SOCKET fd)
{
    if (port->fd_table_size == 0) {
        return NULL;
    }

    size_t slot = (size_t)fd % port->fd_table_size;
    for (size_t probes = 0; probes < port->fd_table_size; probes++) {
        ep_sock_t *sock = port->fd_table[slot];
        if (sock == NULL) {
            return NULL;
        }
        if (sock->fd == fd) {
            return sock;
        }
        slot = (slot + 1) % port->fd_table_size;
    }
    return NULL;
}

static int ep_fd_table_insert(ep_port_t *port, ep_sock_t *sock)
{
    if (port->fd_table_size == 0 ||
        port->fd_table_count * 4 >= port->fd_table_size * 3) {
        size_t new_size;

        if (port->fd_table_size == 0) {
            new_size = WEPOLL_INITIAL_FDS;
        } else {
            if (port->fd_table_size > SIZE_MAX / 2) {
                ep_set_errno(ENOMEM);
                return -1;
            }
            new_size = port->fd_table_size * 2;
        }
        if (ep_fd_table_grow(port, new_size) != 0) {
            return -1;
        }
    }

    size_t slot = (size_t)sock->fd % port->fd_table_size;
    while (port->fd_table[slot] != NULL) {
        slot = (slot + 1) % port->fd_table_size;
    }
    port->fd_table[slot] = sock;
    port->fd_table_count++;
    return 0;
}

static void ep_fd_table_remove(ep_port_t *port, ep_sock_t *sock)
{
    if (port->fd_table_size == 0) {
        return;
    }

    size_t slot = (size_t)sock->fd % port->fd_table_size;
    while (port->fd_table[slot] != sock) {
        if (port->fd_table[slot] == NULL) {
            return;
        }
        slot = (slot + 1) % port->fd_table_size;
    }

    port->fd_table[slot] = NULL;
    port->fd_table_count--;

    for (;;) {
        size_t scan = (slot + 1) % port->fd_table_size;
        ep_sock_t *moved = port->fd_table[scan];
        if (moved == NULL) {
            break;
        }

        port->fd_table[scan] = NULL;
        size_t target = (size_t)moved->fd % port->fd_table_size;
        while (port->fd_table[target] != NULL) {
            target = (target + 1) % port->fd_table_size;
        }
        port->fd_table[target] = moved;
        slot = scan;
    }
}

/* ------------------------------------------------------------------------- */
/* Socket allocation and poll lifecycle.                                    */
/* ------------------------------------------------------------------------- */

static void ep_sock_list_add_locked(ep_port_t *port, ep_sock_t *sock)
{
    sock->prev = NULL;
    sock->next = port->sock_list_head;
    if (port->sock_list_head != NULL) {
        port->sock_list_head->prev = sock;
    }
    port->sock_list_head = sock;
}

static void ep_sock_list_remove_locked(ep_port_t *port, ep_sock_t *sock)
{
    if (sock->prev != NULL) {
        sock->prev->next = sock->next;
    } else {
        port->sock_list_head = sock->next;
    }
    if (sock->next != NULL) {
        sock->next->prev = sock->prev;
    }
    sock->next = NULL;
    sock->prev = NULL;
}

#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
typedef enum ep_identity_check {
    EP_IDENTITY_ERROR = -1,
    EP_IDENTITY_MATCH = 0,
    EP_IDENTITY_STALE,
    EP_IDENTITY_CLOSED
} ep_identity_check_t;

static int ep_socket_identity_is_stable(SOCKET fd)
{
    int socket_type = 0;
    int option_length = (int)sizeof(socket_type);

    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, (char *)&socket_type,
                   &option_length) == SOCKET_ERROR) {
        return 0;
    }
    if (socket_type != SOCK_STREAM) {
        return 1;
    }

    {
        int accepting = 0;
        option_length = (int)sizeof(accepting);
        if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, (char *)&accepting,
                       &option_length) == 0 && accepting != 0) {
            return 1;
        }
    }

    {
        struct sockaddr_storage peer;
        int peer_length = (int)sizeof(peer);
        return getpeername(fd, (struct sockaddr *)&peer, &peer_length) == 0;
    }
}

static ep_identity_check_t ep_sock_validate_identity_locked(
    ep_sock_t *sock, int allow_transition)
{
    uint64_t endpoint_id;
    int identity_result;

    if (sock->endpoint_id_state == EP_SOCKET_ID_UNAVAILABLE) {
        return EP_IDENTITY_MATCH;
    }

    identity_result = ep_socket_get_endpoint_id(sock->fd, &endpoint_id);
    if (identity_result == 0) {
        /* A provider that never exposed an ALE token remains supported with
         * legacy numeric-handle semantics.  Losing a token that was already
         * observed is not a safe identity match. */
        ep_set_errno(EIO);
        return EP_IDENTITY_ERROR;
    }
    if (identity_result < 0) {
        int error = ep_last_err();
        if (error == 0) {
            error = EIO;
            ep_set_errno(error);
        }
        if (error == ENOTSOCK || error == EBADF) {
            return EP_IDENTITY_CLOSED;
        }
        return EP_IDENTITY_ERROR;
    }

    if (endpoint_id == sock->endpoint_id) {
        if (sock->endpoint_id_state == EP_SOCKET_ID_TRANSITIONAL &&
            ep_socket_identity_is_stable(sock->fd)) {
            sock->endpoint_id_state = EP_SOCKET_ID_STABLE;
        }
        return EP_IDENTITY_MATCH;
    }

    if (sock->endpoint_id_state == EP_SOCKET_ID_TRANSITIONAL &&
        allow_transition) {
        /* An AFD completion submitted before connect is evidence that this
         * registration survived the legitimate ALE endpoint transition.
         * Control-path mismatches have no such evidence and are treated as
         * native numeric-handle reuse instead. */
        sock->endpoint_id = endpoint_id;
        if (ep_socket_identity_is_stable(sock->fd)) {
            sock->endpoint_id_state = EP_SOCKET_ID_STABLE;
        }
        return EP_IDENTITY_MATCH;
    }

    return EP_IDENTITY_STALE;
}
#endif

static ep_sock_t *ep_sock_alloc_locked(ep_port_t *port, SOCKET fd)
{
    ep_sock_t *sock = (ep_sock_t *)calloc(1, sizeof(*sock));
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    int identity_result;
#endif
    if (sock == NULL) {
        ep_set_errno(ENOMEM);
        return NULL;
    }

    sock->base_socket = ep_socket_get_base(fd);
    if (sock->base_socket == INVALID_SOCKET) {
        free(sock);
        return NULL;
    }

#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    identity_result = ep_socket_get_endpoint_id(fd, &sock->endpoint_id);
    if (identity_result < 0) {
        free(sock);
        return NULL;
    }
    if (identity_result == 0) {
        sock->endpoint_id_state = EP_SOCKET_ID_UNAVAILABLE;
    } else if (ep_socket_identity_is_stable(fd)) {
        sock->endpoint_id_state = EP_SOCKET_ID_STABLE;
    } else {
        sock->endpoint_id_state = EP_SOCKET_ID_TRANSITIONAL;
    }
#endif
    sock->afd_info = (AFD_POLL_INFO *)ep_afd_pool_take(&port->afd_info_pool);
    if (sock->afd_info == NULL) {
        free(sock);
        return NULL;
    }

    sock->fd = fd;
    sock->port = port;
    sock->generation = ++port->next_sock_generation;
    if (port->next_sock_generation == 0) {
        port->next_sock_generation = 1;
        sock->generation = 1;
    }
    atomic_init(&sock->state, EP_SOCK_REGISTERED);
    atomic_init(&sock->poll_status, EP_POLL_IDLE);
    atomic_init(&sock->delete_pending, 0);
    atomic_init(&sock->ready_queued, 0);
    return sock;
}

static void ep_sock_free_locked(ep_port_t *port, ep_sock_t *sock)
{
    ep_sock_list_remove_locked(port, sock);
    if (sock->afd_info != NULL) {
        ep_afd_pool_give(&port->afd_info_pool, sock->afd_info);
        sock->afd_info = NULL;
    }
    free(sock);
}

/* A registered socket may be closed by the application without an explicit
 * EPOLL_CTL_DEL.  Once AFD reports that the provider handle is gone, remove
 * the public registration but keep any in-flight request alive until its
 * completion has been consumed. */
static void ep_sock_drop_closed_locked(ep_port_t *port, ep_sock_t *sock)
{
    if (ep_fd_table_lookup(port, sock->fd) == sock) {
        ep_fd_table_remove(port, sock);
    }
    atomic_store_explicit(&sock->delete_pending, 1, memory_order_relaxed);
    atomic_store_explicit(&sock->state, EP_SOCK_DELETED,
                          memory_order_relaxed);
    sock->needs_rearm = 0;
    sock->generation = ++port->next_sock_generation;
    if (port->next_sock_generation == 0) {
        port->next_sock_generation = 1;
        sock->generation = 1;
    }
    if (atomic_load_explicit(&sock->poll_status, memory_order_relaxed) ==
        EP_POLL_IDLE) {
        ep_sock_free_locked(port, sock);
    }
}

#ifdef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
#define EP_SOCK_SUBMIT_LOCKED(sock, identity_validated) \
    ep_sock_submit_locked(sock)
static int ep_sock_submit_locked(ep_sock_t *sock)
#else
#define EP_SOCK_SUBMIT_LOCKED(sock, identity_validated) \
    ep_sock_submit_locked((sock), (identity_validated))
static int ep_sock_submit_locked(ep_sock_t *sock, int identity_validated)
#endif
{
    ep_port_t *port = sock->port;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    ep_identity_check_t identity_check;
#endif
    uint32_t poll_status =
        atomic_load_explicit(&sock->poll_status, memory_order_relaxed);

    if (atomic_load_explicit(&port->closing, memory_order_acquire) ||
        atomic_load_explicit(&sock->delete_pending, memory_order_relaxed) ||
        atomic_load_explicit(&sock->ready_queued, memory_order_relaxed) ||
        poll_status != EP_POLL_IDLE) {
        return 0;
    }

#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    if (!identity_validated) {
        identity_check = ep_sock_validate_identity_locked(sock, 0);
        if (identity_check == EP_IDENTITY_ERROR) {
            return -1;
        }
        if (identity_check == EP_IDENTITY_STALE ||
            identity_check == EP_IDENTITY_CLOSED) {
            int error = identity_check == EP_IDENTITY_STALE
                ? ENOENT : ENOTSOCK;
            ep_sock_drop_closed_locked(port, sock);
            ep_set_errno(error);
            return 1;
        }
    }
#endif

    if (sock->oneshot_fired) {
        /* No AFD request remains after a oneshot delivery.  Probe the
         * provider handle so a native closesocket() cannot leave a stale
         * registration forever. */
        SOCKET base = ep_socket_get_base(sock->fd);
        if (base == INVALID_SOCKET) {
            if (ep_last_err() == ENOTSOCK || ep_last_err() == EBADF) {
                ep_sock_drop_closed_locked(port, sock);
                ep_set_errno(ENOTSOCK);
                return 1;
            }
            return -1;
        }
        sock->base_socket = base;
        return 0;
    }
    if (!sock->needs_rearm) {
        return 0;
    }

    uint32_t afd_events = ep_epoll_to_afd_events(sock->user_events);
    if (ep_afd_poll_submit(sock, afd_events) != 0) {
        if (ep_last_err() == ENOTSOCK || ep_last_err() == EBADF) {
            ep_sock_drop_closed_locked(port, sock);
            ep_set_errno(ENOTSOCK);
            return 1;
        }
        return -1;
    }

    sock->needs_rearm = 0;
    atomic_store_explicit(&sock->poll_status, EP_POLL_PENDING,
                          memory_order_relaxed);
    atomic_store_explicit(&sock->state, EP_SOCK_POLLING,
                          memory_order_relaxed);
    port->pending_poll_count++;
    return 0;
}

static int ep_sock_cancel_locked(ep_sock_t *sock)
{
    if (atomic_load_explicit(&sock->poll_status, memory_order_relaxed) !=
        EP_POLL_PENDING) {
        return 0;
    }
    if (ep_afd_cancel(sock) != 0) {
        return -1;
    }

    sock->pending_events = 0;
    atomic_store_explicit(&sock->poll_status, EP_POLL_CANCELLED,
                          memory_order_relaxed);
    return 0;
}

#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
static void ep_sock_retire_stale_locked(ep_port_t *port, ep_sock_t *sock)
{
    int saved_errno = ep_last_err();

    if (atomic_load_explicit(&sock->poll_status, memory_order_relaxed) ==
        EP_POLL_PENDING) {
        (void)ep_sock_cancel_locked(sock);
    }
    atomic_fetch_add_explicit(&port->generation, 1, memory_order_relaxed);
    ep_sock_drop_closed_locked(port, sock);
    ep_set_errno(saved_errno);
}
#endif

/* Return 0 for the registered socket, 1 after retiring a stale/closed
 * numeric-handle entry, and -1 for an identity-query failure. */
static int ep_sock_validate_control_locked(ep_port_t *port, ep_sock_t *sock)
{
#ifdef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    (void)port;
    (void)sock;
    return 0;
#else
    ep_identity_check_t identity_check =
        ep_sock_validate_identity_locked(sock, 0);

    if (identity_check == EP_IDENTITY_MATCH) {
        return 0;
    }
    if (identity_check == EP_IDENTITY_ERROR) {
        return -1;
    }

    {
        int error = identity_check == EP_IDENTITY_STALE
            ? ENOENT : ENOTSOCK;
        ep_sock_retire_stale_locked(port, sock);
        ep_set_errno(error);
    }
    return 1;
#endif
}

static int ep_port_arm_pending_locked(ep_port_t *port)
{
    for (ep_sock_t *sock = port->sock_list_head;
         sock != NULL;) {
        ep_sock_t *next = sock->next;
        int submit_result = EP_SOCK_SUBMIT_LOCKED(sock, 0);
        if (submit_result < 0) {
            return -1;
        }
        sock = next;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* IOCP completion handling.                                                */
/* ------------------------------------------------------------------------- */

void ep_sock_handle_completion(ep_sock_t *sock, DWORD bytes, NTSTATUS status)
{
    ep_port_t *port = sock->port;
    ep_ready_node_t *node = NULL;
    uint32_t delivered = 0;
    uint32_t old_poll_status;

    (void)bytes;
    pthread_mutex_lock(&port->fd_table_lock);

    old_poll_status =
        atomic_load_explicit(&sock->poll_status, memory_order_relaxed);
    if (old_poll_status != EP_POLL_PENDING &&
        old_poll_status != EP_POLL_CANCELLED) {
        pthread_mutex_unlock(&port->fd_table_lock);
        return;
    }
    atomic_store_explicit(&sock->poll_status, EP_POLL_IDLE,
                          memory_order_relaxed);
    if (port->pending_poll_count > 0) {
        port->pending_poll_count--;
    }

    if (atomic_load_explicit(&sock->delete_pending, memory_order_relaxed)) {
        ep_sock_free_locked(port, sock);
        pthread_mutex_unlock(&port->fd_table_lock);
        return;
    }
    if (atomic_load_explicit(&port->closing, memory_order_acquire)) {
        atomic_store_explicit(&sock->state, EP_SOCK_REGISTERED,
                              memory_order_relaxed);
        pthread_mutex_unlock(&port->fd_table_lock);
        return;
    }

    if (old_poll_status == EP_POLL_CANCELLED || status == STATUS_CANCELLED) {
        if (EP_SOCK_SUBMIT_LOCKED(sock, 0) < 0) {
            sock->needs_rearm = 1;
        }
        pthread_mutex_unlock(&port->fd_table_lock);
        return;
    }

    if (status >= 0 && sock->afd_info != NULL &&
        sock->afd_info->NumberOfHandles > 0) {
        if ((sock->afd_info->Handles[0].Events & AFD_POLL_LOCAL_CLOSE) != 0) {
            /* The application closed this socket without EPOLL_CTL_DEL.
             * There is no usable registration left to report; retire it now
             * rather than trying to re-arm a stale numeric SOCKET later. */
            ep_sock_drop_closed_locked(port, sock);
            pthread_mutex_unlock(&port->fd_table_lock);
            return;
        }
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
        if (sock->endpoint_id_state == EP_SOCKET_ID_TRANSITIONAL) {
            ep_identity_check_t identity_check =
                ep_sock_validate_identity_locked(sock, 1);
            if (identity_check == EP_IDENTITY_STALE ||
                identity_check == EP_IDENTITY_CLOSED) {
                ep_sock_drop_closed_locked(port, sock);
                pthread_mutex_unlock(&port->fd_table_lock);
                return;
            }
        }
#endif
        delivered = ep_afd_to_epoll_events(
            sock->afd_info->Handles[0].Events);
    } else if (status < 0) {
        delivered = EPOLLERR;
    }
    delivered &= sock->user_events | EPOLLERR | EPOLLHUP;

    if (delivered == 0) {
        sock->needs_rearm = 1;
        if (EP_SOCK_SUBMIT_LOCKED(sock, 0) < 0) {
            sock->needs_rearm = 1;
        }
        pthread_mutex_unlock(&port->fd_table_lock);
        return;
    }

    node = ep_ready_node_alloc(port);
    if (node == NULL) {
        sock->needs_rearm = 1;
        (void)EP_SOCK_SUBMIT_LOCKED(sock, 0);
        pthread_mutex_unlock(&port->fd_table_lock);
        return;
    }

    node->data = sock->user_data;
    node->user_ctx = sock->user_ctx;
    node->fd = sock->fd;
    node->sock_generation = sock->generation;
    node->events = delivered;
    node->flags = 0;
    if ((sock->user_flags & EPOLLET) != 0) {
        node->flags |= WEPOLL_FLAG_ET_DELIVERED | WEPOLL_FLAG_EDGE_ARMED;
    }
    if ((sock->user_flags & EPOLLONESHOT) != 0) {
        node->flags |= WEPOLL_FLAG_ONESHOT_FIRED;
        sock->oneshot_fired = 1;
    }
    node->timestamp = ep_now_ns();

    sock->pending_events = delivered;
    atomic_store_explicit(&sock->ready_queued, 1, memory_order_relaxed);
    atomic_store_explicit(&sock->state, EP_SOCK_READY, memory_order_relaxed);
    ep_ready_push(&port->ready_queue, node);
    pthread_mutex_unlock(&port->fd_table_lock);
}

/* ------------------------------------------------------------------------- */
/* Port lifecycle.                                                          */
/* ------------------------------------------------------------------------- */

int ep_port_create(int size_hint, int flags, ep_port_t **out)
{
    ep_port_t *port;
    size_t pool_capacity = WEPOLL_AFD_POOL_SIZE;
    int fd_lock_initialized = 0;
    int wait_lock_initialized = 0;
    int ready_initialized = 0;
    int afd_pool_initialized = 0;
    int node_pool_initialized = 0;

    if (out == NULL) {
        ep_set_errno(EFAULT);
        return -1;
    }
    *out = NULL;
    if (ep_global_init() != 0) {
        return -1;
    }

    port = (ep_port_t *)calloc(1, sizeof(*port));
    if (port == NULL) {
        ep_set_errno(ENOMEM);
        return -1;
    }

    int mutex_error = pthread_mutex_init(&port->fd_table_lock, NULL);
    if (mutex_error != 0) {
        ep_set_errno(mutex_error);
        goto fail;
    }
    fd_lock_initialized = 1;
    mutex_error = pthread_mutex_init(&port->wait_lock, NULL);
    if (mutex_error != 0) {
        ep_set_errno(mutex_error);
        goto fail;
    }
    wait_lock_initialized = 1;

    ep_ready_init(&port->ready_queue);
    if (!port->ready_queue.initialized) {
        goto fail;
    }
    ready_initialized = 1;

    if (size_hint > 0) {
        size_t hint = (size_t)size_hint;
        if (hint > SIZE_MAX / 2) {
            ep_set_errno(ENOMEM);
            goto fail;
        }
        hint *= 2;
        if (hint < WEPOLL_INITIAL_FDS) {
            hint = WEPOLL_INITIAL_FDS;
        }
        if (ep_fd_table_grow(port, hint) != 0) {
            goto fail;
        }

        if ((size_t)size_hint > pool_capacity) {
            pool_capacity = (size_t)size_hint;
            if (pool_capacity > 4096) {
                pool_capacity = 4096;
            }
        }
    }

    if (ep_afd_pool_init(&port->afd_info_pool,
                         sizeof(AFD_POLL_INFO), pool_capacity) != 0) {
        goto fail;
    }
    afd_pool_initialized = 1;
    if (ep_afd_pool_init(&port->ready_node_pool,
                         sizeof(ep_ready_node_t), pool_capacity) != 0) {
        goto fail;
    }
    node_pool_initialized = 1;

    port->iocp_batch_size = 64;
    port->iocp_entries = (OVERLAPPED_ENTRY *)calloc(
        port->iocp_batch_size, sizeof(*port->iocp_entries));
    if (port->iocp_entries == NULL) {
        ep_set_errno(ENOMEM);
        goto fail;
    }

    port->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (port->iocp == NULL) {
        ep_set_errno(ep_winerr_to_errno(GetLastError()));
        goto fail;
    }
    if (ep_afd_open(port->iocp, &port->afd) != 0) {
        goto fail;
    }

    port->close_on_exec = (flags & EPOLL_CLOEXEC) != 0;
    atomic_init(&port->closing, 0);
    atomic_init(&port->iocp_closed, 0);
    atomic_init(&port->generation, 0);
    port->next_sock_generation = 0;
    *out = port;
    return 0;

fail:
    {
        int saved_errno = ep_last_err();
        if (port->afd != NULL) {
            CloseHandle(port->afd);
        }
        if (port->iocp != NULL) {
            CloseHandle(port->iocp);
        }
        free(port->iocp_entries);
        free(port->fd_table);
        if (node_pool_initialized) {
            ep_afd_pool_destroy(&port->ready_node_pool);
        }
        if (afd_pool_initialized) {
            ep_afd_pool_destroy(&port->afd_info_pool);
        }
        if (ready_initialized) {
            ep_ready_destroy(&port->ready_queue);
        }
        if (wait_lock_initialized) {
            pthread_mutex_destroy(&port->wait_lock);
        }
        if (fd_lock_initialized) {
            pthread_mutex_destroy(&port->fd_table_lock);
        }
        free(port);
        ep_set_errno(saved_errno);
        return -1;
    }
}

void ep_port_begin_close(ep_port_t *port)
{
    if (port == NULL) {
        return;
    }
    if (atomic_exchange_explicit(&port->closing, 1, memory_order_acq_rel) == 0 &&
        port->iocp != NULL) {
        if (!PostQueuedCompletionStatus(port->iocp, 0, 0, NULL)) {
            /* A failed wake must not leave an API waiter holding the port
             * reference forever.  Closing the completion port wakes blocked
             * GetQueuedCompletionStatusEx calls with an abandoned-wait error.
             * Pending request storage is quarantined later if it cannot be
             * drained safely. */
            (void)CloseHandle(port->iocp);
            atomic_store_explicit(&port->iocp_closed, 1,
                                  memory_order_release);
        }
    }
}

static int ep_port_quarantine(ep_port_t *port, int error)
{
    if (port->afd != NULL) {
        (void)CloseHandle(port->afd);
        port->afd = NULL;
    }
    if (port->iocp != NULL &&
        atomic_exchange_explicit(&port->iocp_closed, 1,
                                 memory_order_acq_rel) == 0) {
        (void)CloseHandle(port->iocp);
    }
    port->iocp = NULL;

    /* Outstanding AFD operations still reference sock->io_status_block and
     * afd_info.  Leaking this unreachable port is safer than freeing storage
     * that the kernel may complete asynchronously. */
    pthread_mutex_unlock(&port->wait_lock);
    ep_set_errno(error);
    return -1;
}

int ep_port_destroy(ep_port_t *port)
{
    if (port == NULL) {
        return 0;
    }

    ep_port_begin_close(port);
    pthread_mutex_lock(&port->wait_lock);

    int cancel_failed = 0;
    pthread_mutex_lock(&port->fd_table_lock);
    for (ep_sock_t *sock = port->sock_list_head;
         sock != NULL;
         sock = sock->next) {
        atomic_store_explicit(&sock->delete_pending, 1, memory_order_relaxed);
        atomic_store_explicit(&sock->state, EP_SOCK_DELETED,
                              memory_order_relaxed);
        sock->needs_rearm = 0;
        if (ep_sock_cancel_locked(sock) != 0) {
            cancel_failed = 1;
        }
    }
    if (cancel_failed && port->afd != NULL) {
        /* Closing the AFD control handle is a bulk cancellation fallback.
         * Completions remain asynchronous and must still be drained. */
        (void)CloseHandle(port->afd);
        port->afd = NULL;
    }
    if (port->fd_table != NULL) {
        memset(port->fd_table, 0,
               port->fd_table_size * sizeof(*port->fd_table));
    }
    port->fd_table_count = 0;

    ep_sock_t *sock = port->sock_list_head;
    while (sock != NULL) {
        ep_sock_t *next = sock->next;
        if (atomic_load_explicit(&sock->poll_status,
                                 memory_order_relaxed) == EP_POLL_IDLE) {
            ep_sock_free_locked(port, sock);
        }
        sock = next;
    }
    pthread_mutex_unlock(&port->fd_table_lock);

    uint64_t drain_deadline =
        GetTickCount64() + EP_CLOSE_DRAIN_TIMEOUT_MS;
    for (;;) {
        size_t pending;
        pthread_mutex_lock(&port->fd_table_lock);
        pending = port->pending_poll_count;
        pthread_mutex_unlock(&port->fd_table_lock);
        if (pending == 0) {
            break;
        }
        if (atomic_load_explicit(&port->iocp_closed,
                                 memory_order_acquire)) {
            return ep_port_quarantine(port, EIO);
        }

        uint64_t now = GetTickCount64();
        if (now >= drain_deadline) {
            return ep_port_quarantine(port, ETIMEDOUT);
        }
        uint64_t remaining = drain_deadline - now;
        DWORD wait_ms = remaining < EP_CLOSE_DRAIN_SLICE_MS
            ? (DWORD)remaining : EP_CLOSE_DRAIN_SLICE_MS;

        ULONG removed = 0;
        BOOL ok = GetQueuedCompletionStatusEx(
            port->iocp, port->iocp_entries, port->iocp_batch_size,
            &removed, wait_ms, FALSE);
        if (!ok) {
            DWORD error = GetLastError();
            if (error == WAIT_TIMEOUT) {
                continue;
            }
            return ep_port_quarantine(port, ep_winerr_to_errno(error));
        }
        for (ULONG i = 0; i < removed; i++) {
            OVERLAPPED *overlapped = port->iocp_entries[i].lpOverlapped;
            if (overlapped == NULL) {
                continue;
            }
            IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)overlapped;
            ep_sock_t *completed = (ep_sock_t *)(
                (unsigned char *)iosb - offsetof(ep_sock_t, io_status_block));
            ep_sock_handle_completion(
                completed,
                port->iocp_entries[i].dwNumberOfBytesTransferred,
                iosb->Status);
        }
    }

    pthread_mutex_lock(&port->fd_table_lock);
    sock = port->sock_list_head;
    while (sock != NULL) {
        ep_sock_t *next = sock->next;
        ep_sock_free_locked(port, sock);
        sock = next;
    }
    pthread_mutex_unlock(&port->fd_table_lock);

    for (;;) {
        ep_ready_node_t *node = ep_ready_drain(&port->ready_queue, INT_MAX);
        if (node == NULL) {
            break;
        }
        while (node != NULL) {
            ep_ready_node_t *next = atomic_load_explicit(
                &node->next, memory_order_relaxed);
            ep_ready_node_free(port, node);
            node = next;
        }
    }
    ep_ready_destroy(&port->ready_queue);

    if (port->afd != NULL) {
        (void)CloseHandle(port->afd);
        port->afd = NULL;
    }
    if (port->iocp != NULL &&
        atomic_exchange_explicit(&port->iocp_closed, 1,
                                 memory_order_acq_rel) == 0) {
        (void)CloseHandle(port->iocp);
    }
    port->iocp = NULL;
    ep_afd_pool_destroy(&port->afd_info_pool);
    ep_afd_pool_destroy(&port->ready_node_pool);
    free(port->fd_table);
    free(port->iocp_entries);

    pthread_mutex_unlock(&port->wait_lock);
    pthread_mutex_destroy(&port->wait_lock);
    pthread_mutex_destroy(&port->fd_table_lock);
    free(port);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* epoll_ctl operations.                                                    */
/* ------------------------------------------------------------------------- */

int ep_port_register(ep_port_t *port, SOCKET fd,
                     uint32_t events, uint32_t flags,
                     epoll_data_t data, void *ctx)
{
    ep_sock_t *existing;
    ep_sock_t *sock;

    if (fd == INVALID_SOCKET) {
        ep_set_errno(EBADF);
        return -1;
    }
    pthread_mutex_lock(&port->fd_table_lock);
    if (atomic_load_explicit(&port->closing, memory_order_acquire)) {
        pthread_mutex_unlock(&port->fd_table_lock);
        ep_set_errno(EBADF);
        return -1;
    }
    existing = ep_fd_table_lookup(port, fd);
    if (existing != NULL) {
        int identity_result =
            ep_sock_validate_control_locked(port, existing);
        if (identity_result == 0) {
            pthread_mutex_unlock(&port->fd_table_lock);
            ep_set_errno(EEXIST);
            return -1;
        }
        if (identity_result < 0 || ep_last_err() != ENOENT) {
            pthread_mutex_unlock(&port->fd_table_lock);
            return -1;
        }
        /* A stable endpoint mismatch means this numeric SOCKET now names a
         * different object.  The stale entry is retired above; ADD may create
         * the new registration while the old cancellation completion drains. */
    }

    sock = ep_sock_alloc_locked(port, fd);
    if (sock == NULL) {
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }
    sock->user_events = events;
    sock->user_flags = flags;
    sock->user_data = data;
    sock->user_ctx = ctx;
    sock->needs_rearm = 1;

    if (ep_fd_table_insert(port, sock) != 0) {
        ep_afd_pool_give(&port->afd_info_pool, sock->afd_info);
        free(sock);
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }
    ep_sock_list_add_locked(port, sock);

    {
        int submit_result = EP_SOCK_SUBMIT_LOCKED(sock, 1);
        if (submit_result < 0) {
            ep_fd_table_remove(port, sock);
            ep_sock_free_locked(port, sock);
            pthread_mutex_unlock(&port->fd_table_lock);
            return -1;
        }
        if (submit_result > 0) {
            int saved_errno = ep_last_err();
            pthread_mutex_unlock(&port->fd_table_lock);
            ep_set_errno(saved_errno);
            return -1;
        }
    }
    atomic_fetch_add_explicit(&port->generation, 1, memory_order_relaxed);
    pthread_mutex_unlock(&port->fd_table_lock);
    return 0;
}

int ep_port_modify(ep_port_t *port, SOCKET fd,
                   uint32_t events, uint32_t flags,
                   epoll_data_t data, void *ctx)
{
    uint32_t old_pending_events;
    uint32_t old_ready_queued;
    uint32_t old_state;
    uint32_t old_user_events;
    uint32_t old_user_flags;
    epoll_data_t old_user_data;
    void *old_user_ctx;
    SOCKET old_base_socket;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    uint64_t old_endpoint_id;
    uint8_t old_endpoint_id_state;
#endif
    uint64_t old_generation;
    uint8_t old_needs_rearm;
    uint8_t old_oneshot_fired;

    pthread_mutex_lock(&port->fd_table_lock);
    ep_sock_t *sock = ep_fd_table_lookup(port, fd);
    if (sock == NULL) {
        pthread_mutex_unlock(&port->fd_table_lock);
        ep_set_errno(ENOENT);
        return -1;
    }
    if (ep_sock_validate_control_locked(port, sock) != 0) {
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }

    if (atomic_load_explicit(&sock->poll_status, memory_order_relaxed) ==
            EP_POLL_PENDING &&
        ep_sock_cancel_locked(sock) != 0) {
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }

    old_user_events = sock->user_events;
    old_user_flags = sock->user_flags;
    old_user_data = sock->user_data;
    old_user_ctx = sock->user_ctx;
    old_pending_events = sock->pending_events;
    old_oneshot_fired = sock->oneshot_fired;
    old_needs_rearm = sock->needs_rearm;
    old_base_socket = sock->base_socket;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    old_endpoint_id = sock->endpoint_id;
    old_endpoint_id_state = sock->endpoint_id_state;
#endif
    old_generation = sock->generation;
    old_ready_queued = atomic_load_explicit(&sock->ready_queued,
                                             memory_order_relaxed);
    old_state = atomic_load_explicit(&sock->state, memory_order_relaxed);

    sock->user_events = events;
    sock->user_flags = flags;
    sock->user_data = data;
    sock->user_ctx = ctx;
    sock->pending_events = 0;
    sock->oneshot_fired = 0;
    sock->needs_rearm = 1;
    atomic_store_explicit(&sock->ready_queued, 0, memory_order_relaxed);
    sock->generation = ++port->next_sock_generation;
    if (port->next_sock_generation == 0) {
        port->next_sock_generation = 1;
        sock->generation = 1;
    }

    if (atomic_load_explicit(&sock->poll_status, memory_order_relaxed) ==
            EP_POLL_IDLE) {
        int submit_result = EP_SOCK_SUBMIT_LOCKED(sock, 1);
        if (submit_result > 0) {
            int saved_errno = ep_last_err();
            pthread_mutex_unlock(&port->fd_table_lock);
            ep_set_errno(saved_errno);
            return -1;
        }
        if (submit_result < 0) {
            int saved_errno = ep_last_err();
            sock->user_events = old_user_events;
            sock->user_flags = old_user_flags;
            sock->user_data = old_user_data;
            sock->user_ctx = old_user_ctx;
            sock->pending_events = old_pending_events;
            sock->oneshot_fired = old_oneshot_fired;
            sock->needs_rearm = old_needs_rearm;
            sock->base_socket = old_base_socket;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
            sock->endpoint_id = old_endpoint_id;
            sock->endpoint_id_state = old_endpoint_id_state;
#endif
            sock->generation = old_generation;
            atomic_store_explicit(&sock->ready_queued, old_ready_queued,
                                  memory_order_relaxed);
            atomic_store_explicit(&sock->state, old_state,
                                  memory_order_relaxed);
            pthread_mutex_unlock(&port->fd_table_lock);
            ep_set_errno(saved_errno);
            return -1;
        }
    }
    atomic_fetch_add_explicit(&port->generation, 1, memory_order_relaxed);
    pthread_mutex_unlock(&port->fd_table_lock);
    return 0;
}

int ep_port_unregister(ep_port_t *port, SOCKET fd)
{
    int saved_errno = ep_last_err();

    pthread_mutex_lock(&port->fd_table_lock);
    ep_sock_t *sock = ep_fd_table_lookup(port, fd);
    if (sock == NULL) {
        pthread_mutex_unlock(&port->fd_table_lock);
        ep_set_errno(ENOENT);
        return -1;
    }
    if (ep_sock_validate_control_locked(port, sock) != 0) {
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }

    if (atomic_load_explicit(&sock->poll_status, memory_order_relaxed) ==
        EP_POLL_PENDING) {
        /* DEL is a logical detach operation.  Even if kernel cancellation
         * fails, remove the public registration now and retain sock storage
         * until the outstanding completion arrives.  This also makes a
         * DEL-before-closesocket() lifetime contract safe for embedders. */
        (void)ep_sock_cancel_locked(sock);
    }

    ep_fd_table_remove(port, sock);
    atomic_store_explicit(&sock->delete_pending, 1, memory_order_relaxed);
    atomic_store_explicit(&sock->state, EP_SOCK_DELETED, memory_order_relaxed);
    sock->needs_rearm = 0;
    sock->generation = ++port->next_sock_generation;
    atomic_fetch_add_explicit(&port->generation, 1, memory_order_relaxed);

    if (atomic_load_explicit(&sock->poll_status, memory_order_relaxed) ==
        EP_POLL_IDLE) {
        ep_sock_free_locked(port, sock);
    }
    pthread_mutex_unlock(&port->fd_table_lock);
    ep_set_errno(saved_errno);
    return 0;
}

int ep_port_rearm(ep_port_t *port, SOCKET fd)
{
    uint32_t old_pending_events;
    uint32_t old_ready_queued;
    uint32_t old_state;
    SOCKET old_base_socket;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    uint64_t old_endpoint_id;
    uint8_t old_endpoint_id_state;
#endif
    uint64_t old_generation;
    uint8_t old_needs_rearm;
    uint8_t old_oneshot_fired;

    pthread_mutex_lock(&port->fd_table_lock);
    ep_sock_t *sock = ep_fd_table_lookup(port, fd);
    if (sock == NULL) {
        pthread_mutex_unlock(&port->fd_table_lock);
        ep_set_errno(ENOENT);
        return -1;
    }
    if (ep_sock_validate_control_locked(port, sock) != 0) {
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }
    if (!sock->oneshot_fired) {
        pthread_mutex_unlock(&port->fd_table_lock);
        return 0;
    }

    old_pending_events = sock->pending_events;
    old_oneshot_fired = sock->oneshot_fired;
    old_needs_rearm = sock->needs_rearm;
    old_base_socket = sock->base_socket;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    old_endpoint_id = sock->endpoint_id;
    old_endpoint_id_state = sock->endpoint_id_state;
#endif
    old_generation = sock->generation;
    old_ready_queued = atomic_load_explicit(&sock->ready_queued,
                                             memory_order_relaxed);
    old_state = atomic_load_explicit(&sock->state, memory_order_relaxed);

    sock->oneshot_fired = 0;
    sock->pending_events = 0;
    sock->needs_rearm = 1;
    atomic_store_explicit(&sock->ready_queued, 0, memory_order_relaxed);
    sock->generation = ++port->next_sock_generation;
    if (port->next_sock_generation == 0) {
        port->next_sock_generation = 1;
        sock->generation = 1;
    }
    int result = EP_SOCK_SUBMIT_LOCKED(sock, 1);
    if (result > 0) {
        int saved_errno = ep_last_err();
        result = -1;
        ep_set_errno(saved_errno);
    } else if (result < 0) {
        int saved_errno = ep_last_err();
        sock->pending_events = old_pending_events;
        sock->oneshot_fired = old_oneshot_fired;
        sock->needs_rearm = old_needs_rearm;
        sock->base_socket = old_base_socket;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
        sock->endpoint_id = old_endpoint_id;
        sock->endpoint_id_state = old_endpoint_id_state;
#endif
        sock->generation = old_generation;
        atomic_store_explicit(&sock->ready_queued, old_ready_queued,
                              memory_order_relaxed);
        atomic_store_explicit(&sock->state, old_state,
                              memory_order_relaxed);
        ep_set_errno(saved_errno);
    } else {
        atomic_fetch_add_explicit(&port->generation, 1,
                                  memory_order_relaxed);
    }
    pthread_mutex_unlock(&port->fd_table_lock);
    return result;
}

/* ------------------------------------------------------------------------- */
/* Ready queue drain and wait loop.                                         */
/* ------------------------------------------------------------------------- */

static int ep_drain_to_buffer(ep_port_t *port,
                              epoll_event_ex *out,
                              int maxevents)
{
    int delivered = 0;

    while (delivered < maxevents) {
        ep_ready_node_t *node = ep_ready_drain(&port->ready_queue, 1);
        if (node == NULL) {
            break;
        }

        int valid = 0;
        pthread_mutex_lock(&port->fd_table_lock);
        ep_sock_t *sock = ep_fd_table_lookup(port, node->fd);
        if (sock != NULL && sock->generation == node->sock_generation &&
            !atomic_load_explicit(&sock->delete_pending,
                                  memory_order_relaxed)) {
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
            ep_identity_check_t identity_check =
                ep_sock_validate_identity_locked(sock, 0);

            if (identity_check == EP_IDENTITY_STALE ||
                identity_check == EP_IDENTITY_CLOSED) {
                /* The completion belongs to the old kernel socket object.
                 * Retire it before a queued snapshot can surface stale data
                 * for a replacement that reused the same numeric handle. */
                ep_sock_retire_stale_locked(port, sock);
            } else
#endif
            {
                /* Endpoint identity is an optional hardening layer.  If its
                 * query fails transiently, retain legacy delivery semantics
                 * rather than losing a level-triggered or oneshot event. */
                valid = 1;
                sock->pending_events = 0;
                atomic_store_explicit(&sock->ready_queued, 0,
                                      memory_order_relaxed);
                atomic_store_explicit(&sock->state, EP_SOCK_REGISTERED,
                                      memory_order_relaxed);
                if (!sock->oneshot_fired) {
                    sock->needs_rearm = 1;
                }
            }
        }
        pthread_mutex_unlock(&port->fd_table_lock);

        if (valid) {
            out[delivered].events = node->events;
            out[delivered].data = node->data;
            out[delivered].flags = node->flags;
            out[delivered].timestamp = node->timestamp;
            out[delivered].user_ctx = node->user_ctx;
            delivered++;
        }
        ep_ready_node_free(port, node);
    }
    return delivered;
}

/* The ready queue has one consumer, so waiters still serialize their drain
 * operation.  Do not let that serialization turn a bounded wait into an
 * unbounded mutex wait: a zero-timeout drain returns immediately when another
 * consumer owns the lock, and a positive timeout includes lock acquisition. */
static int ep_wait_lock_acquire(ep_port_t *port, int timeout_ms,
                                uint64_t deadline)
{
    if (timeout_ms < 0) {
        int lock_result = pthread_mutex_lock(&port->wait_lock);
        if (lock_result != 0) {
            ep_set_errno(lock_result);
            return -1;
        }
        return 1;
    }

    int first_attempt = 1;
    for (;;) {
        if (!first_attempt && timeout_ms > 0 &&
            GetTickCount64() >= deadline) {
            return 0;
        }
        first_attempt = 0;

        int lock_result = pthread_mutex_trylock(&port->wait_lock);
        if (lock_result == 0) {
            return 1;
        }
        if (lock_result != EBUSY) {
            ep_set_errno(lock_result);
            return -1;
        }
        if (atomic_load_explicit(&port->closing, memory_order_acquire)) {
            ep_set_errno(EBADF);
            return -1;
        }
        if (timeout_ms == 0 ||
            (timeout_ms > 0 && GetTickCount64() >= deadline)) {
            return 0;
        }
        Sleep(1);
    }
}

int ep_port_wait(ep_port_t *port, epoll_event_ex *out, int maxevents,
                 int timeout_ms, const wepoll_sigset_t *sigmask)
{
    uint64_t deadline = 0;
    int result = -1;
    int lock_result;

    if (out == NULL) {
        ep_set_errno(EFAULT);
        return -1;
    }
    if (maxevents <= 0) {
        ep_set_errno(EINVAL);
        return -1;
    }
    if (sigmask != NULL) {
        ep_set_errno(EOPNOTSUPP);
        return -1;
    }

    if (timeout_ms > 0) {
        deadline = GetTickCount64() + (uint64_t)timeout_ms;
    }
    lock_result = ep_wait_lock_acquire(port, timeout_ms, deadline);
    if (lock_result <= 0) {
        return lock_result == 0 ? 0 : -1;
    }

    for (;;) {
        if (atomic_load_explicit(&port->closing, memory_order_acquire)) {
            ep_set_errno(EBADF);
            result = -1;
            break;
        }

        pthread_mutex_lock(&port->fd_table_lock);
        int arm_result = ep_port_arm_pending_locked(port);
        pthread_mutex_unlock(&port->fd_table_lock);
        if (arm_result != 0) {
            result = -1;
            break;
        }

        result = ep_drain_to_buffer(port, out, maxevents);
        if (result > 0) {
            break;
        }

        DWORD wait_ms;
        if (timeout_ms < 0) {
            wait_ms = INFINITE;
        } else if (timeout_ms == 0) {
            wait_ms = 0;
        } else {
            uint64_t now = GetTickCount64();
            if (now >= deadline) {
                result = 0;
                break;
            }
            wait_ms = (DWORD)(deadline - now);
        }

        ULONG removed = 0;
        BOOL ok = GetQueuedCompletionStatusEx(
            port->iocp, port->iocp_entries, port->iocp_batch_size,
            &removed, wait_ms, FALSE);
        if (!ok) {
            DWORD error = GetLastError();
            if (error == WAIT_TIMEOUT) {
                result = 0;
            } else {
                ep_set_errno(ep_winerr_to_errno(error));
                result = -1;
            }
            break;
        }

        for (ULONG i = 0; i < removed; i++) {
            OVERLAPPED *overlapped = port->iocp_entries[i].lpOverlapped;
            if (overlapped == NULL) {
                continue;
            }
            IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)overlapped;
            ep_sock_t *sock = (ep_sock_t *)(
                (unsigned char *)iosb - offsetof(ep_sock_t, io_status_block));
            ep_sock_handle_completion(
                sock,
                port->iocp_entries[i].dwNumberOfBytesTransferred,
                iosb->Status);
        }

        result = ep_drain_to_buffer(port, out, maxevents);
        if (result > 0) {
            break;
        }
        if (timeout_ms == 0) {
            result = 0;
            break;
        }
        if (timeout_ms > 0 && GetTickCount64() >= deadline) {
            result = 0;
            break;
        }
    }

    pthread_mutex_unlock(&port->wait_lock);
    return result;
}
