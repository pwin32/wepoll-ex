/*
 * wepoll_ex_posix.c — POSIX extension API implementation.
 *
 * On POSIX the basic epoll_create / epoll_ctl / epoll_wait family is
 * provided by the host libc.  wepoll-ex adds the extension API
 * (epoll_wait_ex, epoll_ctl_ctx, epoll_ctl_batch, epoll_rearm,
 * epoll_fd_count, wepoll_close) on top.
 *
 * To support per-registration metadata we maintain per-epfd fd and data
 * indexes.  A reused numeric fd can own multiple nodes when Linux keeps
 * registrations for distinct open file descriptions.  Wait-time context
 * resolution accepts only a unique opaque epoll_data value because the union
 * does not identify its active member.
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
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>

typedef struct posix_fd_identity {
    uint64_t device;
    uint64_t inode;
    uint64_t mode;
    uint64_t rdevice;
} posix_fd_identity_t;

typedef struct posix_sock_node {
    int                       fd;
    posix_fd_identity_t       identity;
    void                     *user_ctx;
    struct epoll_event        event;
    struct posix_sock_node   *next;
    struct posix_sock_node   *data_next;
    uint64_t                  data_key;
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
    posix_sock_node_t  **data_buckets;
    size_t               n_buckets;
    size_t               count;
    size_t               refs;
    /* Incremented after every extension metadata mutation.  A wait that
     * overlaps a mutation must not guess which registration version produced
     * the native event, because epoll_event carries no generation tag. */
    uint64_t             metadata_generation;
    int                  closing;
    /* Stable duplicate used by metadata-owned epoll_ctl calls. */
    int                  control_epfd;
    int                  identity_fd;
    struct epoll_event   identity_event;
} posix_port_t;

#define POSIX_PORT_MAP_SIZE  1024
#define POSIX_WAKE_TOKEN     UINT64_C(0x5745504f4c4c4558)

/* Native events are written into the packed prefix of the wider caller
 * array, then expanded from back to front.  The size/alignment relationship
 * keeps the native destination valid and guarantees that each wider write
 * starts after every packed source record that is still needed. */
_Static_assert(sizeof(struct epoll_event_ex) >=
                   sizeof(struct epoll_event),
               "epoll_event_ex must not be smaller than epoll_event");
_Static_assert(_Alignof(struct epoll_event_ex) >=
                   _Alignof(struct epoll_event),
               "epoll_event_ex must satisfy epoll_event alignment");

static pthread_mutex_t g_posix_lock = PTHREAD_MUTEX_INITIALIZER;
static posix_port_t   *g_posix_map[POSIX_PORT_MAP_SIZE];

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

/* Wake every extension wait which is using this metadata port.  The eventfd
 * is deliberately left readable: unlike a pipe, an eventfd in level-triggered
 * mode keeps waking other waiters until the port is retired.  EAGAIN means a
 * previous wake is already pending and is therefore success for our purpose. */
static void port_wake_waiters_locked(posix_port_t *p)
{
    uint64_t one = 1;
    int saved_errno = errno;

    for (;;) {
        ssize_t result = write(p->identity_fd, &one, sizeof(one));
        if (result == (ssize_t)sizeof(one) ||
            (result < 0 && errno == EAGAIN)) {
            break;
        }
        if (result < 0 && errno == EINTR) continue;
        /* The descriptor is an internal nonblocking eventfd.  There is no
         * useful recovery path for another error; preserve the caller's
         * errno and let the normal close/refcount path finish teardown. */
        break;
    }
    errno = saved_errno;
}

/* Stop new users, wait for existing metadata users, and unlink the port.
 * The caller holds g_posix_lock and must free p after releasing it. */
