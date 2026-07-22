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

typedef struct epfd_entry {
    int fd;
    ep_port_t *port;
    unsigned int refs;
    int closing;
    pthread_cond_t refs_drained;
    struct epfd_entry *next;
} epfd_entry_t;

static epfd_entry_t *g_epfd_buckets[EPFD_BUCKET_COUNT];
static pthread_mutex_t g_epfd_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_next_fd = 1;

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
    int cond_rc = pthread_cond_init(&entry->refs_drained, NULL);
    if (cond_rc != 0) {
        free(entry);
        ep_set_errno(cond_rc);
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
        pthread_cond_destroy(&entry->refs_drained);
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

static void epfd_put(epfd_entry_t *entry)
{
    pthread_mutex_lock(&g_epfd_lock);
    if (entry->refs > 0) {
        entry->refs--;
    }
    if (entry->closing && entry->refs == 0) {
        pthread_cond_signal(&entry->refs_drained);
    }
    pthread_mutex_unlock(&g_epfd_lock);
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

static int timeout_from_timespec(const struct timespec *timeout,
                                 int *timeout_ms)
{
    uint64_t seconds;
    uint64_t millis;

    if (timeout == NULL) {
        *timeout_ms = -1;
        return 0;
    }
    if (timeout->tv_sec < 0 ||
        timeout->tv_nsec < 0 ||
        timeout->tv_nsec >= 1000000000L) {
        ep_set_errno(EINVAL);
        return -1;
    }

    seconds = (uint64_t)timeout->tv_sec;
    if (seconds > (uint64_t)INT_MAX / UINT64_C(1000)) {
        ep_set_errno(EOVERFLOW);
        return -1;
    }

    millis = seconds * UINT64_C(1000);
    millis += ((uint64_t)timeout->tv_nsec + UINT64_C(999999)) /
              UINT64_C(1000000);
    if (millis > (uint64_t)INT_MAX) {
        ep_set_errno(EOVERFLOW);
        return -1;
    }
    *timeout_ms = (int)millis;
    return 0;
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
        if ((event->events & EPOLLET) != 0) {
            /* AFD_POLL is one-shot/level-triggered; re-arming it while a
             * socket remains ready would duplicate unread events.  Fail
             * explicitly until a real edge-triggered state machine exists. */
            ep_set_errno(EOPNOTSUPP);
            return -1;
        }
        if ((event->events & EPOLLEXCLUSIVE) != 0) {
            ep_set_errno(EOPNOTSUPP);
            return -1;
        }
        flags = event->events & (EPOLLONESHOT | EPOLLEXCLUSIVE);
        return ep_port_register(port, (SOCKET)fd,
                                event->events, flags,
                                event->data, user_ctx);

    case EPOLL_CTL_MOD:
        if (event == NULL) {
            ep_set_errno(EFAULT);
            return -1;
        }
        if ((event->events & EPOLLET) != 0) {
            ep_set_errno(EOPNOTSUPP);
            return -1;
        }
        if ((event->events & EPOLLEXCLUSIVE) != 0) {
            ep_set_errno(EOPNOTSUPP);
            return -1;
        }
        flags = event->events & EPOLLONESHOT;
        return ep_port_modify(port, (SOCKET)fd,
                              event->events, flags,
                              event->data, user_ctx);

    case EPOLL_CTL_DEL:
        return ep_port_unregister(port, (SOCKET)fd);

    default:
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
    epfd_entry_t *entry = epfd_require(epfd);
    int result;

    if (entry == NULL) {
        return -1;
    }
    result = epoll_ctl_port(entry->port, op, fd, event, user_ctx);
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

WEPOLL_EX_API int epoll_pwait(int epfd,
                              struct epoll_event *events,
                              int maxevents,
                              int timeout,
                              const wepoll_sigset_t *sigmask)
{
    epfd_entry_t *entry;
    epoll_event_ex *extended;
    int result;

    if (events == NULL) {
        ep_set_errno(EFAULT);
        return -1;
    }
    if (maxevents <= 0) {
        ep_set_errno(EINVAL);
        return -1;
    }
    if ((size_t)maxevents > SIZE_MAX / sizeof(*extended)) {
        ep_set_errno(EINVAL);
        return -1;
    }

    entry = epfd_require(epfd);
    if (entry == NULL) {
        return -1;
    }

    extended = (epoll_event_ex *)calloc((size_t)maxevents,
                                        sizeof(*extended));
    if (extended == NULL) {
        epfd_put(entry);
        ep_set_errno(ENOMEM);
        return -1;
    }

    result = epoll_wait_port(entry, extended, maxevents, timeout, sigmask);
    if (result >= 0) {
        for (int i = 0; i < result; i++) {
            events[i].events = extended[i].events;
            events[i].data = extended[i].data;
        }
    }

    free(extended);
    epfd_put(entry);
    return result;
}

WEPOLL_EX_API int epoll_pwait2(int epfd,
                               struct epoll_event *events,
                               int maxevents,
                               const struct timespec *timeout,
                               const wepoll_sigset_t *sigmask)
{
    int timeout_ms;

    if (timeout_from_timespec(timeout, &timeout_ms) != 0) {
        return -1;
    }
    return epoll_pwait(epfd, events, maxevents, timeout_ms, sigmask);
}

WEPOLL_EX_API int epoll_wait_ex(int epfd,
                                struct epoll_event_ex *events,
                                int maxevents,
                                int timeout)
{
    epfd_entry_t *entry;
    int result;

    if (events == NULL) {
        ep_set_errno(EFAULT);
        return -1;
    }
    if (maxevents <= 0) {
        ep_set_errno(EINVAL);
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
    int timeout_ms;
    int result;

    if (events == NULL) {
        ep_set_errno(EFAULT);
        return -1;
    }
    if (maxevents <= 0) {
        ep_set_errno(EINVAL);
        return -1;
    }
    if (timeout_from_timespec(timeout, &timeout_ms) != 0) {
        return -1;
    }

    entry = epfd_require(epfd);
    if (entry == NULL) {
        return -1;
    }
    result = epoll_wait_port(entry, events, maxevents,
                             timeout_ms, sigmask);
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
    return UINT32_C(0x00010000);
}

WEPOLL_EX_API const char *wepoll_ex_version_string(void)
{
    return "wepoll-ex 1.0.0 (experimental IOCP+AFD)";
}

WEPOLL_EX_API int wepoll_close(int epfd)
{
    epfd_entry_t *entry;
    ep_port_t *port;

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

    pthread_mutex_lock(&g_epfd_lock);
    while (entry->refs != 0) {
        pthread_cond_wait(&entry->refs_drained, &g_epfd_lock);
    }
    epfd_unlink_locked(entry);
    pthread_mutex_unlock(&g_epfd_lock);

    int result = ep_port_destroy(port);
    int saved_errno = ep_last_err();
    pthread_cond_destroy(&entry->refs_drained);
    free(entry);
    if (result != 0) {
        ep_set_errno(saved_errno);
    }
    return result;
}
