/*
 * wepoll_ex_posix.c — POSIX extension API implementation.
 *
 * On POSIX the basic epoll_create / epoll_ctl / epoll_wait family is
 * provided by the host libc.  wepoll-ex adds the extension API
 * (epoll_wait_ex, epoll_ctl_ctx, epoll_ctl_batch, epoll_rearm,
 * epoll_fd_count, wepoll_close) on top.
 *
 * To support per-fd metadata we maintain a per-epfd hash table keyed on
 * the registered fd.  Wait-time context resolution scans for a unique
 * epoll_data value because the union does not identify its active member.
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "wepoll_ex.h"
#include "wepoll_ex_internal.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <pthread.h>

typedef struct posix_sock_node {
    int                       fd;
    void                     *user_ctx;
    struct epoll_event        event;
    struct posix_sock_node   *next;
} posix_sock_node_t;

typedef struct posix_sock_snapshot {
    void                *user_ctx;
    struct epoll_event   event;
} posix_sock_snapshot_t;

typedef struct posix_port {
    int                  epfd;
    pthread_mutex_t      lock;
    pthread_cond_t       idle;
    posix_sock_node_t  **buckets;
    size_t               n_buckets;
    size_t               count;
    size_t               refs;
    uint64_t             generation;
    int                  closing;
    /* Stable duplicate used by metadata-owned epoll_ctl calls. */
    int                  control_epfd;
    int                  identity_fd;
    struct epoll_event   identity_event;
} posix_port_t;

#define POSIX_PORT_MAP_SIZE  1024
static pthread_mutex_t g_posix_lock = PTHREAD_MUTEX_INITIALIZER;
static posix_port_t   *g_posix_map[POSIX_PORT_MAP_SIZE];
static uint64_t        g_posix_generation = 1;

static posix_port_t *port_lookup_locked(int epfd)
{
    /* Linear search — there's usually only one or two epoll instances
     * per process.  We could use the epfd as a key into a hash but
     * the bookkeeping cost outweighs the benefit for typical nginx
     * configurations (one epoll per worker). */
    for (int i = 0; i < POSIX_PORT_MAP_SIZE; i++) {
        posix_port_t *p = g_posix_map[i];
        if (p && p->epfd == epfd) return p;
    }
    return NULL;
}

static void port_remove_locked(posix_port_t *p)
{
    for (int i = 0; i < POSIX_PORT_MAP_SIZE; i++) {
        if (g_posix_map[i] == p) {
            g_posix_map[i] = NULL;
            return;
        }
    }
}

/* Stop new users, wait for existing metadata users, and unlink the port.
 * The caller holds g_posix_lock and must free p after releasing it. */
static void port_retire_locked(posix_port_t *p)
{
    p->closing = 1;
    while (p->refs != 0) pthread_cond_wait(&p->idle, &g_posix_lock);
    port_remove_locked(p);
}

/* Validate that epfd refers to an epoll instance without changing its
 * interest list.  A newly created pipe endpoint cannot already be present in
 * any epoll set, so DEL returns ENOENT for epoll descriptors and EINVAL for
 * other open descriptor types. */
static int epfd_validate(int epfd)
{
    if (epfd < 0) {
        errno = EBADF;
        return -1;
    }
    if (fcntl(epfd, F_GETFD) == -1) return -1;

    int saved_errno = errno;
    int probe[2];
    if (pipe2(probe, O_CLOEXEC) != 0) return -1;

    int result = epoll_ctl(epfd, EPOLL_CTL_DEL, probe[0], NULL);
    int probe_errno = errno;
    (void)close(probe[0]);
    (void)close(probe[1]);

    if (result == -1 && probe_errno == ENOENT) {
        errno = saved_errno;
        return 0;
    }

    errno = result == 0 ? EINVAL : probe_errno;
    return -1;
}

/* A metadata port can outlive the integer descriptor if a caller uses plain
 * close() instead of wepoll_close().  Keep an unreadable eventfd registered in
 * the native epoll set so a later reuse of the same integer can be detected
 * without relying on non-portable fd identity APIs.  Return 1 when the fd is
 * an epoll instance, but not the one associated with p; return -1 for a
 * syscall/descriptor error. */
static int port_identity_check(posix_port_t *p, int epfd)
{
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, p->identity_fd,
                  &p->identity_event) == 0) {
        return 0;
    }

    if (errno == ENOENT) return 1;
    return -1;
}

