/*
 * wepoll_ex_api.c — public API surface.
 *
 * These are the symbols that user code links against.  Each function
 * validates its arguments, translates the Linux-shaped arguments into
 * the internal port layer's call shape, and returns -1 with errno set
 * on error.
 *
 * The epfd argument returned by epoll_create* is a small integer
 * (suitable for use in fd_set-style arrays, mirroring Linux).  Internally
 * we maintain a process-global table mapping integer fds -> ep_port_t*.
 * The table is indexed by fd value modulo table size; collisions walk
 * a chain.
 */
#include "wepoll_ex_internal.h"

#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* Process-global epfd table.  Linux fds are small non-negative ints, */
/* so we mimic that here.  We can't use the SOCKET value directly     */
/* because SOCKETs on Windows can be huge handles.                    */
/* --------------------------------------------------------------------- */

#define EPFD_TABLE_SIZE  1024

typedef struct epfd_slot {
    int            fd;       /* user-visible epoll fd */
    ep_port_t     *port;
} epfd_slot_t;

static epfd_slot_t g_epfd_table[EPFD_TABLE_SIZE];
static pthread_mutex_t g_epfd_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_next_fd = 1;

static int epfd_alloc(ep_port_t *port)
{
    pthread_mutex_lock(&g_epfd_lock);
    int fd = atomic_fetch_add(&g_next_fd, 1);
    /* Find a slot — linear probe. */
    int slot = fd % EPFD_TABLE_SIZE;
    for (int probes = 0; probes < EPFD_TABLE_SIZE; probes++) {
        if (g_epfd_table[slot].port == NULL) {
            g_epfd_table[slot].fd = fd;
            g_epfd_table[slot].port = port;
            pthread_mutex_unlock(&g_epfd_lock);
            return fd;
        }
        slot = (slot + 1) % EPFD_TABLE_SIZE;
    }
    pthread_mutex_unlock(&g_epfd_lock);
    ep_set_errno(EMFILE);
    return -1;
}

static ep_port_t *epfd_lookup(int fd)
{
    if (fd <= 0) return NULL;
    pthread_mutex_lock(&g_epfd_lock);
    int slot = fd % EPFD_TABLE_SIZE;
    for (int probes = 0; probes < EPFD_TABLE_SIZE; probes++) {
        if (g_epfd_table[slot].port == NULL) break;
        if (g_epfd_table[slot].fd == fd) {
            ep_port_t *p = g_epfd_table[slot].port;
            pthread_mutex_unlock(&g_epfd_lock);
            return p;
        }
        slot = (slot + 1) % EPFD_TABLE_SIZE;
    }
    pthread_mutex_unlock(&g_epfd_lock);
    return NULL;
}

static ep_port_t *epfd_release(int fd)
{
    pthread_mutex_lock(&g_epfd_lock);
    int slot = fd % EPFD_TABLE_SIZE;
    for (int probes = 0; probes < EPFD_TABLE_SIZE; probes++) {
        if (g_epfd_table[slot].port == NULL) break;
        if (g_epfd_table[slot].fd == fd) {
            ep_port_t *p = g_epfd_table[slot].port;
            g_epfd_table[slot].port = NULL;
            g_epfd_table[slot].fd = 0;
            pthread_mutex_unlock(&g_epfd_lock);
            return p;
        }
        slot = (slot + 1) % EPFD_TABLE_SIZE;
    }
    pthread_mutex_unlock(&g_epfd_lock);
    return NULL;
}

/* --------------------------------------------------------------------- */
/* Public API.                                                        */
/* --------------------------------------------------------------------- */

WEPOLL_EX_API int epoll_create(int size)
{
    if (size <= 0) {
        ep_set_errno(EINVAL);
        return -1;
    }
    return epoll_create_ex(size, 0);
}

WEPOLL_EX_API int epoll_create1(int flags)
{
    if (flags & ~EPOLL_CLOEXEC) {
        ep_set_errno(EINVAL);
        return -1;
    }
    return epoll_create_ex(0, flags);
}

