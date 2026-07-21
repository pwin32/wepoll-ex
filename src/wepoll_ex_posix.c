/*
 * wepoll_ex_posix.c — POSIX extension API implementation.
 *
 * On POSIX the basic epoll_create / epoll_ctl / epoll_wait family is
 * provided by the host libc.  wepoll-ex adds the extension API
 * (epoll_wait_ex, epoll_ctl_ctx, epoll_ctl_batch, epoll_rearm,
 * epoll_fd_count, wepoll_close) on top.
 *
 * To support the per-fd user_ctx pointer we maintain a per-epfd hash
 * table keyed on the registered fd.  Lookups are O(1) amortised.
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "wepoll_ex.h"
#include "wepoll_ex_internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/epoll.h>
#include <time.h>
#include <pthread.h>

typedef struct posix_sock_node {
    int                       fd;
    void                     *user_ctx;
    uint32_t                  user_flags;
    struct posix_sock_node   *next;
} posix_sock_node_t;

typedef struct posix_port {
    int                  epfd;
    pthread_mutex_t      lock;
    posix_sock_node_t  **buckets;
    size_t               n_buckets;
    size_t               count;
} posix_port_t;

#define POSIX_PORT_MAP_SIZE  1024
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

static posix_port_t *port_get_or_create(int epfd)
{
    pthread_mutex_lock(&g_posix_lock);
    posix_port_t *p = port_lookup_locked(epfd);
    if (p) { pthread_mutex_unlock(&g_posix_lock); return p; }

    /* Find a free slot. */
    p = calloc(1, sizeof(*p));
    if (!p) { pthread_mutex_unlock(&g_posix_lock); return NULL; }
    p->epfd = epfd;
    pthread_mutex_init(&p->lock, NULL);
    p->n_buckets = 1024;
    p->buckets = calloc(p->n_buckets, sizeof(posix_sock_node_t *));
    if (!p->buckets) { free(p); pthread_mutex_unlock(&g_posix_lock); return NULL; }

    for (int i = 0; i < POSIX_PORT_MAP_SIZE; i++) {
        if (g_posix_map[i] == NULL) {
            g_posix_map[i] = p;
            pthread_mutex_unlock(&g_posix_lock);
            return p;
        }
    }
    /* No free slot — table full. */
    pthread_mutex_destroy(&p->lock);
    free(p->buckets);
    free(p);
    pthread_mutex_unlock(&g_posix_lock);
    return NULL;
}

static void port_destroy(int epfd)
{
    pthread_mutex_lock(&g_posix_lock);
    for (int i = 0; i < POSIX_PORT_MAP_SIZE; i++) {
        if (g_posix_map[i] && g_posix_map[i]->epfd == epfd) {
            posix_port_t *p = g_posix_map[i];
            g_posix_map[i] = NULL;
            pthread_mutex_unlock(&g_posix_lock);

            for (size_t b = 0; b < p->n_buckets; b++) {
                posix_sock_node_t *n = p->buckets[b];
                while (n) {
                    posix_sock_node_t *next = n->next;
                    free(n);
                    n = next;
                }
            }
            free(p->buckets);
            pthread_mutex_destroy(&p->lock);
            free(p);
            return;
        }
    }
    pthread_mutex_unlock(&g_posix_lock);
}

static posix_sock_node_t *node_find_or_add(posix_port_t *p, int fd)
{
    size_t slot = (size_t)fd % p->n_buckets;
    pthread_mutex_lock(&p->lock);
    posix_sock_node_t *n = p->buckets[slot];
    while (n) { if (n->fd == fd) break; n = n->next; }
    if (!n) {
        n = calloc(1, sizeof(*n));
        if (n) {
            n->fd = fd;
            n->next = p->buckets[slot];
            p->buckets[slot] = n;
            p->count++;
        }
    }
    pthread_mutex_unlock(&p->lock);
    return n;
}

static void node_remove(posix_port_t *p, int fd)
{
    size_t slot = (size_t)fd % p->n_buckets;
    pthread_mutex_lock(&p->lock);
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
    pthread_mutex_unlock(&p->lock);
}

static posix_sock_node_t *node_lookup(posix_port_t *p, int fd)
{
    size_t slot = (size_t)fd % p->n_buckets;
    pthread_mutex_lock(&p->lock);
    posix_sock_node_t *n = p->buckets[slot];
    while (n) { if (n->fd == fd) break; n = n->next; }
    pthread_mutex_unlock(&p->lock);
    return n;
}

/* --------------------------------------------------------------------- */
/* Public extension API — POSIX implementation.                      */
/* --------------------------------------------------------------------- */

WEPOLL_EX_API int epoll_create_ex(int size, int flags)
{
    (void)size;
    return epoll_create1(flags);
}

WEPOLL_EX_API int epoll_ctl_ctx(int epfd, int op, int fd,
                                struct epoll_event *ev, void *user_ctx)
{
    posix_port_t *p = port_get_or_create(epfd);
    if (!p) { errno = ENOMEM; return -1; }

