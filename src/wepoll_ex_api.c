/*
 * wepoll_ex_api.c -- public Windows API surface.
 *
 * The user-visible epoll descriptor is a small positive integer.  A
 * process-global table maps it to an ep_port_t and holds a reference for
 * every public operation.  Closing first removes the descriptor from public
 * lookup, wakes blocked waiters, and waits for those references to drain
 * before destroying the port.
 */
#include "wepoll_ex_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Separate chaining keeps descriptor entries at stable addresses while
 * operations hold references and avoids open-addressing deletion holes. */
#define EPFD_BUCKET_COUNT 64U
/* A stuck public operation must not make close hang forever.  This matches
 * the port's bounded AFD-completion drain while keeping the two waits
 * independent: this deadline covers only public API references. */
#define EP_API_CLOSE_TIMEOUT_MS 5000U

typedef struct epfd_entry {
    int fd;
    ep_port_t *port;
    unsigned int refs;
    int closing;
    int detached;
    HANDLE refs_drained_event;
    struct epfd_entry *next;
} epfd_entry_t;

static epfd_entry_t *g_epfd_buckets[EPFD_BUCKET_COUNT];
static pthread_mutex_t g_epfd_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_next_fd = 1;
static _Atomic unsigned int g_api_close_timeout_ms =
    EP_API_CLOSE_TIMEOUT_MS;
static _Atomic uint64_t g_api_close_timeout_count;
static _Atomic uint64_t g_api_deferred_destroy_count;

/* Internal diagnostics and deterministic-test hooks.  These symbols are not
 * part of the installed public API and therefore deliberately omit the
 * WEPOLL_EX_API export annotation. */
uint64_t ep_api_close_timeout_count(void);
void ep_test_set_api_close_timeout_ms(unsigned int timeout_ms);
void *ep_test_api_ref_hold(int epfd);
void ep_test_api_ref_release(void *reference);
uint64_t ep_test_api_deferred_destroy_count(void);

static unsigned int epfd_bucket(int fd)
{
    return (unsigned int)fd % EPFD_BUCKET_COUNT;
}

static epfd_entry_t *epfd_find_locked(int fd)
{
    epfd_entry_t *entry;

    if (fd <= 0) {
        return NULL;
    }

    for (entry = g_epfd_buckets[epfd_bucket(fd)];
         entry != NULL;
         entry = entry->next) {
        if (entry->fd == fd) {
            return entry;
        }
    }
    return NULL;
}

static int epfd_alloc(ep_port_t *port)
{
    epfd_entry_t *entry;
    int fd = -1;

    entry = (epfd_entry_t *)calloc(1, sizeof(*entry));
    if (entry == NULL) {
        ep_set_errno(ENOMEM);
        return -1;
    }
    entry->refs_drained_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (entry->refs_drained_event == NULL) {
        free(entry);
        ep_set_errno(ep_winerr_to_errno(GetLastError()));
        return -1;
    }

    pthread_mutex_lock(&g_epfd_lock);
    for (int attempts = 0; attempts < INT_MAX; attempts++) {
        int candidate = g_next_fd;

        g_next_fd = g_next_fd == INT_MAX ? 1 : g_next_fd + 1;
        if (candidate > 0 && epfd_find_locked(candidate) == NULL) {
            fd = candidate;
            break;
        }
    }

    if (fd > 0) {
        unsigned int bucket = epfd_bucket(fd);

        entry->fd = fd;
        entry->port = port;
        entry->next = g_epfd_buckets[bucket];
        g_epfd_buckets[bucket] = entry;
    }
    pthread_mutex_unlock(&g_epfd_lock);

    if (fd < 0) {
        (void)CloseHandle(entry->refs_drained_event);
        free(entry);
        ep_set_errno(EMFILE);
    }
    return fd;
}

static epfd_entry_t *epfd_acquire(int fd)
{
    epfd_entry_t *entry;

    pthread_mutex_lock(&g_epfd_lock);
    entry = epfd_find_locked(fd);
    if (entry != NULL && !entry->closing) {
        entry->refs++;
    } else {
        entry = NULL;
    }
    pthread_mutex_unlock(&g_epfd_lock);
    return entry;
}