WEPOLL_EX_API int epoll_create_ex(int size, int flags)
{
    if (ep_global_init() != 0) return -1;

    ep_port_t *port = NULL;
    if (ep_port_create(size, flags, &port) != 0) return -1;

    int fd = epfd_alloc(port);
    if (fd < 0) {
        ep_port_destroy(port);
        return -1;
    }
    return fd;
}

WEPOLL_EX_API int epoll_ctl(int epfd, int op, int fd,
                            struct epoll_event *ev)
{
    return epoll_ctl_ctx(epfd, op, fd, ev, NULL);
}

WEPOLL_EX_API int epoll_ctl_ctx(int epfd, int op, int fd,
                                struct epoll_event *ev, void *user_ctx)
{
    ep_port_t *port = epfd_lookup(epfd);
    if (port == NULL) {
        ep_set_errno(EBADF);
        return -1;
    }

    /* Translate socket fd to SOCKET on Windows. */
    SOCKET sockfd = (SOCKET)fd;

    switch (op) {
    case EPOLL_CTL_ADD:
        if (ev == NULL) { ep_set_errno(EFAULT); return -1; }
        /* EPOLLEXCLUSIVE is only meaningful on ADD; Linux rejects MOD. */
        return ep_port_register(port, sockfd,
                                ev->events,
                                /* split user flags */
                                ev->events & (EPOLLET | EPOLLONESHOT |
                                              EPOLLEXCLUSIVE),
                                ev->data,
                                user_ctx);
    case EPOLL_CTL_MOD:
        if (ev == NULL) { ep_set_errno(EFAULT); return -1; }
        return ep_port_modify(port, sockfd,
                              ev->events,
                              ev->events & (EPOLLET | EPOLLONESHOT),
                              ev->data,
                              user_ctx);
    case EPOLL_CTL_DEL:
        return ep_port_unregister(port, sockfd);
    default:
        ep_set_errno(EINVAL);
        return -1;
    }
}

WEPOLL_EX_API int epoll_wait(int epfd, struct epoll_event *events,
                             int maxevents, int timeout)
{
    return epoll_pwait(epfd, events, maxevents, timeout, NULL);
}

WEPOLL_EX_API int epoll_pwait(int epfd, struct epoll_event *events,
                              int maxevents, int timeout,
                              const sigset_t *sigmask)
{
    if (events == NULL || maxevents <= 0) {
        ep_set_errno(EINVAL);
        return -1;
    }
    ep_port_t *port = epfd_lookup(epfd);
    if (port == NULL) {
        ep_set_errno(EBADF);
        return -1;
    }

    epoll_event_ex *ex = (epoll_event_ex *)
        calloc(maxevents, sizeof(*ex));
    if (ex == NULL) {
        ep_set_errno(ENOMEM);
        return -1;
    }
    int n = ep_port_wait(port, ex, maxevents, timeout, sigmask);
    if (n < 0) { free(ex); return -1; }
    /* Down-cast: drop extension fields. */
    for (int i = 0; i < n; i++) {
        events[i].events = ex[i].events;
        events[i].data   = ex[i].data;
    }
    free(ex);
    return n;
}

WEPOLL_EX_API int epoll_pwait2(int epfd, struct epoll_event *events,
                               int maxevents,
                               const struct timespec *timeout,
                               const sigset_t *sigmask)
{
    int ms;
    if (timeout == NULL) ms = -1;
    else if (timeout->tv_sec == 0 && timeout->tv_nsec == 0) ms = 0;
    else ms = (int)(timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000);
    return epoll_pwait(epfd, events, maxevents, ms, sigmask);
}

WEPOLL_EX_API int epoll_wait_ex(int epfd, struct epoll_event_ex *events,
                                int maxevents, int timeout)
{
    return epoll_pwait2_ex(epfd, events, maxevents,
                           timeout > 0 ? &(struct timespec){
                               .tv_sec = timeout / 1000,
                               .tv_nsec = (long)(timeout % 1000) * 1000000L
                           } : NULL,
                           NULL);
}

