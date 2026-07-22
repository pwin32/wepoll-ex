/*
 * wepoll-ex — Experimental epoll-shaped event layer for Windows
 *
 * The Windows implementation uses IOCP + AFD and exposes a deliberately
 * small, epoll-shaped API.  It supports sockets and a documented subset of
 * Linux semantics; it is not a drop-in implementation of every guarantee.
 *
 * Public API contract:
 *   - Common Linux entry points are exposed on Windows, plus an `_ex`
 *     extension family for diagnostics and per-registration context.
 *   - All EPOLL* event flag bits defined by Linux uapi are present.
 *   - errno is set to the closest portable/Linux-style value on failure.
 *     See docs/DESIGN.md for the current platform limitations.
 *
 * Originally derived from wepoll by Bert Belder <bertbelder@gmail.com>.
 * Extended for server workloads by the wepoll-ex authors.
 */
#ifndef WEPOLL_EX_H_
#define WEPOLL_EX_H_

#include <stdint.h>
#include <stddef.h>
#include <signal.h>   /* sigset_t on POSIX */
#include <time.h>     /* struct timespec for epoll_pwait2 */

#include "wepoll_ex_export.h"

/* On POSIX we delegate the basic epoll_create / epoll_ctl / epoll_wait
 * family to the host libc (which already provides them via
 * <sys/epoll.h>).  On Windows we provide the full implementation. */
#ifdef _WIN32
#  define WEPOLL_EX_PROVIDES_BASIC_EPOLL 1
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
   typedef SOCKET epoll_fd_t;
#  define EPOLL_FD_INVALID  (epoll_fd_t)(-1)
#else
#  include <sys/epoll.h>
   typedef int epoll_fd_t;
#  define EPOLL_FD_INVALID  (-1)
#endif

/* Windows has no native sigset_t.  The wait-mask argument is intentionally
 * opaque there (Windows waits do not manipulate a POSIX signal mask), while
 * POSIX callers retain the ordinary sigset_t type. */
#ifdef _WIN32
typedef void wepoll_sigset_t;
#else
typedef sigset_t wepoll_sigset_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Linux-compatible event flag bits.  On POSIX these are already defined
 * by <sys/epoll.h>; we only re-define on Windows to keep the same
 * values across platforms.
 * --------------------------------------------------------------------- */
#ifdef _WIN32
#define EPOLLIN       (1U << 0)
#define EPOLLPRI      (1U << 1)
#define EPOLLOUT      (1U << 2)
#define EPOLLERR      (1U << 3)
#define EPOLLHUP      (1U << 4)
#define EPOLLRDNORM   (1U << 6)
#define EPOLLRDBAND   (1U << 7)
#define EPOLLWRNORM   (1U << 8)
#define EPOLLWRBAND   (1U << 9)
#define EPOLLMSG      (1U << 10)
#define EPOLLRDHUP    (1U << 13)
#define EPOLLEXCLUSIVE (1U << 28)
#define EPOLLWAKEUP   (1U << 29)
#define EPOLLONESHOT  (1U << 30)
#define EPOLLET       (1U << 31)

#define EPOLL_CLOEXEC 02000000  /* octal — matches Linux O_CLOEXEC */

enum EPOLL_CTL_OP {
    EPOLL_CTL_ADD = 1,
    EPOLL_CTL_DEL = 2,
    EPOLL_CTL_MOD = 3
};

typedef union epoll_data {
    void    *ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
    epoll_fd_t sock; /* Windows socket-sized descriptor (extension). */
} epoll_data_t;

typedef struct epoll_event {
    uint32_t      events;
    epoll_data_t  data;
} epoll_event;
#endif /* _WIN32 */

/* ---------------------------------------------------------------------------
 * Extended event — used by epoll_wait_ex() and friends.
 *
 * Carries the source epoll_event plus auxiliary information that high-
 * performance consumers want without a second syscall:
 *   - flags      : internal descriptor state (currently oneshot delivery).
 *   - timestamp  : monotonic nanosecond timestamp sampled near IOCP
 *                  delivery; its origin is intentionally unspecified.
 *   - user_ctx   : opaque pointer registered per-fd via epoll_ctl_ctx().
 *                  Consumers can associate a stable application object with
 *                  the registration without a separate metadata lookup.
 * ------------------------------------------------------------------------- */
#define WEPOLL_FLAG_ONESHOT_FIRED  (1U << 0)
#define WEPOLL_FLAG_ET_DELIVERED   (1U << 1) /* reserved; EPOLLET unsupported */
#define WEPOLL_FLAG_EDGE_ARMED     (1U << 2) /* reserved; EPOLLET unsupported */