static int epfd_destroy(epfd_entry_t *entry)
{
    int result = ep_port_destroy(entry->port);
    int saved_errno = ep_last_err();

    (void)CloseHandle(entry->refs_drained_event);
    free(entry);
    if (result != 0) {
        ep_set_errno(saved_errno);
    }
    return result;
}

static void epfd_put(epfd_entry_t *entry)
{
    int destroy = 0;

    pthread_mutex_lock(&g_epfd_lock);
    if (entry->refs > 0) {
        entry->refs--;
    }
    if (entry->closing && entry->refs == 0) {
        if (entry->detached) {
            destroy = 1;
        } else {
            (void)SetEvent(entry->refs_drained_event);
        }
    }
    pthread_mutex_unlock(&g_epfd_lock);

    if (destroy) {
        /* The public operation has already selected its result and errno.
         * Deferred teardown can report its own error only diagnostically, so
         * do not let it overwrite the completing operation's errno. */
        int operation_errno = ep_last_err();
        int destroy_result = epfd_destroy(entry);

        if (destroy_result == 0) {
            atomic_fetch_add_explicit(&g_api_deferred_destroy_count, 1,
                                      memory_order_relaxed);
        }
        ep_set_errno(operation_errno);
    }
}

static void epfd_unlink_locked(epfd_entry_t *entry)
{
    epfd_entry_t **link = &g_epfd_buckets[epfd_bucket(entry->fd)];

    while (*link != NULL && *link != entry) {
        link = &(*link)->next;
    }
    if (*link == entry) {
        *link = entry->next;
    }
}

static epfd_entry_t *epfd_require(int fd)
{
    epfd_entry_t *entry = epfd_acquire(fd);

    if (entry == NULL) {
        ep_set_errno(EBADF);
    }
    return entry;
}

uint64_t ep_api_close_timeout_count(void)
{
    return atomic_load_explicit(&g_api_close_timeout_count,
                                memory_order_relaxed);
}

void ep_test_set_api_close_timeout_ms(unsigned int timeout_ms)
{
    atomic_store_explicit(&g_api_close_timeout_ms, timeout_ms,
                          memory_order_relaxed);
}

void *ep_test_api_ref_hold(int epfd)
{
    return epfd_require(epfd);
}

void ep_test_api_ref_release(void *reference)
{
    if (reference != NULL) {
        epfd_put((epfd_entry_t *)reference);
    }
}

uint64_t ep_test_api_deferred_destroy_count(void)
{
    return atomic_load_explicit(&g_api_deferred_destroy_count,
                                memory_order_relaxed);
}

static int epoll_ctl_port(ep_port_t *port,
                          int op,
                          epoll_fd_t fd,
                          struct epoll_event *event,
                          void *user_ctx)
{
    uint32_t flags;

    if (fd == EPOLL_FD_INVALID) {
        ep_set_errno(EBADF);
        return -1;
    }

    switch (op) {
    case EPOLL_CTL_ADD:
        if (event == NULL) {
            ep_set_errno(EFAULT);
            return -1;
        }
        /* Linux permits only IN/OUT/WAKEUP/ET (plus ERR/HUP, which are always
         * reported) alongside EPOLLEXCLUSIVE.  In particular, ET is valid but
         * ONESHOT and RDHUP are not. */
        if ((event->events & EPOLLEXCLUSIVE) != 0) {
            uint32_t exclusive_allowed =
                EPOLLIN | EPOLLOUT | EPOLLWAKEUP | EPOLLET |
                EPOLLERR | EPOLLHUP | EPOLLEXCLUSIVE;

            if (ep_port_validate_target(port, (SOCKET)fd,
                                        EP_TARGET_VALIDATE_ADD) != 0) {
                return -1;
            }
            if ((event->events & ~exclusive_allowed) != 0) {
                ep_set_errno(EINVAL);
                return -1;
            }
        }
        flags = event->events & (EPOLLONESHOT | EPOLLEXCLUSIVE | EPOLLET);
        return ep_port_register(port, (SOCKET)fd,
                                event->events, flags,
                                event->data, user_ctx);

    case EPOLL_CTL_MOD:
        if (event == NULL) {
            ep_set_errno(EFAULT);
            return -1;
        }
        if ((event->events & EPOLLEXCLUSIVE) != 0) {
            /* Linux rejects EPOLLEXCLUSIVE on MOD. */
            if (ep_port_validate_target(port, (SOCKET)fd,
                                        EP_TARGET_VALIDATE_CONTROL) != 0) {
                return -1;
            }
            ep_set_errno(EINVAL);
            return -1;
        }
        flags = event->events & (EPOLLONESHOT | EPOLLET);
        return ep_port_modify(port, (SOCKET)fd,
                              event->events, flags,
                              event->data, user_ctx);

    case EPOLL_CTL_DEL:
        return ep_port_unregister(port, (SOCKET)fd);

    default:
        if (ep_port_validate_target(port, (SOCKET)fd,
                                    EP_TARGET_VALIDATE_CONTROL) != 0) {
            return -1;
        }
        ep_set_errno(EINVAL);
        return -1;
    }
}