    if (op == EPOLL_CTL_ADD) {
        if (!ev) { errno = EFAULT; return -1; }
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, ev) != 0) return -1;
        posix_sock_node_t *n = node_find_or_add(p, fd);
        if (n) {
            n->user_ctx   = user_ctx;
            n->user_flags = ev->events;
        }
        return 0;
    } else if (op == EPOLL_CTL_MOD) {
        if (!ev) { errno = EFAULT; return -1; }
        if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, ev) != 0) return -1;
        posix_sock_node_t *n = node_find_or_add(p, fd);
        if (n) {
            if (user_ctx) n->user_ctx = user_ctx;
            n->user_flags = ev->events;
        }
        return 0;
    } else if (op == EPOLL_CTL_DEL) {
        int rc = epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        if (rc != 0) return -1;
        node_remove(p, fd);
        return 0;
    }
    errno = EINVAL;
    return -1;
}

WEPOLL_EX_API int epoll_wait_ex(int epfd, struct epoll_event_ex *events,
                                int maxevents, int timeout)
{
    if (!events || maxevents <= 0) { errno = EINVAL; return -1; }
    struct epoll_event *kevs = calloc(maxevents, sizeof(*kevs));
    if (!kevs) { errno = ENOMEM; return -1; }
    int n = epoll_wait(epfd, kevs, maxevents, timeout);
    if (n < 0) { free(kevs); return -1; }

    posix_port_t *p = port_get_or_create(epfd);
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    for (int i = 0; i < n; i++) {
        events[i].events    = kevs[i].events;
        events[i].data      = kevs[i].data;
        events[i].flags     = 0;
        events[i].timestamp = now_ns;
        events[i].user_ctx  = NULL;
        if (p) {
            posix_sock_node_t *node = node_lookup(p, kevs[i].data.fd);
            if (node) {
                events[i].user_ctx = node->user_ctx;
                if (node->user_flags & EPOLLET)      events[i].flags |= WEPOLL_FLAG_ET_DELIVERED | WEPOLL_FLAG_EDGE_ARMED;
                if (node->user_flags & EPOLLONESHOT) events[i].flags |= WEPOLL_FLAG_ONESHOT_FIRED;
            }
        }
    }
    free(kevs);
    return n;
}

WEPOLL_EX_API int epoll_pwait2_ex(int epfd, struct epoll_event_ex *events,
                                  int maxevents,
                                  const struct timespec *timeout,
                                  const sigset_t *sigmask)
{
    int ms;
    if (!timeout) ms = -1;
    else ms = (int)(timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000);
    (void)sigmask;
    return epoll_wait_ex(epfd, events, maxevents, ms);
}

WEPOLL_EX_API int epoll_ctl_batch(int epfd, const int *ops,
                                  const int *fds,
                                  const struct epoll_event *events,
                                  int count)
{
    int applied = 0;
    for (int i = 0; i < count; i++) {
        struct epoll_event ev = events ? events[i] : (struct epoll_event){0};
        if (epoll_ctl_ctx(epfd, ops[i], fds[i], &ev, NULL) == 0) {
            applied++;
        } else {
            /* Roll back. */
            for (int j = applied - 1; j >= 0; j--) {
                if (ops[j] == EPOLL_CTL_ADD) {
                    epoll_ctl_ctx(epfd, EPOLL_CTL_DEL, fds[j], NULL, NULL);
                }
            }
            return -1;
        }
    }
    return 0;
}

WEPOLL_EX_API int epoll_drain(int epfd, struct epoll_event *events,
                              int maxevents)
{
    return epoll_wait(epfd, events, maxevents, 0);
}

WEPOLL_EX_API int epoll_rearm(int epfd, int fd)
{
    /* Look up the cached event mask and re-MOD it.  This matches
     * wepoll-ex's Windows implementation, which re-arms the AFD poll
     * without going through full EPOLL_CTL_MOD validation. */
    posix_port_t *p = port_get_or_create(epfd);
    if (!p) { errno = ENOENT; return -1; }
    posix_sock_node_t *n = node_lookup(p, fd);
    if (!n) { errno = ENOENT; return -1; }
    struct epoll_event ev = { .events = n->user_flags, .data.fd = fd };
    return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

WEPOLL_EX_API int epoll_fd_count(int epfd)
{
    posix_port_t *p = port_get_or_create(epfd);
    return p ? (int)p->count : 0;
}

WEPOLL_EX_API uint32_t wepoll_ex_version(void)        { return 0x01000000; }
WEPOLL_EX_API const char *wepoll_ex_version_string(void) {
    return "wepoll-ex 1.0.0 (POSIX wrapper)";
}

WEPOLL_EX_API int wepoll_close(int epfd)
{
    port_destroy(epfd);
    return close(epfd);
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