static void port_free(posix_port_t *p)
{
    for (size_t b = 0; b < p->n_buckets; b++) {
        posix_sock_node_t *n = p->buckets[b];
        while (n) {
            posix_sock_node_t *next = n->next;
            free(n);
            n = next;
        }
    }
    free(p->buckets);
    if (p->control_epfd >= 0) (void)close(p->control_epfd);
    if (p->identity_fd >= 0) (void)close(p->identity_fd);
    pthread_cond_destroy(&p->idle);
    pthread_mutex_destroy(&p->lock);
    free(p);
}

/* Acquire a stable metadata-port reference.  g_posix_lock serializes port
 * creation with wepoll_close(), preventing close/reuse races while extension
 * operations are starting. */
static posix_port_t *port_acquire_or_create(int epfd)
{
    for (;;) {
        pthread_mutex_lock(&g_posix_lock);
        posix_port_t *p = port_lookup_locked(epfd);
        if (p) {
            if (p->closing) {
                pthread_mutex_unlock(&g_posix_lock);
                errno = EBADF;
                return NULL;
            }

            int identity_result = port_identity_check(p, epfd);
            if (identity_result < 0) {
                /* Keep the stale mapping for EINVAL/EBADF as a guard: a
                 * later wepoll_close() can retire it without closing a
                 * descriptor that reused this integer. */
                int saved_errno = errno;
                pthread_mutex_unlock(&g_posix_lock);
                errno = saved_errno;
                return NULL;
            }
            if (identity_result == 0) {
                p->refs++;
                pthread_mutex_unlock(&g_posix_lock);
                return p;
            }

            /* The integer no longer names the mapped epoll generation.
             * Retire the old metadata before validating or tracking the
             * current descriptor. */
            port_retire_locked(p);
            pthread_mutex_unlock(&g_posix_lock);
            port_free(p);
            continue;
        }

        if (epfd_validate(epfd) != 0) {
            pthread_mutex_unlock(&g_posix_lock);
            return NULL;
        }

        /* Find a free slot. */
        p = calloc(1, sizeof(*p));
        if (!p) {
            pthread_mutex_unlock(&g_posix_lock);
            errno = ENOMEM;
            return NULL;
        }
        p->epfd = epfd;
        p->control_epfd = -1;
        p->identity_fd = -1;
        int mutex_error = pthread_mutex_init(&p->lock, NULL);
        if (mutex_error != 0) {
            free(p);
            pthread_mutex_unlock(&g_posix_lock);
            errno = mutex_error;
            return NULL;
        }
        int cond_error = pthread_cond_init(&p->idle, NULL);
        if (cond_error != 0) {
            pthread_mutex_destroy(&p->lock);
            free(p);
            pthread_mutex_unlock(&g_posix_lock);
            errno = cond_error;
            return NULL;
        }
        p->n_buckets = 1024;
        p->buckets = calloc(p->n_buckets, sizeof(posix_sock_node_t *));
        if (!p->buckets) {
            pthread_cond_destroy(&p->idle);
            pthread_mutex_destroy(&p->lock);
            free(p);
            pthread_mutex_unlock(&g_posix_lock);
            errno = ENOMEM;
            return NULL;
        }

        p->control_epfd = fcntl(epfd, F_DUPFD_CLOEXEC, 0);
        if (p->control_epfd < 0) {
            int saved_errno = errno;
            port_free(p);
            pthread_mutex_unlock(&g_posix_lock);
            errno = saved_errno;
            return NULL;
        }

        p->identity_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (p->identity_fd < 0) {
            int saved_errno = errno;
            port_free(p);
            pthread_mutex_unlock(&g_posix_lock);
            errno = saved_errno;
            return NULL;
        }
        p->identity_event.events = EPOLLIN;
        p->identity_event.data.u64 = UINT64_C(0x5745504f4c4c4558);
        if (epoll_ctl(p->control_epfd, EPOLL_CTL_ADD, p->identity_fd,
                      &p->identity_event) != 0) {
            int saved_errno = errno;
            port_free(p);
            pthread_mutex_unlock(&g_posix_lock);
            errno = saved_errno;
            return NULL;
        }

        p->refs = 1;
        p->generation = g_posix_generation++;
        if (g_posix_generation == 0) g_posix_generation = 1;

        for (int i = 0; i < POSIX_PORT_MAP_SIZE; i++) {
            if (g_posix_map[i] == NULL) {
                g_posix_map[i] = p;
                pthread_mutex_unlock(&g_posix_lock);
                return p;
            }
        }
        /* No free slot — table full. */
        port_free(p);
        pthread_mutex_unlock(&g_posix_lock);
        errno = EMFILE;
        return NULL;
    }
}

