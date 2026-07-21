/*
 * wepoll_ex_port.c — ep_port_t lifecycle, fd table, IOCP loop.
 *
 * One ep_port_t corresponds to one epoll instance.  Internally it owns:
 *   - An IOCP handle (CreateIoCompletionPort)
 *   - An AFD handle (NtCreateFile on \Device\Afd\WepollEx)
 *   - A growable fd -> ep_sock_t* table
 *   - An MPSC lock-free ready queue of fired events
 *   - Two AFD buffer pools: one for AFD_POLL_INFO buffers, one for
 *     ready-queue nodes
 *
 * The IOCP is the heart of the design: every AFD poll completion lands
 * on the IOCP, where a worker dequeues it, translates AFD events to
 * epoll events, and pushes the result onto the ready queue.  epoll_wait
 * then drains the ready queue without contention.
 *
 * For nginx-class workloads the IOCP approach scales linearly: each
 * accepted connection costs one AFD_POLL pended and one IOCP packet
 * when activity is detected.  No user-mode polling thread.
 */
#include "wepoll_ex_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <windows.h>
#  include <winternl.h>
#  include <mswsock.h>
#endif

/* --------------------------------------------------------------------- */
/* Internal helpers.                                                  */
/* --------------------------------------------------------------------- */

static uint64_t ep_now_ns(void)
{
    /* We use QueryPerformanceCounter on Windows; clock_gettime on
     * POSIX.  Both are monotonic. */
#ifdef _WIN32
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000000ULL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* --------------------------------------------------------------------- */
/* fd table.                                                          */
/* --------------------------------------------------------------------- */

static int ep_fd_table_grow(ep_port_t *port, size_t new_size)
{
    ep_sock_t **new_table = (ep_sock_t **)calloc(new_size, sizeof(ep_sock_t *));
    if (new_table == NULL) {
        ep_set_errno(ENOMEM);
        return -1;
    }
    for (size_t i = 0; i < port->fd_table_size; i++) {
        ep_sock_t *s = port->fd_table[i];
        if (s) {
            size_t slot = (size_t)s->fd % new_size;
            while (new_table[slot]) slot = (slot + 1) % new_size;
            new_table[slot] = s;
        }
    }
    free(port->fd_table);
    port->fd_table = new_table;
    port->fd_table_size = new_size;
    return 0;
}

static ep_sock_t *ep_fd_table_lookup(ep_port_t *port, SOCKET fd)
{
    if (port->fd_table_size == 0) return NULL;
    size_t slot = (size_t)fd % port->fd_table_size;
    for (size_t probes = 0; probes < port->fd_table_size; probes++) {
        ep_sock_t *s = port->fd_table[slot];
        if (s == NULL) return NULL;
        if (s->fd == fd) return s;
        slot = (slot + 1) % port->fd_table_size;
    }
    return NULL;
}

static int ep_fd_table_insert(ep_port_t *port, ep_sock_t *s)
{
    if (port->fd_table_count * 4 >= port->fd_table_size * 3) {
        size_t new_size = port->fd_table_size == 0
                          ? WEPOLL_INITIAL_FDS
                          : port->fd_table_size * 2;
        if (ep_fd_table_grow(port, new_size) != 0) return -1;
    }
    size_t slot = (size_t)s->fd % port->fd_table_size;
    while (port->fd_table[slot]) slot = (slot + 1) % port->fd_table_size;
    port->fd_table[slot] = s;
    port->fd_table_count++;
    return 0;
}

static void ep_fd_table_remove(ep_port_t *port, ep_sock_t *s)
{
    size_t slot = (size_t)s->fd % port->fd_table_size;
    while (port->fd_table[slot] != s) {
        slot = (slot + 1) % port->fd_table_size;
        if (port->fd_table[slot] == NULL) return;
    }
    port->fd_table[slot] = NULL;
    port->fd_table_count--;

    size_t j = slot;
    for (;;) {
        j = (j + 1) % port->fd_table_size;
        ep_sock_t *t = port->fd_table[j];
        if (t == NULL) break;
        port->fd_table[j] = NULL;
        size_t k = (size_t)t->fd % port->fd_table_size;
        while (port->fd_table[k]) k = (k + 1) % port->fd_table_size;
        port->fd_table[k] = t;
    }
}

/* --------------------------------------------------------------------- */
/* ep_sock_t allocation.                                              */
/*                                                                     */
/* Uses the per-port AFD buffer pool for the AFD_POLL_INFO buffer,    */
/* eliminating malloc on the EPOLL_CTL_ADD hot path.                  */
/* --------------------------------------------------------------------- */

static ep_sock_t *ep_sock_alloc(SOCKET fd, ep_port_t *port)
{
    ep_sock_t *s = (ep_sock_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        ep_set_errno(ENOMEM);
        return NULL;
    }
    s->fd = fd;
    s->port = port;
    s->state = EP_SOCK_REGISTERED;

    /* Pull the AFD_POLL_INFO buffer from the pool. */
    s->afd_info = (AFD_POLL_INFO *)ep_afd_pool_take(&port->afd_info_pool);
    if (s->afd_info == NULL) {
        free(s);
        return NULL;
    }
    return s;
}

static void ep_sock_free(ep_port_t *port, ep_sock_t *s)
{
    if (s->afd_info) {
        ep_afd_pool_give(&port->afd_info_pool, s->afd_info);
        s->afd_info = NULL;
    }
    free(s);
}

/* --------------------------------------------------------------------- */
/* Re-arm AFD poll for a sock.                                        */
/* --------------------------------------------------------------------- */

static int ep_sock_rearm_afd(ep_sock_t *s)
{
    if (s->user_events == 0) return 0;

    uint32_t afd_events = ep_epoll_to_afd_events(s->user_events);
    if (afd_events == 0) return 0;

    s->poll_pending = 0;
    return ep_afd_poll_submit(s, afd_events);
}

/* --------------------------------------------------------------------- */
/* IOCP completion dispatch.                                         */
/*                                                                     */
/* Called when GetQueuedCompletionStatusEx returns a packet for a     */
/* poll we submitted.  The lpOverlapped we get back is the            */
/* OVERLAPPED embedded in ep_sock_t.                                */
/* --------------------------------------------------------------------- */

void ep_sock_handle_completion(ep_sock_t *sock, DWORD bytes,
                               NTSTATUS status)
{
    (void)bytes;

    sock->poll_pending = 0;

    if (atomic_load(&sock->state) == EP_SOCK_DELETED) {
        return;
    }

    if (status == STATUS_CANCELLED) return;

    uint32_t fired = 0;
    if (status >= 0 && sock->afd_info) {
        fired = ep_afd_to_epoll_events(sock->afd_info->Handles[0].Events);
    } else if (status < 0) {
        fired = EPOLLERR;
    }

    uint32_t delivered = fired & (sock->user_events | EPOLLERR | EPOLLHUP);

    if (delivered == 0) {
        ep_sock_rearm_afd(sock);
        return;
    }

    /* Edge-triggered semantics: only deliver if there's something new. */
    if (sock->user_flags & EPOLLET) {
        if ((delivered & ~sock->pending_events) == 0) {
            ep_sock_rearm_afd(sock);
            return;
        }
        delivered = delivered & ~sock->pending_events;
    }

    sock->pending_events |= delivered;

    if (sock->user_flags & EPOLLONESHOT) {
        sock->oneshot_fired = 1;
    } else {
        ep_sock_rearm_afd(sock);
    }

    /* Allocate a ready node from the pool — no malloc on the hot
     * path.  If the pool is exhausted (very rare), we malloc a
     * fresh node that will be returned to the pool when drained. */
    ep_ready_node_t *node = ep_ready_node_alloc(sock->port);
    if (node == NULL) {
        /* OOM on the ready path: drop the event. */
        return;
    }
    node->sock      = sock;
    node->events    = delivered;
    node->flags     = (sock->user_flags & EPOLLET)       ? WEPOLL_FLAG_ET_DELIVERED : 0;
    node->flags    |= (sock->user_flags & EPOLLET)       ? WEPOLL_FLAG_EDGE_ARMED   : 0;
    node->flags    |= (sock->user_flags & EPOLLONESHOT)  ? WEPOLL_FLAG_ONESHOT_FIRED: 0;
    node->timestamp = ep_now_ns();
    ep_ready_push(&sock->port->ready_queue, node);
}

/* --------------------------------------------------------------------- */
/* Public port lifecycle.                                            */
/* --------------------------------------------------------------------- */

int ep_port_create(int size_hint, int flags, ep_port_t **out)
{
    if (ep_global_init() != 0) return -1;

    ep_port_t *p = (ep_port_t *)calloc(1, sizeof(*p));
    if (p == NULL) {
        ep_set_errno(ENOMEM);
        return -1;
    }

#ifdef _WIN32
    p->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (p->iocp == NULL) {
        ep_set_errno(ep_winerr_to_errno(GetLastError()));
        free(p);
        return -1;
    }

    if (ep_afd_open(&p->afd) != 0) {
        CloseHandle(p->iocp);
        free(p);
        return -1;
    }

    if (CreateIoCompletionPort(p->afd, p->iocp, 0, 0) == NULL) {
        ep_set_errno(ep_winerr_to_errno(GetLastError()));
        CloseHandle(p->afd);
        CloseHandle(p->iocp);
        free(p);
        return -1;
    }
#else
    int fd = epoll_create(size_hint > 0 ? size_hint : 1);
    if (fd < 0) { free(p); return -1; }
    p->iocp = (HANDLE)(intptr_t)fd;
    p->afd  = NULL;
#endif

    pthread_mutex_init(&p->fd_table_lock, NULL);
    pthread_mutex_init(&p->sock_list_lock, NULL);
    ep_ready_init(&p->ready_queue);

    p->fd_table_size = 0;
    p->fd_table_count = 0;
    p->fd_table = NULL;
    p->sock_list_head = NULL;
    p->close_on_exec = (flags & EPOLL_CLOEXEC) ? 1 : 0;
    atomic_store(&p->generation, 0);

    /* Pre-size the fd table if caller gave a hint. */
    if (size_hint > 0) {
        size_t hint = (size_t)size_hint * 2;
        if (hint < WEPOLL_INITIAL_FDS) hint = WEPOLL_INITIAL_FDS;
        ep_fd_table_grow(p, hint);
    }

    /* Initialize the AFD buffer pools.  Capacity is sized to the
     * expected steady-state fd count, with on-the-fly growth for
     * bursts.  The default is conservative (256 buffers per pool);
     * nginx-class workloads with 10k+ connections will hit the
     * pool-exhaustion fallback regularly but the per-malloc cost
     * is amortised over the fd's lifetime. */
    size_t pool_cap = WEPOLL_AFD_POOL_SIZE;
    if (size_hint > 0 && (size_t)size_hint > pool_cap) {
        pool_cap = (size_t)size_hint;
    }
    if (ep_afd_pool_init(&p->afd_info_pool,
                         sizeof(AFD_POLL_INFO), pool_cap) != 0) {
        ep_port_destroy(p);
        return -1;
    }
    if (ep_afd_pool_init(&p->ready_node_pool,
                         sizeof(ep_ready_node_t), pool_cap) != 0) {
        ep_port_destroy(p);
        return -1;
    }

    /* Allocate the IOCP batch buffer.  64 entries is a sensible
     * default — most epoll_wait calls return far fewer events, and
     * GetQueuedCompletionStatusEx's cost is amortised across the
     * entries it returns. */
    p->iocp_batch_size = 64;
#ifdef _WIN32
    p->iocp_entries = (OVERLAPPED_ENTRY *)
        calloc(p->iocp_batch_size, sizeof(*p->iocp_entries));
    if (p->iocp_entries == NULL) {
        ep_set_errno(ENOMEM);
        ep_port_destroy(p);
        return -1;
    }
#else
    p->iocp_entries = NULL;
#endif

    *out = p;
    return 0;
}

void ep_port_destroy(ep_port_t *port)
{
    if (port == NULL) return;

    /* Detach all live socks. */
    pthread_mutex_lock(&port->sock_list_lock);
    ep_sock_t *s = port->sock_list_head;
    while (s) {
        ep_sock_t *next = s->next;
        ep_sock_free(port, s);
        s = next;
    }
    port->sock_list_head = NULL;
    pthread_mutex_unlock(&port->sock_list_lock);

    /* Drain ready queue and free any nodes still on it. */
    for (;;) {
        ep_ready_node_t *n = ep_ready_drain(&port->ready_queue, INT_MAX);
        if (n == NULL) break;
        while (n) {
            ep_ready_node_t *next = atomic_load(&n->next);
            ep_ready_node_free(port, n);
            n = next;
        }
    }
    ep_ready_destroy(&port->ready_queue);

    ep_afd_pool_destroy(&port->afd_info_pool);
    ep_afd_pool_destroy(&port->ready_node_pool);

    free(port->fd_table);
    free(port->iocp_entries);

#ifdef _WIN32
    if (port->afd)  CloseHandle(port->afd);
    if (port->iocp) CloseHandle(port->iocp);
#else
    if (port->iocp) close((int)(intptr_t)port->iocp);
#endif

    pthread_mutex_destroy(&port->fd_table_lock);
    pthread_mutex_destroy(&port->sock_list_lock);

    free(port);
}

/* --------------------------------------------------------------------- */
/* epoll_ctl implementation.                                         */
/* --------------------------------------------------------------------- */

int ep_port_register(ep_port_t *port, SOCKET fd,
                     uint32_t events, uint32_t flags,
                     epoll_data_t data, void *ctx)
{
    pthread_mutex_lock(&port->fd_table_lock);

    if (ep_fd_table_lookup(port, fd) != NULL) {
        ep_set_errno(EEXIST);
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }

    ep_sock_t *s = ep_sock_alloc(fd, port);
    if (s == NULL) {
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }
    s->user_events = events;
    s->user_flags  = flags;
    s->user_data   = data;
    s->user_ctx    = ctx;

    if (ep_fd_table_insert(port, s) != 0) {
        ep_sock_free(port, s);
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }

    pthread_mutex_lock(&port->sock_list_lock);
    s->next = port->sock_list_head;
    s->prev = NULL;
    if (port->sock_list_head) port->sock_list_head->prev = s;
    port->sock_list_head = s;
    pthread_mutex_unlock(&port->sock_list_lock);

    pthread_mutex_unlock(&port->fd_table_lock);

    atomic_fetch_add(&port->generation, 1);

    if (events != 0) {
        if (ep_sock_rearm_afd(s) != 0) {
            ep_port_unregister(port, fd);
            return -1;
        }
    }
    return 0;
}

int ep_port_modify(ep_port_t *port, SOCKET fd,
                   uint32_t events, uint32_t flags,
                   epoll_data_t data, void *ctx)
{
    pthread_mutex_lock(&port->fd_table_lock);
    ep_sock_t *s = ep_fd_table_lookup(port, fd);
    if (s == NULL) {
        ep_set_errno(ENOENT);
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }

    s->user_events = events;
    s->user_flags  = flags;
    s->user_data   = data;
    if (ctx != NULL) s->user_ctx = ctx;
    s->oneshot_fired = 0;
    s->pending_events = 0;

    pthread_mutex_unlock(&port->fd_table_lock);

    return ep_sock_rearm_afd(s);
}

int ep_port_unregister(ep_port_t *port, SOCKET fd)
{
    pthread_mutex_lock(&port->fd_table_lock);
    ep_sock_t *s = ep_fd_table_lookup(port, fd);
    if (s == NULL) {
        ep_set_errno(ENOENT);
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }

    atomic_store(&s->state, EP_SOCK_DELETED);

    pthread_mutex_lock(&port->sock_list_lock);
    if (s->prev) s->prev->next = s->next;
    else         port->sock_list_head = s->next;
    if (s->next) s->next->prev = s->prev;
    pthread_mutex_unlock(&port->sock_list_lock);

    ep_fd_table_remove(port, s);
    pthread_mutex_unlock(&port->fd_table_lock);

    ep_sock_free(port, s);
    return 0;
}

int ep_port_rearm(ep_port_t *port, SOCKET fd)
{
    pthread_mutex_lock(&port->fd_table_lock);
    ep_sock_t *s = ep_fd_table_lookup(port, fd);
    if (s == NULL) {
        ep_set_errno(ENOENT);
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }
    if (!s->oneshot_fired) {
        pthread_mutex_unlock(&port->fd_table_lock);
        return 0;
    }
    s->oneshot_fired = 0;
    s->pending_events = 0;
    pthread_mutex_unlock(&port->fd_table_lock);

    return ep_sock_rearm_afd(s);
}

/* --------------------------------------------------------------------- */
/* Helper: drain ready queue into caller's epoll_event_ex buffer.     */
/*                                                                   */
/* Returns the number of events written.  Nodes are returned to the  */
/* ready_node_pool as they're consumed.                             */
/* --------------------------------------------------------------------- */

static int ep_drain_to_buffer(ep_port_t *port,
                              epoll_event_ex *out, int maxevents)
{
    int delivered = 0;
    ep_ready_node_t *node = ep_ready_drain(&port->ready_queue, maxevents);
    while (node && delivered < maxevents) {
        epoll_event_ex *dst = &out[delivered];
        dst->events    = node->events;
        dst->flags     = node->flags;
        dst->timestamp = node->timestamp;

        if (node->sock) {
            dst->data     = node->sock->user_data;
            dst->user_ctx = node->sock->user_ctx;
        } else {
            memset(&dst->data, 0, sizeof(dst->data));
            dst->user_ctx = NULL;
        }

        ep_ready_node_t *next = atomic_load(&node->next);
        ep_ready_node_free(port, node);
        node = next;
        delivered++;
    }
    /* If we stopped early (maxevents hit), push the remainder back. */
    if (node) {
        /* Re-attach by pushing each remaining node back onto the
         * queue.  We can't just re-link the chain because the
         * consumer cleared the head/tail; we have to push one at
         * a time. */
        while (node) {
            ep_ready_node_t *next = atomic_load(&node->next);
            atomic_store(&node->next, (ep_ready_node_t *)NULL);
            ep_ready_push(&port->ready_queue, node);
            node = next;
        }
    }
    return delivered;
}

/* --------------------------------------------------------------------- */
/* epoll_wait implementation.                                        */
/*                                                                   */
/* 1. Drain ready queue (no syscall).                              */
/* 2. If still empty and timeout != 0, block on the IOCP.           */
/* 3. GetQueuedCompletionStatusEx returns up to iocp_batch_size     */
/*    completions in one call — process all of them, then drain     */
/*    the ready queue again.                                       */
/* --------------------------------------------------------------------- */

int ep_port_wait(ep_port_t *port, epoll_event_ex *out, int maxevents,
                 int timeout_ms, const sigset_t *sigmask)
{
    (void)sigmask;

    if (maxevents <= 0) {
        ep_set_errno(EINVAL);
        return -1;
    }

    /* Step 1: drain the ready queue without blocking. */
    int delivered = ep_drain_to_buffer(port, out, maxevents);
    if (delivered > 0) return delivered;

    if (timeout_ms == 0) return 0;

#ifdef _WIN32
    DWORD due = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    ULONG removed = 0;
    BOOL ok;

    /* GetQueuedCompletionStatusEx: batched IOCP delivery.  Returns
     * up to iocp_batch_size completions in one syscall, amortising
     * the kernel transition cost across multiple events. */
    ok = GetQueuedCompletionStatusEx(
        port->iocp,
        port->iocp_entries,
        port->iocp_batch_size,
        &removed,
        due,
        FALSE);

    if (!ok) {
        DWORD err = GetLastError();
        if (err == WAIT_TIMEOUT) return 0;
        ep_set_errno(ep_winerr_to_errno(err));
        return -1;
    }

    /* Dispatch every completion that came back. */
    for (ULONG i = 0; i < removed; i++) {
        OVERLAPPED *ovlp = port->iocp_entries[i].lpOverlapped;
        if (ovlp == NULL) continue;

        /* Recover the sock from the OVERLAPPED pointer. */
        ep_sock_t *sock = (ep_sock_t *)
            ((char *)ovlp - offsetof(ep_sock_t, overlapped));
        IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)ovlp;

        ep_sock_handle_completion(sock,
                                  port->iocp_entries[i].dwNumberOfBytesTransferred,
                                  iosb->Status);
    }

    /* Step 3: drain whatever the completions pushed onto the ready
     * queue. */
    delivered = ep_drain_to_buffer(port, out, maxevents);
#endif

    return delivered;
}