static int epoll_wait_port(epfd_entry_t *entry,
                           epoll_event_ex *events,
                           int maxevents,
                           int timeout_ms,
                           const wepoll_sigset_t *sigmask)
{
    return ep_port_wait(entry->port, events, maxevents,
                        timeout_ms, sigmask);
}

static int epoll_wait_port_timeout(epfd_entry_t *entry,
                                   epoll_event_ex *events,
                                   int maxevents,
                                   const ep_wait_timeout_t *timeout,
                                   const wepoll_sigset_t *sigmask)
{
    return ep_port_wait_timeout(entry->port, events, maxevents,
                                timeout, sigmask);
}

WEPOLL_EX_API int epoll_create(int size)
{
    if (size <= 0) {
        ep_set_errno(EINVAL);
        return -1;
    }
    /* Linux has ignored this legacy sizing argument since 2.6.8. Keep the
     * standard entry point behavior-only compatible; callers that explicitly
     * want Windows preallocation use epoll_create_ex(). */
    return epoll_create_ex(0, 0);
}

WEPOLL_EX_API int epoll_create1(int flags)
{
    return epoll_create_ex(0, flags);
}

WEPOLL_EX_API int epoll_create_ex(int size, int flags)
{
    ep_port_t *port = NULL;
    int fd;

    if (size < 0 || (flags & ~EPOLL_CLOEXEC) != 0) {
        ep_set_errno(EINVAL);
        return -1;
    }
    if (ep_global_init() != 0) {
        return -1;
    }
    if (ep_port_create(size, flags, &port) != 0) {
        return -1;
    }

    fd = epfd_alloc(port);
    if (fd < 0) {
        int saved_errno = ep_last_err();
        ep_port_begin_close(port);
        (void)ep_port_destroy(port);
        ep_set_errno(saved_errno);
    }
    return fd;
}

WEPOLL_EX_API int epoll_ctl(int epfd,
                            int op,
                            epoll_fd_t fd,
                            struct epoll_event *event)
{
    return epoll_ctl_ctx(epfd, op, fd, event, NULL);
}

WEPOLL_EX_API int epoll_ctl_ctx(int epfd,
                                int op,
                                epoll_fd_t fd,
                                struct epoll_event *event,
                                void *user_ctx)
{
    struct epoll_event event_copy;
    struct epoll_event *effective_event = NULL;
    epfd_entry_t *entry;
    int result;

    /* Mainline Linux copies every non-DEL event before acquiring either
     * descriptor or validating the operation. DEL intentionally ignores its
     * event argument. */
    if (op != EPOLL_CTL_DEL) {
        if (event == NULL) {
            ep_set_errno(EFAULT);
            return -1;
        }
        event_copy = *event;
        effective_event = &event_copy;
    }
    if (fd == EPOLL_FD_INVALID) {
        ep_set_errno(EBADF);
        return -1;
    }

    entry = epfd_require(epfd);
    if (entry == NULL) {
        return -1;
    }
    result = epoll_ctl_port(entry->port, op, fd, effective_event, user_ctx);
    epfd_put(entry);
    return result;
}