static void port_release(posix_port_t *p)
{
    pthread_mutex_lock(&g_posix_lock);
    p->refs--;
    if (p->closing && p->refs == 0) pthread_cond_signal(&p->idle);
    pthread_mutex_unlock(&g_posix_lock);
}

static uint64_t port_generation(int epfd)
{
    pthread_mutex_lock(&g_posix_lock);
    posix_port_t *p = port_lookup_locked(epfd);
    uint64_t generation = 0;
    if (p && !p->closing && port_identity_check(p, epfd) == 0) {
        generation = p->generation;
    }
    pthread_mutex_unlock(&g_posix_lock);
    return generation;
}

static posix_port_t *port_acquire_generation(int epfd, uint64_t generation)
{
    if (generation == 0) return NULL;

    pthread_mutex_lock(&g_posix_lock);
    posix_port_t *p = port_lookup_locked(epfd);
    if (!p || p->closing || p->generation != generation ||
        port_identity_check(p, epfd) != 0) {
        pthread_mutex_unlock(&g_posix_lock);
        return NULL;
    }
    p->refs++;
    pthread_mutex_unlock(&g_posix_lock);
    return p;
}

static posix_sock_node_t *node_find_locked(posix_port_t *p, int fd,
                                           size_t *slot_out)
{
    size_t slot = (size_t)fd % p->n_buckets;
    if (slot_out) *slot_out = slot;
    posix_sock_node_t *n = p->buckets[slot];
    while (n) { if (n->fd == fd) break; n = n->next; }
    return n;
}

static void node_remove_locked(posix_port_t *p, int fd)
{
    size_t slot = (size_t)fd % p->n_buckets;
    posix_sock_node_t **pp = &p->buckets[slot];
    while (*pp) {
        if ((*pp)->fd == fd) {
            posix_sock_node_t *dead = *pp;
            *pp = dead->next;
            free(dead);
            p->count--;
            break;
        }
        pp = &(*pp)->next;
    }
}

/* Keep the native interest list and the extension metadata synchronized.
 * The metadata lock also serializes control operations on the same port, so
 * ADD/DEL races cannot leave a stale node behind. */
static int port_ctl_ctx(posix_port_t *p, int op, int fd,
                        struct epoll_event *event, void *user_ctx)
{
    posix_sock_node_t *reserved = NULL;

    pthread_mutex_lock(&p->lock);
    size_t slot = 0;
    posix_sock_node_t *n = NULL;
    if (fd >= 0) n = node_find_locked(p, fd, &slot);

    /* A successful MOD of a registration made through native epoll_ctl still
     * needs a metadata node.  Reserve it before changing native state so an
     * allocation failure cannot leave the two views inconsistent. */
    if (op == EPOLL_CTL_MOD && event && fd >= 0 && !n) {
        reserved = calloc(1, sizeof(*reserved));
        if (!reserved) {
            pthread_mutex_unlock(&p->lock);
            errno = ENOMEM;
            return -1;
        }
        reserved->fd = fd;
    }

    if (epoll_ctl(p->control_epfd, op, fd,
                  op == EPOLL_CTL_DEL ? NULL : event) != 0) {
        int saved_errno = errno;
        free(reserved);
        pthread_mutex_unlock(&p->lock);
        errno = saved_errno;
        return -1;
    }

    if (op == EPOLL_CTL_DEL) {
        node_remove_locked(p, fd);
    } else {
        if (!n) {
            n = reserved;
            reserved = NULL;
            if (!n) {
                n = calloc(1, sizeof(*n));
                if (!n) {
                    int saved_errno = ENOMEM;
                    (void)epoll_ctl(p->control_epfd, EPOLL_CTL_DEL,
                                    fd, NULL);
                    pthread_mutex_unlock(&p->lock);
                    errno = saved_errno;
                    return -1;
                }
                n->fd = fd;
            }
            n->next = p->buckets[slot];
            p->buckets[slot] = n;
            p->count++;
        }
        n->event = *event;
        n->user_ctx = user_ctx;
    }

    free(reserved);
    pthread_mutex_unlock(&p->lock);
    return 0;
}