static void port_retire_locked(posix_port_t *p)
{
    p->closing = 1;
    port_wake_waiters_locked(p);
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
    free(p->data_buckets);
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
        p->data_buckets = calloc(p->n_buckets,
                                 sizeof(posix_sock_node_t *));
        if (!p->data_buckets) {
            free(p->buckets);
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
        p->identity_event.data.u64 = POSIX_WAKE_TOKEN;
        if (epoll_ctl(p->control_epfd, EPOLL_CTL_ADD, p->identity_fd,
                      &p->identity_event) != 0) {
            int saved_errno = errno;
            port_free(p);
            pthread_mutex_unlock(&g_posix_lock);
            errno = saved_errno;
            return NULL;
        }

        p->refs = 1;
        p->metadata_generation = 1;

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

static int fd_identity_capture(int fd, posix_fd_identity_t *identity)
{
    struct stat statbuf;

    if (fstat(fd, &statbuf) != 0) return -1;
    identity->device = (uint64_t)statbuf.st_dev;
    identity->inode = (uint64_t)statbuf.st_ino;
    identity->mode = (uint64_t)(statbuf.st_mode & S_IFMT);
    identity->rdevice = (uint64_t)statbuf.st_rdev;
    return 0;
}

static int fd_identity_equal(const posix_fd_identity_t *left,
                             const posix_fd_identity_t *right)
{
    return left->device == right->device &&
           left->inode == right->inode &&
           left->mode == right->mode &&
           left->rdevice == right->rdevice;
}

/* Find the metadata node for the current open file description.  POSIX does
 * not expose a portable open-file-description handle, so the wrapper keeps a
 * lightweight fstat fingerprint.  Socket and pipe identities are stable on
 * Linux; if a filesystem/device presents the same fingerprint for multiple
 * live registrations, the caller receives EOPNOTSUPP instead of mutating an
 * arbitrary node. */
static posix_sock_node_t *node_find_identity_locked(
    posix_port_t *p, int fd, const posix_fd_identity_t *identity,
    int *match_count, size_t *slot_out)
{
    size_t slot = (size_t)fd % p->n_buckets;
    int matches = 0;
    posix_sock_node_t *match = NULL;

    if (slot_out) *slot_out = slot;
    for (posix_sock_node_t *n = p->buckets[slot]; n != NULL;
         n = n->next) {
        if (n->fd != fd) continue;
        if (fd_identity_equal(&n->identity, identity)) {
            match = n;
            matches++;
        }
    }
    if (match_count) *match_count = matches;
    return matches == 1 ? match : NULL;
}

static size_t node_data_slot(const posix_port_t *p, uint64_t data_key)
{
    uint64_t mixed = data_key ^ (data_key >> 32);
    return (size_t)(mixed % p->n_buckets);
}

static void node_data_insert_locked(posix_port_t *p,
                                    posix_sock_node_t *node)
{
    node->data_key = node->event.data.u64;
    size_t slot = node_data_slot(p, node->data_key);
    node->data_next = p->data_buckets[slot];
    p->data_buckets[slot] = node;
}

static void node_data_remove_locked(posix_port_t *p,
                                    posix_sock_node_t *node)
{
    size_t slot = node_data_slot(p, node->data_key);
    posix_sock_node_t **link = &p->data_buckets[slot];
    while (*link != NULL) {
        if (*link == node) {
            *link = node->data_next;
            node->data_next = NULL;
            return;
        }
        link = &(*link)->data_next;
    }
}

static int node_remove_ptr_locked(posix_port_t *p, posix_sock_node_t *node)
{
    size_t slot = (size_t)node->fd % p->n_buckets;
    posix_sock_node_t **pp = &p->buckets[slot];
    while (*pp) {
        if (*pp == node) {
            posix_sock_node_t *dead = node;
            *pp = dead->next;
            node_data_remove_locked(p, dead);
            free(dead);
            p->count--;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

/* Keep the native interest list and the extension metadata synchronized.
 * The metadata lock also serializes control operations on the same port, so
 * ADD/DEL races cannot leave a stale node behind.  Linux epoll keys a native
 * registration by both the numeric fd and its open file description.  A
 * close/reuse sequence can therefore leave multiple live registrations with
 * one integer fd; successful ADDs retain one metadata node per registration.
 * MOD/DEL select by the current descriptor's fstat fingerprint and reject a
 * fingerprint collision rather than mutating an arbitrary node. */
static int port_ctl_ctx(posix_port_t *p, int op, int fd,
                        struct epoll_event *event, void *user_ctx)
{
    posix_sock_node_t *reserved = NULL;
    posix_fd_identity_t identity;
    int identity_matches = 0;

    pthread_mutex_lock(&p->lock);
    if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD &&
        op != EPOLL_CTL_DEL) {
        pthread_mutex_unlock(&p->lock);
        errno = EINVAL;
        return -1;
    }
    if (op != EPOLL_CTL_DEL && event == NULL) {
        pthread_mutex_unlock(&p->lock);
        errno = EFAULT;
        return -1;
    }

    if (fd_identity_capture(fd, &identity) != 0) {
        int saved_errno = errno;
        pthread_mutex_unlock(&p->lock);
        errno = saved_errno;
        return -1;
    }

    size_t slot = 0;
    posix_sock_node_t *n = NULL;
    if (op != EPOLL_CTL_ADD) {
        n = node_find_identity_locked(p, fd, &identity,
                                      &identity_matches, &slot);
        if (identity_matches > 1) {
            pthread_mutex_unlock(&p->lock);
            errno = EOPNOTSUPP;
            return -1;
        }
    } else {
        slot = (size_t)fd % p->n_buckets;
    }

    /* A successful MOD of a registration made through native epoll_ctl still
     * needs a metadata node.  Reserve it before changing native state so an
     * allocation failure cannot leave the two views inconsistent.  A stale
     * node for a prior open file description is intentionally retained; a
     * successful MOD then adopts the current native registration as a new
     * metadata node. */
    if (op == EPOLL_CTL_MOD && event && fd >= 0 && !n) {
        reserved = calloc(1, sizeof(*reserved));
        if (!reserved) {
            pthread_mutex_unlock(&p->lock);
            errno = ENOMEM;
            return -1;
        }
        reserved->fd = fd;
        reserved->identity = identity;
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
        /* If the current fd has no matching metadata (for example, a native
         * registration after fd reuse), leave stale nodes untouched. */
        if (n != NULL && node_remove_ptr_locked(p, n)) {
            p->metadata_generation++;
            if (p->metadata_generation == 0) p->metadata_generation = 1;
        }
    } else {
        /* ADD always represents a new native registration, even when the
         * integer fd is already present for a different open description. */
        if (op == EPOLL_CTL_ADD || !n) {
            n = reserved;
            reserved = NULL;
            if (!n) n = calloc(1, sizeof(*n));
            if (!n) {
                int saved_errno = ENOMEM;
                (void)epoll_ctl(p->control_epfd, EPOLL_CTL_DEL,
                                fd, NULL);
                pthread_mutex_unlock(&p->lock);
                errno = saved_errno;
                return -1;
            }
            n->fd = fd;
            n->identity = identity;
            n->next = p->buckets[slot];
            p->buckets[slot] = n;
            p->count++;
        }
        node_data_remove_locked(p, n);
        n->event = *event;
        n->user_ctx = user_ctx;
        node_data_insert_locked(p, n);
        p->metadata_generation++;
        if (p->metadata_generation == 0) p->metadata_generation = 1;
    }

    free(reserved);
    pthread_mutex_unlock(&p->lock);
    return 0;
}

static int node_snapshot_by_data_locked(posix_port_t *p, epoll_data_t data,
                                        posix_sock_snapshot_t *snapshot)
{
    int matches = 0;

    /* epoll_data_t has no tag telling us which union member the caller used.
     * Compare the opaque 64-bit value and only accept a unique match.  The
     * reverse index limits this search to nodes carrying that value while
     * still preserving the duplicate-data ambiguity rule. */
    size_t slot = node_data_slot(p, data.u64);
    for (posix_sock_node_t *n = p->data_buckets[slot];
         n != NULL && matches < 2; n = n->data_next) {
        if (n->data_key == data.u64) {
            snapshot->user_ctx = n->user_ctx;
            snapshot->event = n->event;
            matches++;
        }
    }
    return matches == 1;
}

static int port_is_closing(posix_port_t *p)
{
    pthread_mutex_lock(&g_posix_lock);
    int closing = p->closing;
    pthread_mutex_unlock(&g_posix_lock);
    return closing;
}

static int port_count(posix_port_t *p)
{
    /* POSIX exposes no non-destructive API to enumerate an epoll set.  The
     * extension layer therefore counts registrations it owns (ADD/MOD via
     * epoll_ctl_ctx); native epoll_ctl callers are outside this metadata
     * view until an extension MOD adopts the registration. */
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
    posix_fd_identity_t identity;
    int identity_matches = 0;

    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (fd_identity_capture(fd, &identity) != 0) return -1;

    pthread_mutex_lock(&p->lock);
    posix_sock_node_t *n = node_find_identity_locked(
        p, fd, &identity, &identity_matches, NULL);
    if (identity_matches > 1) {
        pthread_mutex_unlock(&p->lock);
        errno = EOPNOTSUPP;
        return -1;
    }
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

static int validate_timespec_timeout(const struct timespec *timeout)
{
    if (timeout == NULL) {
        return 0;
    }
    if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
        timeout->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int timeout_to_millisecond_chunk(const struct timespec *timeout,
                                        int *out, int *has_more)
{
    if (validate_timespec_timeout(timeout) != 0) return -1;
    if (timeout == NULL) {
        *out = -1;
        *has_more = 0;
        return 0;
    }

    const time_t max_seconds = INT_MAX / 1000;
    const long max_nanoseconds =
        (long)(INT_MAX % 1000) * 1000000L;
    if (timeout->tv_sec > max_seconds ||
        (timeout->tv_sec == max_seconds &&
         timeout->tv_nsec > max_nanoseconds)) {
        *out = INT_MAX;
        *has_more = 1;
        return 0;
    }

    int64_t milliseconds = (int64_t)timeout->tv_sec * 1000;
    if (timeout->tv_nsec != 0) {
        milliseconds += (timeout->tv_nsec + 999999L) / 1000000L;
    }
    *out = (int)milliseconds;
    *has_more = 0;
    return 0;
}

static void timeout_subtract_millisecond_chunk(struct timespec *timeout,
                                               int milliseconds)
{
    time_t seconds = milliseconds / 1000;
    long nanoseconds = (long)(milliseconds % 1000) * 1000000L;

    timeout->tv_sec -= seconds;
    if (timeout->tv_nsec < nanoseconds) {
        timeout->tv_sec--;
        timeout->tv_nsec += 1000000000L;
    }
    timeout->tv_nsec -= nanoseconds;
}

static int posix_millisecond_wait(int epfd, struct epoll_event *events,
                                  int maxevents, int timeout_ms,
                                  const sigset_t *sigmask)
{
    if (sigmask) {
        return epoll_pwait(epfd, events, maxevents, timeout_ms, sigmask);
    }
    return epoll_wait(epfd, events, maxevents, timeout_ms);
}

static int posix_timespec_wait_chunks(int epfd, struct epoll_event *events,
                                      int maxevents,
                                      struct timespec remaining,
                                      const sigset_t *sigmask)
{
    /* The millisecond APIs accept only an int timeout.  Split longer valid
     * timespecs into bounded waits, advancing only after a chunk times out so
     * readiness, EINTR, and descriptor errors are returned unchanged. */
    for (;;) {
        int timeout_ms;
        int has_more;
        if (timeout_to_millisecond_chunk(&remaining, &timeout_ms,
                                         &has_more) != 0) {
            return -1;
        }

        int result = posix_millisecond_wait(epfd, events, maxevents,
                                            timeout_ms, sigmask);
        if (result != 0 || !has_more) return result;

        timeout_subtract_millisecond_chunk(&remaining, timeout_ms);
    }
}

typedef struct posix_signal_mask_cleanup {
    sigset_t previous;
    int error;
} posix_signal_mask_cleanup_t;

static void posix_signal_mask_restore(void *arg)
{
    posix_signal_mask_cleanup_t *cleanup = arg;

    cleanup->error = pthread_sigmask(SIG_SETMASK, &cleanup->previous, NULL);
}

static int posix_timespec_wait_fallback(int epfd, struct epoll_event *events,
                                        int maxevents,
                                        const struct timespec *timeout,
                                        const sigset_t *sigmask)
{
    if (validate_timespec_timeout(timeout) != 0) return -1;
    if (timeout == NULL) {
        return posix_millisecond_wait(epfd, events, maxevents, -1, sigmask);
    }

    int timeout_ms;
    int has_more;
    if (timeout_to_millisecond_chunk(timeout, &timeout_ms, &has_more) != 0) {
        return -1;
    }
    if (!has_more) {
        return posix_millisecond_wait(epfd, events, maxevents, timeout_ms,
                                      sigmask);
    }
    if (!sigmask) {
        return posix_timespec_wait_chunks(epfd, events, maxevents, *timeout,
                                          NULL);
    }

    /* epoll_pwait restores the caller's mask after every syscall.  Block all
     * catchable signals between chunks, then let each epoll_pwait atomically
     * install the requested mask.  This prevents an inter-chunk delivery from
     * being consumed without interrupting the logical epoll_pwait2 wait. */
    sigset_t blocked_mask;
    if (sigfillset(&blocked_mask) != 0) return -1;

    posix_signal_mask_cleanup_t mask_cleanup = { .error = 0 };
    int mask_error = pthread_sigmask(SIG_SETMASK, &blocked_mask,
                                     &mask_cleanup.previous);
    if (mask_error != 0) {
        errno = mask_error;
        return -1;
    }

    int result;
    int saved_errno;
    pthread_cleanup_push(posix_signal_mask_restore, &mask_cleanup);
    result = posix_timespec_wait_chunks(epfd, events, maxevents, *timeout,
                                        sigmask);
    saved_errno = errno;
    pthread_cleanup_pop(1);

    if (mask_cleanup.error != 0) {
        errno = mask_cleanup.error;
        return -1;
    }
    errno = saved_errno;
    return result;
}

typedef struct posix_wait_cleanup {
    posix_port_t *port;
} posix_wait_cleanup_t;

/* epoll_wait and epoll_pwait are pthread cancellation points on Linux.  A
 * cancelled extended wait must release its metadata-port reference,
 * otherwise a later wepoll_close() waits forever for a reference that no
 * thread can release. */
static void posix_wait_cancel_cleanup(void *arg)
{
    posix_wait_cleanup_t *cleanup = arg;

    port_release(cleanup->port);
}

static int posix_native_wait(int epfd, void *events,
                             int maxevents, int timeout_ms,
                             const struct timespec *timeout,
                             int use_timespec, const sigset_t *sigmask)
{
    if (use_timespec) {
#if defined(WEPOLL_EX_HAVE_EPOLL_PWAIT2)
        int result = epoll_pwait2(epfd, events, maxevents, timeout, sigmask);
        if (result >= 0 || errno != ENOSYS) return result;

        /* A new libc can run on a kernel predating epoll_pwait2.  Preserve
         * atomic signal-mask handling while falling back to millisecond
         * timeout precision in that configuration. */
#endif
        return posix_timespec_wait_fallback(epfd, events, maxevents,
                                            timeout, sigmask);
    }

    return posix_millisecond_wait(epfd, events, maxevents, timeout_ms,
                                  sigmask);
}

static int posix_wait_ex(int epfd, struct epoll_event_ex *events,
                         int maxevents, int timeout_ms,
                         const struct timespec *timeout,
                         int use_timespec,
                         const sigset_t *sigmask)
{
    if (maxevents <= 0 || maxevents > WEPOLL_EPOLL_EX_MAX_EVENTS) {
        errno = EINVAL;
        return -1;
    }
    if (!events) {
        errno = EFAULT;
        return -1;
    }

    /* Hold a metadata-port reference across the native wait.  The wait uses
     * the stable duplicate, not the caller's integer descriptor, so
     * wepoll_close() can close the public fd and wake the waiter without
     * freeing its bookkeeping prematurely. */
    posix_port_t *p = port_acquire_or_create(epfd);
    if (!p) return -1;

    pthread_mutex_lock(&p->lock);
    uint64_t metadata_generation = p->metadata_generation;
    int wait_epfd = p->control_epfd;
    pthread_mutex_unlock(&p->lock);

    unsigned char *native_events = (unsigned char *)(void *)events;
    posix_wait_cleanup_t cleanup = { .port = p };
    int n;
    pthread_cleanup_push(posix_wait_cancel_cleanup, &cleanup);
    n = posix_native_wait(wait_epfd, native_events, maxevents, timeout_ms,
                          timeout, use_timespec, sigmask);
    pthread_cleanup_pop(0);
    if (n < 0) {
        int saved_errno = errno;
        if (port_is_closing(p)) saved_errno = EBADF;
        port_release(p);
        errno = saved_errno;
        return -1;
    }

    /* A close wake is an internal event and must never be exposed as a user
     * readiness event.  Return EBADF consistently with the Windows backend. */
    if (port_is_closing(p)) {
        port_release(p);
        errno = EBADF;
        return -1;
    }

    struct timespec ts = {0};
    uint64_t now_ns = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        now_ns = (uint64_t)ts.tv_sec * 1000000000ULL +
                 (uint64_t)ts.tv_nsec;
    }

    pthread_mutex_lock(&p->lock);
    int metadata_stable = p->metadata_generation == metadata_generation;
    for (int i = n; i-- > 0;) {
        struct epoll_event native_event;
        struct epoll_event_ex expanded_event;

        /* Copy the packed source before writing its wider destination.  In
         * descending order, destination i begins at i * sizeof(ex), which is
         * at or after the end of every lower-index source record still
         * needed.  Byte-wise copies avoid accessing the same storage through
         * incompatible struct lvalues. */
        memcpy(&native_event,
               native_events + (size_t)i * sizeof(native_event),
               sizeof(native_event));
        memset(&expanded_event, 0, sizeof(expanded_event));
        expanded_event.events = native_event.events;
        expanded_event.data = native_event.data;
        expanded_event.timestamp = now_ns;

        if (metadata_stable) {
            posix_sock_snapshot_t snapshot = {0};
            if (node_snapshot_by_data_locked(p, native_event.data,
                                             &snapshot)) {
                expanded_event.user_ctx = snapshot.user_ctx;
                if (snapshot.event.events & EPOLLET) {
                    expanded_event.flags |= WEPOLL_FLAG_ET_DELIVERED |
                                            WEPOLL_FLAG_EDGE_ARMED;
                }
                if (snapshot.event.events & EPOLLONESHOT) {
                    expanded_event.flags |= WEPOLL_FLAG_ONESHOT_FIRED;
                }
            }
        }
        memcpy(&events[i], &expanded_event, sizeof(expanded_event));
    }
    pthread_mutex_unlock(&p->lock);

    /* If close raced with metadata decoration, suppress the batch rather than
     * returning an event after the close linearization point. */
    if (port_is_closing(p)) {
        port_release(p);
        errno = EBADF;
        return -1;
    }
    port_release(p);
    return n;
}

/* --------------------------------------------------------------------- */
/* Public extension API — POSIX implementation.                      */
/* --------------------------------------------------------------------- */

WEPOLL_EX_API int epoll_create_ex(int size, int flags)
{
    if (size < 0 ||
        (flags & ~(EPOLL_CLOEXEC |
                   WEPOLL_EX_CREATE_EXPLICIT_REARM)) != 0) {
        errno = EINVAL;
        return -1;
    }
    if ((flags & WEPOLL_EX_CREATE_EXPLICIT_REARM) != 0) {
        errno = EOPNOTSUPP;
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
    return posix_wait_ex(epfd, events, maxevents, timeout, NULL, 0, NULL);
}

WEPOLL_EX_API int epoll_pwait2_ex(int epfd, struct epoll_event_ex *events,
                                  int maxevents,
                                  const struct timespec *timeout,
                                  const sigset_t *sigmask)
{
    if (validate_timespec_timeout(timeout) != 0) return -1;
    return posix_wait_ex(epfd, events, maxevents, 0, timeout, 1, sigmask);
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

WEPOLL_EX_API int epoll_rearm_classes(int epfd, int fd, uint32_t classes)
{
    (void)epfd;
    (void)fd;
    (void)classes;
    errno = EOPNOTSUPP;
    return -1;
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

WEPOLL_EX_API uint32_t wepoll_ex_version(void)
{
    return WEPOLL_EX_VERSION_NUMBER;
}
WEPOLL_EX_API const char *wepoll_ex_version_string(void)
{
    return "wepoll-ex " WEPOLL_EX_VERSION_STRING " (POSIX wrapper)";
}

WEPOLL_EX_API int wepoll_ex_get_socket_lifetime_policy(void)
{
    return WEPOLL_EX_SOCKET_LIFETIME_NOT_APPLICABLE;
}

static int copy_versioned_snapshot(void *destination, size_t destination_size,
                                   const void *snapshot, size_t snapshot_size)
{
    size_t prefix_size = sizeof(uint32_t) * 2;

    if (destination == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (destination_size < prefix_size) {
        errno = EINVAL;
        return -1;
    }

    memset(destination, 0, destination_size);
    memcpy(destination, snapshot,
           destination_size < snapshot_size ? destination_size : snapshot_size);
    return 0;
}

WEPOLL_EX_API int wepoll_ex_get_capabilities(
    wepoll_ex_capabilities *capabilities, size_t capabilities_size)
{
    wepoll_ex_capabilities snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = WEPOLL_EX_CAPABILITIES_VERSION;
    snapshot.struct_size = (uint32_t)sizeof(snapshot);
    snapshot.flags = WEPOLL_EX_CAP_NATIVE_EDGE_QUEUE |
                     WEPOLL_EX_CAP_ATOMIC_SIGNAL_MASK_WAIT |
                     WEPOLL_EX_CAP_NATIVE_EPOLL_DESCRIPTOR;
    return copy_versioned_snapshot(capabilities, capabilities_size,
                                   &snapshot, sizeof(snapshot));
}

WEPOLL_EX_API int wepoll_ex_get_stats(int epfd, wepoll_ex_stats *stats,
                                      size_t stats_size)
{
    posix_port_t *p;
    wepoll_ex_stats snapshot;

    if (stats == NULL || stats_size < sizeof(uint32_t) * 2) {
        errno = stats == NULL ? EFAULT : EINVAL;
        return -1;
    }
    p = port_acquire_or_create(epfd);
    if (p == NULL) return -1;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = WEPOLL_EX_STATS_VERSION;
    snapshot.struct_size = (uint32_t)sizeof(snapshot);
    snapshot.socket_lifetime_policy =
        WEPOLL_EX_SOCKET_LIFETIME_NOT_APPLICABLE;

    pthread_mutex_lock(&p->lock);
    snapshot.active_registrations = p->count;
    pthread_mutex_unlock(&p->lock);
    port_release(p);
    return copy_versioned_snapshot(stats, stats_size,
                                   &snapshot, sizeof(snapshot));
}

WEPOLL_EX_API int wepoll_ex_get_global_stats(
    wepoll_ex_global_stats *stats, size_t stats_size)
{
    wepoll_ex_global_stats snapshot;

    if (stats == NULL || stats_size < sizeof(uint32_t) * 2) {
        errno = stats == NULL ? EFAULT : EINVAL;
        return -1;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = WEPOLL_EX_STATS_VERSION;
    snapshot.struct_size = (uint32_t)sizeof(snapshot);
    return copy_versioned_snapshot(stats, stats_size,
                                   &snapshot, sizeof(snapshot));
}

WEPOLL_EX_API int wepoll_ex_wake(int epfd)
{
    (void)epfd;
    errno = EOPNOTSUPP;
    return -1;
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
        port_wake_waiters_locked(p);
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
int  ep_port_destroy(ep_port_t *p)                { (void)p; return 0; }
int  ep_port_register(ep_port_t *p, SOCKET f, uint32_t e, uint32_t fl,
                      epoll_data_t d, void *c)     { (void)p;(void)f;(void)e;(void)fl;(void)d;(void)c; errno=ENOSYS; return -1; }
int  ep_port_modify(ep_port_t *p, SOCKET f, uint32_t e, uint32_t fl,
                    epoll_data_t d, void *c)       { (void)p;(void)f;(void)e;(void)fl;(void)d;(void)c; errno=ENOSYS; return -1; }
int  ep_port_unregister(ep_port_t *p, SOCKET f)    { (void)p;(void)f; errno=ENOSYS; return -1; }
int  ep_port_rearm(ep_port_t *p, SOCKET f)         { (void)p;(void)f; errno=ENOSYS; return -1; }
int  ep_port_rearm_classes(ep_port_t *p, SOCKET f, uint32_t c) { (void)p;(void)f;(void)c; errno=ENOSYS; return -1; }
int  ep_port_wake(ep_port_t *p)                    { (void)p; errno=ENOSYS; return -1; }
int  ep_port_wait(ep_port_t *p, epoll_event_ex *o, int m, int t, const sigset_t *s)
                                                    { (void)p;(void)o;(void)m;(void)t;(void)s; errno=ENOSYS; return -1; }
void ep_sock_handle_completion(ep_sock_t *s, DWORD b, NTSTATUS st) { (void)s;(void)b;(void)st; }
DWORD RtlNtStatusToDosError(NTSTATUS s)             { (void)s; return 0; }