WEPOLL_EX_API int epoll_wait(int epfd,
                             struct epoll_event *events,
                             int maxevents,
                             int timeout)
{
    return epoll_pwait(epfd, events, maxevents, timeout, NULL);
}

static int epoll_wait_basic_timeout(int epfd,
                                    struct epoll_event *events,
                                    int maxevents,
                                    const ep_wait_timeout_t *timeout,
                                    const wepoll_sigset_t *sigmask)
{
    epfd_entry_t *entry;
    int result;

    if (maxevents <= 0 || maxevents > WEPOLL_EPOLL_MAX_EVENTS) {
        ep_set_errno(EINVAL);
        return -1;
    }
    if (events == NULL) {
        ep_set_errno(EFAULT);
        return -1;
    }
    entry = epfd_require(epfd);
    if (entry == NULL) {
        return -1;
    }

    result = ep_port_wait_basic_timeout(entry->port, events, maxevents,
                                        timeout, sigmask);
    epfd_put(entry);
    return result;
}

WEPOLL_EX_API int epoll_pwait(int epfd,
                              struct epoll_event *events,
                              int maxevents,
                              int timeout,
                              const wepoll_sigset_t *sigmask)
{
    ep_wait_timeout_t wait_timeout;

    ep_wait_timeout_from_milliseconds(timeout, &wait_timeout);
    return epoll_wait_basic_timeout(epfd, events, maxevents,
                                    &wait_timeout, sigmask);
}

WEPOLL_EX_API int epoll_pwait2(int epfd,
                               struct epoll_event *events,
                               int maxevents,
                               const struct timespec *timeout,
                               const wepoll_sigset_t *sigmask)
{
    ep_wait_timeout_t wait_timeout;

    if (ep_wait_timeout_from_timespec(timeout, &wait_timeout) != 0) {
        return -1;
    }
    return epoll_wait_basic_timeout(epfd, events, maxevents,
                                    &wait_timeout, sigmask);
}

WEPOLL_EX_API int epoll_wait_ex(int epfd,
                                struct epoll_event_ex *events,
                                int maxevents,
                                int timeout)
{
    epfd_entry_t *entry;
    int result;

    if (maxevents <= 0 || maxevents > WEPOLL_EPOLL_EX_MAX_EVENTS) {
        ep_set_errno(EINVAL);
        return -1;
    }
    if (events == NULL) {
        ep_set_errno(EFAULT);
        return -1;
    }
    entry = epfd_require(epfd);
    if (entry == NULL) {
        return -1;
    }
    result = epoll_wait_port(entry, events, maxevents, timeout, NULL);
    epfd_put(entry);
    return result;
}

WEPOLL_EX_API int epoll_pwait2_ex(int epfd,
                                  struct epoll_event_ex *events,
                                  int maxevents,
                                  const struct timespec *timeout,
                                  const wepoll_sigset_t *sigmask)
{
    epfd_entry_t *entry;
    ep_wait_timeout_t wait_timeout;
    int result;

    if (ep_wait_timeout_from_timespec(timeout, &wait_timeout) != 0) {
        return -1;
    }
    if (maxevents <= 0 || maxevents > WEPOLL_EPOLL_EX_MAX_EVENTS) {
        ep_set_errno(EINVAL);
        return -1;
    }
    if (events == NULL) {
        ep_set_errno(EFAULT);
        return -1;
    }
    entry = epfd_require(epfd);
    if (entry == NULL) {
        return -1;
    }
    result = epoll_wait_port_timeout(entry, events, maxevents,
                                     &wait_timeout, sigmask);
    epfd_put(entry);
    return result;
}