typedef struct epoll_event_ex {
    uint32_t      events;
    epoll_data_t  data;
    uint32_t      flags;       /* WEPOLL_FLAG_* */
    uint64_t      timestamp;   /* monotonic ns, unspecified origin */
    void         *user_ctx;    /* per-fd opaque pointer */
} epoll_event_ex;

/* ---------------------------------------------------------------------------
 * Core Linux-compatible API.
 *
 * On Windows these are provided by wepoll-ex.  On POSIX they come from
 * the host libc via <sys/epoll.h>.
 *
 * Every function sets errno on failure and returns -1, matching Linux.
 * ------------------------------------------------------------------------- */
#ifdef _WIN32
WEPOLL_EX_API int  epoll_create(int size);
WEPOLL_EX_API int  epoll_create1(int flags);
WEPOLL_EX_API int  epoll_ctl(int epfd, int op, epoll_fd_t fd,
                             struct epoll_event *ev);
WEPOLL_EX_API int  epoll_wait(int epfd, struct epoll_event *events,
                              int maxevents, int timeout);
WEPOLL_EX_API int  epoll_pwait(int epfd, struct epoll_event *events,
                               int maxevents, int timeout,
                               const wepoll_sigset_t *sigmask);
WEPOLL_EX_API int  epoll_pwait2(int epfd, struct epoll_event *events,
                                int maxevents,
                                const struct timespec *timeout,
                                const wepoll_sigset_t *sigmask);
#endif

/* ---------------------------------------------------------------------------
 * Extension API.
 *
 * These functions expose capabilities that the Linux epoll API cannot
 * represent.  They are pure extensions — code that uses them ceases to
 * be Linux-portable and should be guarded with #ifdef WEPOLL_EX_H_.
 * ------------------------------------------------------------------------- */

/* Same as epoll_create1, but allows the caller to reserve a hint about
 * the maximum number of fds that will be registered, so the IOCP
 * completion queue and AFD poll pool can be pre-sized.  Passing 0
 * falls back to a sensible default (64k fds). */
WEPOLL_EX_API int  epoll_create_ex(int size, int flags);

/* epoll_ctl with an extra user_ctx pointer that will be surfaced in
 * every epoll_event_ex delivered for this fd.  Pass NULL to clear. */
WEPOLL_EX_API int  epoll_ctl_ctx(int epfd, int op, epoll_fd_t fd,
                                 struct epoll_event *ev, void *user_ctx);

/* epoll_wait that returns extended events. */
WEPOLL_EX_API int  epoll_wait_ex(int epfd, struct epoll_event_ex *events,
                                 int maxevents, int timeout);

/* epoll_pwait2 that returns extended events. */
WEPOLL_EX_API int  epoll_pwait2_ex(int epfd, struct epoll_event_ex *events,
                                   int maxevents,
                                   const struct timespec *timeout,
                                   const wepoll_sigset_t *sigmask);

/* Batched epoll_ctl: apply operations in order.  On failure, earlier
 * operations remain applied; ADD operations are best-effort rolled back.
 * The function is not transactional. */
WEPOLL_EX_API int  epoll_ctl_batch(int epfd,
                                   const int *ops,
                                   const epoll_fd_t *fds,
                                   const struct epoll_event *events,
                                   int count);

/* Drain the ready queue without blocking.  Returns 0 if no events are
 * ready, otherwise behaves like epoll_wait with timeout=0. */
WEPOLL_EX_API int  epoll_drain(int epfd, struct epoll_event *events,
                               int maxevents);

/* Re-arm a fd that fired in EPOLLONESHOT mode without a full
 * EPOLL_CTL_MOD round-trip.  Equivalent to:
 *     epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &saved_event);
 * but cheaper because it skips user-mode validation and re-uses the
 * cached registration. */
WEPOLL_EX_API int  epoll_rearm(int epfd, epoll_fd_t fd);

/* Return the number of fds currently registered on the epoll
 * instance.  Useful for diagnostics and nginx's status module. */
WEPOLL_EX_API int  epoll_fd_count(int epfd);

/* Return library version: 0x00MMmmpp (major, minor, patch). */
WEPOLL_EX_API uint32_t wepoll_ex_version(void);
WEPOLL_EX_API const char *wepoll_ex_version_string(void);

/* Close an epoll fd created by epoll_create / epoll_create1 /
 * epoll_create_ex.  On POSIX this normally behaves like close(); a detected
 * stale tracked generation returns -1/EBADF without closing the replacement.
 * Synchronize native close/reuse against this call.  On Windows the user must
 * call wepoll_close() because the integer epfd is virtual and not a real
 * HANDLE. */
WEPOLL_EX_API int wepoll_close(int epfd);

#ifdef __cplusplus
}
#endif

#endif /* WEPOLL_EX_H_ */