static int node_snapshot_by_data(posix_port_t *p, epoll_data_t data,
                                 posix_sock_snapshot_t *snapshot)
{
    int matches = 0;

    /* epoll_data_t has no tag telling us which union member the caller used.
     * Compare the opaque 64-bit value and only accept a unique match.  This
     * supports fd, ptr, and u64 payloads without dereferencing ptr values. */
    pthread_mutex_lock(&p->lock);
    for (size_t slot = 0; slot < p->n_buckets && matches < 2; slot++) {
        for (posix_sock_node_t *n = p->buckets[slot]; n; n = n->next) {
            if (n->event.data.u64 == data.u64) {
                snapshot->user_ctx = n->user_ctx;
                snapshot->event = n->event;
                matches++;
                if (matches == 2) break;
            }
        }
    }
    pthread_mutex_unlock(&p->lock);
    return matches == 1;
}

static int port_count(posix_port_t *p)
{
    pthread_mutex_lock(&p->lock);
    size_t count = p->count;
    pthread_mutex_unlock(&p->lock);

    if (count > (size_t)INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (int)count;
}

static int port_rearm(posix_port_t *p, int fd)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (fcntl(fd, F_GETFD) == -1) {
        return -1;
    }

    pthread_mutex_lock(&p->lock);
    posix_sock_node_t *n = NULL;
    n = node_find_locked(p, fd, NULL);
    if (!n) {
        pthread_mutex_unlock(&p->lock);
        errno = ENOENT;
        return -1;
    }

    struct epoll_event event = n->event;
    int result = epoll_ctl(p->control_epfd, EPOLL_CTL_MOD, fd, &event);
    int saved_errno = errno;
    pthread_mutex_unlock(&p->lock);
    if (result != 0) errno = saved_errno;
    return result;
}

static int timeout_to_milliseconds(const struct timespec *timeout, int *out)
{
    if (timeout == NULL) {
        *out = -1;
        return 0;
    }
    if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
        timeout->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    if (timeout->tv_sec > INT_MAX / 1000) {
        errno = EOVERFLOW;
        return -1;
    }

    int64_t milliseconds = (int64_t)timeout->tv_sec * 1000;
    if (timeout->tv_nsec != 0) {
        milliseconds += (timeout->tv_nsec + 999999L) / 1000000L;
    }
    if (milliseconds > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    *out = (int)milliseconds;
    return 0;
}

static int posix_wait_ex(int epfd, struct epoll_event_ex *events,
                         int maxevents, int timeout,
                         const sigset_t *sigmask)
{
    if (!events || maxevents <= 0) {
        errno = EINVAL;
        return -1;
    }

    /* Remember which metadata generation belongs to the descriptor before
     * entering the wait.  If another thread closes and reuses this fd number,
     * events from the old wait must never acquire context from the new port. */
    uint64_t generation = port_generation(epfd);

    struct epoll_event *kevs = calloc((size_t)maxevents, sizeof(*kevs));
    if (!kevs) {
        errno = ENOMEM;
        return -1;
    }

    int n;
    if (sigmask) {
        n = epoll_pwait(epfd, kevs, maxevents, timeout, sigmask);
    } else {
        n = epoll_wait(epfd, kevs, maxevents, timeout);
    }
    if (n < 0) {
        int saved_errno = errno;
        free(kevs);
        errno = saved_errno;
        return -1;
    }

    posix_port_t *p = port_acquire_generation(epfd, generation);
    struct timespec ts = {0};
    uint64_t now_ns = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        now_ns = (uint64_t)ts.tv_sec * 1000000000ULL +
                 (uint64_t)ts.tv_nsec;
    }

    for (int i = 0; i < n; i++) {
        events[i].events = kevs[i].events;
        events[i].data = kevs[i].data;
        events[i].flags = 0;
        events[i].timestamp = now_ns;
        events[i].user_ctx = NULL;

        if (p) {
            posix_sock_snapshot_t snapshot;
            if (node_snapshot_by_data(p, kevs[i].data, &snapshot)) {
                events[i].user_ctx = snapshot.user_ctx;
                if (snapshot.event.events & EPOLLET) {
                    events[i].flags |= WEPOLL_FLAG_ET_DELIVERED |
                                       WEPOLL_FLAG_EDGE_ARMED;
                }
                if (snapshot.event.events & EPOLLONESHOT) {
                    events[i].flags |= WEPOLL_FLAG_ONESHOT_FIRED;
                }
            }
        }
    }

    if (p) port_release(p);
    free(kevs);
    return n;
}