WEPOLL_EX_API int epoll_ctl_batch(int epfd,
                                  const int *ops,
                                  const epoll_fd_t *fds,
                                  const struct epoll_event *events,
                                  int count)
{
    epfd_entry_t *entry;
    int result = 0;

    if (count <= 0) {
        ep_set_errno(EINVAL);
        return -1;
    }
    if (ops == NULL || fds == NULL) {
        ep_set_errno(EFAULT);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        if (ops[i] != EPOLL_CTL_ADD &&
            ops[i] != EPOLL_CTL_MOD &&
            ops[i] != EPOLL_CTL_DEL) {
            ep_set_errno(EINVAL);
            return -1;
        }
        if (ops[i] != EPOLL_CTL_DEL && events == NULL) {
            ep_set_errno(EFAULT);
            return -1;
        }
        if (fds[i] == EPOLL_FD_INVALID) {
            ep_set_errno(EBADF);
            return -1;
        }
    }

    entry = epfd_require(epfd);
    if (entry == NULL) {
        return -1;
    }

    for (int i = 0; i < count; i++) {
        struct epoll_event *event =
            ops[i] == EPOLL_CTL_DEL ? NULL :
            (struct epoll_event *)&events[i];

        if (epoll_ctl_port(entry->port, ops[i], fds[i],
                           event, NULL) != 0) {
            int saved_errno = ep_last_err();

            /* ADD is the only operation that can be safely undone without
             * retaining a full copy of prior registration state.  MOD and
             * DEL remain applied, so the batch API is deliberately
             * best-effort rather than transactional. */
            for (int j = i - 1; j >= 0; j--) {
                if (ops[j] == EPOLL_CTL_ADD) {
                    (void)epoll_ctl_port(entry->port, EPOLL_CTL_DEL,
                                         fds[j], NULL, NULL);
                }
            }
            ep_set_errno(saved_errno);
            result = -1;
            break;
        }
    }

    epfd_put(entry);
    return result;
}

WEPOLL_EX_API int epoll_drain(int epfd,
                              struct epoll_event *events,
                              int maxevents)
{
    return epoll_wait(epfd, events, maxevents, 0);
}

WEPOLL_EX_API int epoll_rearm(int epfd, epoll_fd_t fd)
{
    epfd_entry_t *entry = epfd_require(epfd);
    int result;

    if (entry == NULL) {
        return -1;
    }
    if (fd == EPOLL_FD_INVALID) {
        epfd_put(entry);
        ep_set_errno(EBADF);
        return -1;
    }
    result = ep_port_rearm(entry->port, (SOCKET)fd);
    epfd_put(entry);
    return result;
}

WEPOLL_EX_API int epoll_fd_count(int epfd)
{
    epfd_entry_t *entry = epfd_require(epfd);
    size_t count;

    if (entry == NULL) {
        return -1;
    }
    pthread_mutex_lock(&entry->port->fd_table_lock);
    count = entry->port->fd_table_count;
    pthread_mutex_unlock(&entry->port->fd_table_lock);
    epfd_put(entry);

    if (count > (size_t)INT_MAX) {
        ep_set_errno(EOVERFLOW);
        return -1;
    }
    return (int)count;
}

WEPOLL_EX_API uint32_t wepoll_ex_version(void)
{
    return WEPOLL_EX_VERSION_NUMBER;
}

WEPOLL_EX_API const char *wepoll_ex_version_string(void)
{
    return "wepoll-ex " WEPOLL_EX_VERSION_STRING
           " (experimental IOCP+AFD)";
}

WEPOLL_EX_API int wepoll_ex_get_socket_lifetime_policy(void)
{
#if defined(WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME)
    return WEPOLL_EX_SOCKET_LIFETIME_SYNCHRONIZED;
#elif defined(WEPOLL_EX_STRICT_SOCKET_IDENTITY)
    return WEPOLL_EX_SOCKET_LIFETIME_STRICT;
#else
    return WEPOLL_EX_SOCKET_LIFETIME_BEST_EFFORT;
#endif
}