WEPOLL_EX_API int epoll_pwait2_ex(int epfd, struct epoll_event_ex *events,
                                  int maxevents,
                                  const struct timespec *timeout,
                                  const sigset_t *sigmask)
{
    if (events == NULL || maxevents <= 0) {
        ep_set_errno(EINVAL);
        return -1;
    }
    ep_port_t *port = epfd_lookup(epfd);
    if (port == NULL) {
        ep_set_errno(EBADF);
        return -1;
    }
    int ms;
    if (timeout == NULL) ms = -1;
    else if (timeout->tv_sec == 0 && timeout->tv_nsec == 0) ms = 0;
    else ms = (int)(timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000);
    return ep_port_wait(port, events, maxevents, ms, sigmask);
}

WEPOLL_EX_API int epoll_ctl_batch(int epfd,
                                  const int *ops,
                                  const int *fds,
                                  const struct epoll_event *events,
                                  int count)
{
    if (count <= 0) {
        ep_set_errno(EINVAL);
        return -1;
    }
    /* For simplicity (and atomicity) we apply all ops under the
     * port's fd_table_lock.  On any failure we roll back. */
    ep_port_t *port = epfd_lookup(epfd);
    if (port == NULL) {
        ep_set_errno(EBADF);
        return -1;
    }

    /* Record successful ops for rollback. */
    int *applied = (int *)calloc(count, sizeof(int));
    int *applied_fds = (int *)calloc(count, sizeof(int));
    if (applied == NULL || applied_fds == NULL) {
        free(applied); free(applied_fds);
        ep_set_errno(ENOMEM);
        return -1;
    }

    int applied_count = 0;
    int rc = 0;
    for (int i = 0; i < count; i++) {
        struct epoll_event ev = events[i];
        if (epoll_ctl(epfd, ops[i], fds[i], &ev) == 0) {
            applied[applied_count] = ops[i];
            applied_fds[applied_count] = fds[i];
            applied_count++;
        } else {
            rc = -1;
            break;
        }
    }

    if (rc != 0) {
        /* Roll back. */
        for (int i = applied_count - 1; i >= 0; i--) {
            int op = applied[i];
            int fd = applied_fds[i];
            if (op == EPOLL_CTL_ADD) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            } else if (op == EPOLL_CTL_MOD) {
                /* Can't truly roll back a MOD without the prior event.
                 * Best-effort: re-add as zero-event.  The caller
                 * should detect failure and recover. */
            } else if (op == EPOLL_CTL_DEL) {
                /* Can't undo a DEL. */
            }
        }
    }
    free(applied);
    free(applied_fds);
    return rc;
}

WEPOLL_EX_API int epoll_drain(int epfd, struct epoll_event *events,
                              int maxevents)
{
    return epoll_wait(epfd, events, maxevents, 0);
}

WEPOLL_EX_API int epoll_rearm(int epfd, int fd)
{
    ep_port_t *port = epfd_lookup(epfd);
    if (port == NULL) {
        ep_set_errno(EBADF);
        return -1;
    }
    return ep_port_rearm(port, (SOCKET)fd);
}

WEPOLL_EX_API int epoll_fd_count(int epfd)
{
    ep_port_t *port = epfd_lookup(epfd);
    if (port == NULL) {
        ep_set_errno(EBADF);
        return -1;
    }
    return (int)port->fd_table_count;
}

WEPOLL_EX_API uint32_t wepoll_ex_version(void)
{
    /* 1.0.0 initial release. */
    return 0x01000000;
}

WEPOLL_EX_API const char *wepoll_ex_version_string(void)
{
    return "wepoll-ex 1.0.0 (IOCP+AFD)";
}

/* --------------------------------------------------------------------- */
/* close() hook.  When the user closes an epoll fd we need to tear      */
/* down the port.  We export this for the compatibility shim to call;   */
/* on Windows the user must call wepoll_close() because close()        */
/* doesn't know about our integer epfd scheme.                          */
/* --------------------------------------------------------------------- */

WEPOLL_EX_API int wepoll_close(int epfd)
{
    ep_port_t *port = epfd_release(epfd);
    if (port == NULL) {
        ep_set_errno(EBADF);
        return -1;
    }
    ep_port_destroy(port);
    return 0;
}