/* --------------------------------------------------------------------- */
/* Public extension API — POSIX implementation.                      */
/* --------------------------------------------------------------------- */

WEPOLL_EX_API int epoll_create_ex(int size, int flags)
{
    if (size < 0 || (flags & ~EPOLL_CLOEXEC) != 0) {
        errno = EINVAL;
        return -1;
    }

    int epfd = epoll_create1(flags);
    if (epfd < 0) return -1;

    posix_port_t *p = port_acquire_or_create(epfd);
    if (!p) {
        int saved_errno = errno;
        close(epfd);
        errno = saved_errno;
        return -1;
    }
    port_release(p);
    return epfd;
}

WEPOLL_EX_API int epoll_ctl_ctx(int epfd, int op, int fd,
                                struct epoll_event *ev, void *user_ctx)
{
    posix_port_t *p = port_acquire_or_create(epfd);
    if (!p) return -1;

    int result = port_ctl_ctx(p, op, fd, ev, user_ctx);
    int saved_errno = errno;
    port_release(p);
    if (result != 0) errno = saved_errno;
    return result;
}

WEPOLL_EX_API int epoll_wait_ex(int epfd, struct epoll_event_ex *events,
                                int maxevents, int timeout)
{
    return posix_wait_ex(epfd, events, maxevents, timeout, NULL);
}

WEPOLL_EX_API int epoll_pwait2_ex(int epfd, struct epoll_event_ex *events,
                                  int maxevents,
                                  const struct timespec *timeout,
                                  const sigset_t *sigmask)
{
    int ms;
    if (timeout_to_milliseconds(timeout, &ms) != 0) return -1;
    return posix_wait_ex(epfd, events, maxevents, ms, sigmask);
}