static int copy_stats_snapshot(void *destination, size_t destination_size,
                               const void *snapshot, size_t snapshot_size)
{
    size_t prefix_size = sizeof(uint32_t) * 2;

    if (destination == NULL) {
        ep_set_errno(EFAULT);
        return -1;
    }
    if (destination_size < prefix_size) {
        ep_set_errno(EINVAL);
        return -1;
    }
    memset(destination, 0, destination_size);
    memcpy(destination, snapshot,
           destination_size < snapshot_size ? destination_size : snapshot_size);
    return 0;
}

WEPOLL_EX_API int wepoll_ex_get_stats(int epfd, wepoll_ex_stats *stats,
                                      size_t stats_size)
{
    epfd_entry_t *entry;
    wepoll_ex_stats snapshot;
    int result;

    if (stats == NULL || stats_size < sizeof(uint32_t) * 2) {
        ep_set_errno(stats == NULL ? EFAULT : EINVAL);
        return -1;
    }
    entry = epfd_require(epfd);
    if (entry == NULL) return -1;
    result = ep_port_get_stats(entry->port, &snapshot);
    epfd_put(entry);
    if (result != 0) return -1;
    return copy_stats_snapshot(stats, stats_size, &snapshot, sizeof(snapshot));
}

WEPOLL_EX_API int wepoll_ex_get_global_stats(
    wepoll_ex_global_stats *stats, size_t stats_size)
{
    wepoll_ex_global_stats snapshot;

    if (stats == NULL || stats_size < sizeof(uint32_t) * 2) {
        ep_set_errno(stats == NULL ? EFAULT : EINVAL);
        return -1;
    }
    ep_get_global_stats(&snapshot);
    return copy_stats_snapshot(stats, stats_size, &snapshot, sizeof(snapshot));
}

WEPOLL_EX_API int wepoll_close(int epfd)
{
    epfd_entry_t *entry;
    ep_port_t *port;
    unsigned int timeout_ms;
    int wait_error;

    pthread_mutex_lock(&g_epfd_lock);
    entry = epfd_find_locked(epfd);
    if (entry == NULL || entry->closing) {
        pthread_mutex_unlock(&g_epfd_lock);
        ep_set_errno(EBADF);
        return -1;
    }
    entry->closing = 1;
    port = entry->port;
    pthread_mutex_unlock(&g_epfd_lock);

    /* Wake any operation blocked in ep_port_wait before waiting for its API
     * reference to drain. */
    ep_port_begin_close(port);

    timeout_ms = atomic_load_explicit(&g_api_close_timeout_ms,
                                      memory_order_relaxed);
    pthread_mutex_lock(&g_epfd_lock);
    if (entry->refs != 0) {
        HANDLE refs_drained_event = entry->refs_drained_event;
        DWORD wait_result;

        pthread_mutex_unlock(&g_epfd_lock);
        wait_result = WaitForSingleObject(refs_drained_event, timeout_ms);
        if (wait_result == WAIT_OBJECT_0) {
            wait_error = 0;
        } else if (wait_result == WAIT_TIMEOUT) {
            wait_error = ETIMEDOUT;
        } else {
            wait_error = ep_winerr_to_errno(GetLastError());
            if (wait_error == 0) wait_error = EIO;
        }
        pthread_mutex_lock(&g_epfd_lock);
    } else {
        wait_error = 0;
    }
    if (entry->refs != 0) {
        /* Public lookup is severed before ownership is transferred.  The
         * final outstanding operation observes detached under the same lock
         * and becomes responsible for teardown in epfd_put(). */
        entry->detached = 1;
        epfd_unlink_locked(entry);
        if (wait_error == 0) wait_error = ETIMEDOUT;
        if (wait_error == ETIMEDOUT) {
            atomic_fetch_add_explicit(&g_api_close_timeout_count, 1,
                                      memory_order_relaxed);
        }
        pthread_mutex_unlock(&g_epfd_lock);
        ep_set_errno(wait_error);
        return -1;
    }
    epfd_unlink_locked(entry);
    pthread_mutex_unlock(&g_epfd_lock);

    return epfd_destroy(entry);
}