WEPOLL_EX_API int epoll_ctl_batch(int epfd, const int *ops,
                                  const int *fds,
                                  const struct epoll_event *events,
                                  int count)
{
    if (count <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (!ops || !fds) {
        errno = EFAULT;
        return -1;
    }

    /* DEL does not consume an event entry.  Permit a NULL events array for a
     * batch made entirely of DEL operations, while still requiring it for
     * every ADD/MOD operation. */
    for (int i = 0; i < count; i++) {
        if (ops[i] != EPOLL_CTL_ADD && ops[i] != EPOLL_CTL_MOD &&
            ops[i] != EPOLL_CTL_DEL) {
            errno = EINVAL;
            return -1;
        }
        if ((ops[i] == EPOLL_CTL_ADD || ops[i] == EPOLL_CTL_MOD) &&
            events == NULL) {
            errno = EFAULT;
            return -1;
        }
    }

    posix_port_t *p = port_acquire_or_create(epfd);
    if (!p) return -1;

    int applied = 0;
    for (int i = 0; i < count; i++) {
        struct epoll_event ev;
        struct epoll_event *event = NULL;
        if (ops[i] == EPOLL_CTL_ADD || ops[i] == EPOLL_CTL_MOD) {
            ev = events[i];
            event = &ev;
        }
        if (port_ctl_ctx(p, ops[i], fds[i], event, NULL) == 0) {
            applied++;
        } else {
            int saved_errno = errno;

            /* Best-effort rollback can safely undo successful ADDs.  MOD and
             * DEL lack their prior state in this API and remain applied. */
            for (int j = applied - 1; j >= 0; j--) {
                if (ops[j] == EPOLL_CTL_ADD) {
                    (void)port_ctl_ctx(p, EPOLL_CTL_DEL, fds[j], NULL, NULL);
                }
            }
            port_release(p);
            errno = saved_errno;
            return -1;
        }
    }
    port_release(p);
    return 0;
}

WEPOLL_EX_API int epoll_drain(int epfd, struct epoll_event *events,
                              int maxevents)
{
    return epoll_wait(epfd, events, maxevents, 0);
}

WEPOLL_EX_API int epoll_rearm(int epfd, int fd)
{
    posix_port_t *p = port_acquire_or_create(epfd);
    if (!p) return -1;

    int result = port_rearm(p, fd);
    int saved_errno = errno;
    port_release(p);
    if (result != 0) errno = saved_errno;
    return result;
}

WEPOLL_EX_API int epoll_fd_count(int epfd)
{
    posix_port_t *p = port_acquire_or_create(epfd);
    if (!p) return -1;

    int result = port_count(p);
    int saved_errno = errno;
    port_release(p);
    if (result < 0) errno = saved_errno;
    return result;
}

WEPOLL_EX_API uint32_t wepoll_ex_version(void)        { return 0x00010000; }
WEPOLL_EX_API const char *wepoll_ex_version_string(void) {
    return "wepoll-ex 1.0.0 (POSIX wrapper)";
}

WEPOLL_EX_API int wepoll_close(int epfd)
{
    pthread_mutex_lock(&g_posix_lock);
    posix_port_t *p = port_lookup_locked(epfd);

    if (p) {
        if (p->closing) {
            pthread_mutex_unlock(&g_posix_lock);
            errno = EBADF;
            return -1;
        }

        int identity_result = port_identity_check(p, epfd);
        int identity_errno = errno;
        if (identity_result != 0) {
            if (identity_result < 0 && identity_errno != EBADF &&
                identity_errno != EINVAL) {
                pthread_mutex_unlock(&g_posix_lock);
                errno = identity_errno;
                return -1;
            }

            /* The tracked epoll descriptor was closed natively and its
             * integer was either invalid or reused.  Retire only the stale
             * metadata; never close the replacement descriptor. */
            port_retire_locked(p);
            pthread_mutex_unlock(&g_posix_lock);
            port_free(p);
            errno = EBADF;
            return -1;
        }

        p->closing = 1;
    }

    /* Keep the registry lock across close so no extension call can validate
     * the old descriptor and create a new metadata port in the final gap.
     * Existing metadata users operate on control_epfd, so closing the public
     * fd before waiting for them also narrows the native close/reuse window. */
    int result = close(epfd);
    int saved_errno = errno;
    if (p) {
        while (p->refs != 0) pthread_cond_wait(&p->idle, &g_posix_lock);
        port_remove_locked(p);
    }
    pthread_mutex_unlock(&g_posix_lock);

    if (p) port_free(p);
    if (result != 0) errno = saved_errno;
    return result;
}

/* --------------------------------------------------------------------- */
/* Stubs for symbols referenced by the shared pool / port code.        */
/* On POSIX the actual engine (wepoll_ex_port.c) is not compiled, but  */
/* wepoll_ex_pool.c is shared and references these helpers.  Provide   */
/* minimal POSIX implementations.                                       */
/* --------------------------------------------------------------------- */
void ep_set_errno(int e) { errno = e; }
int  ep_last_err(void)   { return errno; }
int  ep_winerr_to_errno(DWORD w) { (void)w; return EINVAL; }
int  ep_status_to_errno(NTSTATUS s) { (void)s; return EINVAL; }

/* These are never called on POSIX but are referenced by the header. */
ep_ntdll_t g_ntdll = {0};
int  ep_global_init(void)            { g_ntdll.initialized = 1; return 0; }
void ep_global_fini(void)            { g_ntdll.initialized = 0; }

/* Windows-only entry points — provide no-op stubs so the linker is
 * happy if a static lib references them. */
int  ep_port_create(int s, int f, ep_port_t **o)  { (void)s;(void)f;(void)o; errno=ENOSYS; return -1; }
void ep_port_destroy(ep_port_t *p)                 { (void)p; }
int  ep_port_register(ep_port_t *p, SOCKET f, uint32_t e, uint32_t fl,
                      epoll_data_t d, void *c)     { (void)p;(void)f;(void)e;(void)fl;(void)d;(void)c; errno=ENOSYS; return -1; }
int  ep_port_modify(ep_port_t *p, SOCKET f, uint32_t e, uint32_t fl,
                    epoll_data_t d, void *c)       { (void)p;(void)f;(void)e;(void)fl;(void)d;(void)c; errno=ENOSYS; return -1; }
int  ep_port_unregister(ep_port_t *p, SOCKET f)    { (void)p;(void)f; errno=ENOSYS; return -1; }
int  ep_port_rearm(ep_port_t *p, SOCKET f)         { (void)p;(void)f; errno=ENOSYS; return -1; }
int  ep_port_wait(ep_port_t *p, epoll_event_ex *o, int m, int t, const sigset_t *s)
                                                    { (void)p;(void)o;(void)m;(void)t;(void)s; errno=ENOSYS; return -1; }
void ep_sock_handle_completion(ep_sock_t *s, DWORD b, NTSTATUS st) { (void)s;(void)b;(void)st; }
DWORD RtlNtStatusToDosError(NTSTATUS s)             { (void)s; return 0; }
