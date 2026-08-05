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
 *   - All public EPOLL* event flag bits exposed by Linux <sys/epoll.h> are
 *     present.
 *   - Windows implements EPOLLET (observed-edge), an opt-in explicit edge
 *     rearm mode, and ADD-time EPOLLEXCLUSIVE; non-null wait signal masks are
 *     accepted and ignored.
 *   - Windows also accepts selected waitable HANDLEs and pipes in addition to
 *     sockets; waitable HANDLEs require SYNCHRONIZE access, while mutexes,
 *     disk files, and other unsupported types return EPERM.
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
#include "wepoll_ex_version.h"

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

/* Linux x86/x86-64 exposes a 12-byte UAPI record with data at offset four.
 * x86-64 packs it to one-byte alignment; x86 retains four-byte alignment.
 * Match those public layouts on Windows so arrays and FFI bindings use the
 * same representation. Other architectures retain native alignment. */
#  if defined(__x86_64__) || defined(__amd64__) || \
      defined(_M_X64) || defined(_M_AMD64)
#    pragma pack(push, 1)
#    define WEPOLL_EX_EPOLL_EVENT_PACK_POP 1
#  elif defined(__i386__) || defined(_M_IX86)
#    pragma pack(push, 4)
#    define WEPOLL_EX_EPOLL_EVENT_PACK_POP 1
#  endif
typedef struct epoll_event {
    uint32_t      events;
    epoll_data_t  data;
} epoll_event;
#  if defined(WEPOLL_EX_EPOLL_EVENT_PACK_POP)
#    pragma pack(pop)
#    undef WEPOLL_EX_EPOLL_EVENT_PACK_POP
#  endif
#endif /* _WIN32 */

/* ---------------------------------------------------------------------------
 * Extended event — used by epoll_wait_ex() and friends.
 *
 * Carries the source epoll_event plus auxiliary information that high-
 * performance consumers want without a second syscall:
 *   - flags      : delivery state.  Windows currently reports oneshot
 *                  delivery; POSIX may also report native edge delivery.
 *   - timestamp  : monotonic nanosecond timestamp sampled around delivery;
 *                  its origin is intentionally unspecified.
 *   - user_ctx   : opaque pointer registered per-fd via epoll_ctl_ctx().
 *                  Consumers can associate a stable application object with
 *                  the registration without a separate metadata lookup.
 * ------------------------------------------------------------------------- */
#define WEPOLL_FLAG_ONESHOT_FIRED  (1U << 0)
#define WEPOLL_FLAG_ET_DELIVERED   (1U << 1) /* native EPOLLET was delivered */
#define WEPOLL_FLAG_EDGE_ARMED     (1U << 2) /* native edge registration */
#define WEPOLL_FLAG_WAKE_EVENT     (1U << 3) /* synthetic tagged wake */

typedef struct epoll_event_ex {
    uint32_t      events;
    epoll_data_t  data;
    uint32_t      flags;       /* WEPOLL_FLAG_* */
    uint64_t      timestamp;   /* monotonic ns, unspecified origin */
    void         *user_ctx;    /* per-fd opaque pointer */
} epoll_event_ex;

/* Windows socket-lifetime validation policy selected when the library was
 * built.  BEST_EFFORT preserves compatibility with providers that cannot
 * expose a stable WFP endpoint identity.  STRICT rejects those providers.
 * SYNCHRONIZED relies on the embedding application to DEL every registration
 * before closesocket().  The POSIX development wrapper reports NOT_APPLICABLE. */
typedef enum wepoll_ex_socket_lifetime_policy {
    WEPOLL_EX_SOCKET_LIFETIME_BEST_EFFORT = 0,
    WEPOLL_EX_SOCKET_LIFETIME_STRICT = 1,
    WEPOLL_EX_SOCKET_LIFETIME_SYNCHRONIZED = 2,
    WEPOLL_EX_SOCKET_LIFETIME_NOT_APPLICABLE = 3
} wepoll_ex_socket_lifetime_policy;

/* Versioned operational snapshots.  Callers pass sizeof(their structure) to
 * the getter; future releases may append fields while preserving the prefix. */
#define WEPOLL_EX_STATS_VERSION 1U

typedef struct wepoll_ex_stats {
    uint32_t version;
    uint32_t struct_size;
    uint32_t socket_lifetime_policy;
    uint32_t reserved;
    uint64_t active_registrations;
    uint64_t pending_polls;
    uint64_t rearm_queue_depth;
    uint64_t oneshot_probe_queue_depth;
    uint64_t ready_queue_depth;
    uint64_t afd_pool_in_use;
    uint64_t afd_pool_peak;
    uint64_t ready_pool_in_use;
    uint64_t ready_pool_peak;
    uint64_t rearm_work_items;
    uint64_t stale_events_dropped;
    uint64_t identity_failures;
    uint64_t asynchronous_errors;
    uint64_t zero_timeout_budget_hits;
    uint64_t wake_requests;
    uint64_t wake_coalesced;
    uint64_t wake_returns;
    uint64_t tcp_current_level_probes;
    uint64_t tcp_current_level_fallbacks;
} wepoll_ex_stats;

typedef struct wepoll_ex_global_stats {
    uint32_t version;
    uint32_t struct_size;
    uint64_t quarantined_ports;
    uint64_t reaped_ports;
    uint64_t irrecoverable_ports;
    uint64_t api_close_timeouts;
    uint64_t active_quarantines;
} wepoll_ex_global_stats;

/* Versioned compile/runtime capability snapshot.  These flags describe the
 * selected backend's public semantics, not merely whether an EPOLL* constant
 * is accepted.  In particular, native and observed edge delivery are kept
 * distinct, and a process-local exclusive implementation is reported
 * explicitly instead of being presented as cross-process Linux behavior. */
#define WEPOLL_EX_CAPABILITIES_VERSION 1U

#define WEPOLL_EX_CAP_NATIVE_EDGE_QUEUE \
    (UINT64_C(1) << 0)
#define WEPOLL_EX_CAP_OBSERVED_EDGE_FILTER \
    (UINT64_C(1) << 1)
#define WEPOLL_EX_CAP_EXCLUSIVE_PROCESS_LOCAL \
    (UINT64_C(1) << 2)
#define WEPOLL_EX_CAP_WAKE_STANDARD_WAIT \
    (UINT64_C(1) << 3)
#define WEPOLL_EX_CAP_WAKE_EXTENDED_WAIT \
    (UINT64_C(1) << 4)
#define WEPOLL_EX_CAP_ATOMIC_SIGNAL_MASK_WAIT \
    (UINT64_C(1) << 5)
#define WEPOLL_EX_CAP_NATIVE_EPOLL_DESCRIPTOR \
    (UINT64_C(1) << 6)
#define WEPOLL_EX_CAP_VIRTUAL_EPOLL_DESCRIPTOR \
    (UINT64_C(1) << 7)
#define WEPOLL_EX_CAP_EXPLICIT_EDGE_REARM \
    (UINT64_C(1) << 8)
#define WEPOLL_EX_CAP_TAGGED_WAKE_EVENT \
    (UINT64_C(1) << 9)
#define WEPOLL_EX_CAP_CLOSE_SOCKET_HELPER \
    (UINT64_C(1) << 10)
#define WEPOLL_EX_CAP_EXPLICIT_REARM_ONESHOT \
    (UINT64_C(1) << 11)
#define WEPOLL_EX_CAP_VIRTUAL_EPOLL_DUP \
    (UINT64_C(1) << 12)
#define WEPOLL_EX_CAP_ERROR_INFO \
    (UINT64_C(1) << 13)

typedef struct wepoll_ex_capabilities {
    uint32_t version;
    uint32_t struct_size;
    uint64_t flags;       /* WEPOLL_EX_CAP_* */
    uint64_t reserved[2];
} wepoll_ex_capabilities;

/* Per-thread details for the most recent failed wepoll-ex call.  portable_error
 * is the errno value returned by the public API.  native_code is present only
 * when the implementation retained the exact Win32, Winsock, or NTSTATUS
 * source; winsock_error is a documented equivalent when one is meaningful.
 * The channel is defined only after a call returns -1 and is not changed by a
 * successful call to wepoll_ex_get_last_error_info(). */
#define WEPOLL_EX_ERROR_INFO_VERSION 1U

typedef enum wepoll_ex_native_error_domain {
    WEPOLL_EX_NATIVE_ERROR_NONE = 0,
    WEPOLL_EX_NATIVE_ERROR_WIN32 = 1,
    WEPOLL_EX_NATIVE_ERROR_WINSOCK = 2,
    WEPOLL_EX_NATIVE_ERROR_NTSTATUS = 3
} wepoll_ex_native_error_domain;

#define WEPOLL_EX_ERROR_NATIVE_EXACT       (1U << 0)
#define WEPOLL_EX_ERROR_WINSOCK_EQUIVALENT (1U << 1)

typedef struct wepoll_ex_error_info {
    uint32_t version;
    uint32_t struct_size;
    int32_t portable_error;
    uint32_t native_domain;
    uint32_t native_code;
    int32_t winsock_error;
    uint32_t flags;
    uint32_t reserved;
} wepoll_ex_error_info;

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
/* Positive finite timespec waits use an optional high-resolution Windows
 * timer with a millisecond IOCP safety fallback. Long valid durations are
 * accepted; timer resolution does not guarantee scheduler wake latency. */
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

/* epoll_create_ex flags.  EXPLICIT_REARM is a Windows-only socket EPOLLET
 * mode: delivery disarms the returned read/write/terminal readiness classes
 * until epoll_rearm_classes() acknowledges that the application drained the
 * corresponding operations.  A terminal delivery disarms every class while
 * the application decides whether to close or deliberately rearm.  With
 * EPOLLONESHOT, partial class acknowledgements keep the one-shot disabled and
 * the final acknowledgement starts the next generation.  EPOLLEXCLUSIVE is
 * still rejected.  POSIX reports EOPNOTSUPP for this flag, and the Linux-
 * compatible epoll_create1() entry point never accepts it. */
#define WEPOLL_EX_CREATE_EXPLICIT_REARM 0x01000000

/* Class mask accepted by epoll_rearm_classes().  READ includes ordinary,
 * priority, and graceful peer-shutdown readiness; WRITE includes ordinary
 * writable readiness; TERMINAL includes EPOLLERR/EPOLLHUP. */
#define WEPOLL_EX_REARM_READ      (1U << 0)
#define WEPOLL_EX_REARM_WRITE     (1U << 1)
#define WEPOLL_EX_REARM_TERMINAL  (1U << 2)
#define WEPOLL_EX_REARM_ALL       (WEPOLL_EX_REARM_READ | \
                                   WEPOLL_EX_REARM_WRITE | \
                                   WEPOLL_EX_REARM_TERMINAL)

/* Same as epoll_create1, with an optional Windows capacity hint and the
 * WEPOLL_EX_CREATE_* flags above. A positive size pre-grows the fd table and
 * raises the initial AFD/ready pool capacity; the hint is capped at 4096 and
 * does not limit later registrations or resize the fixed IOCP dequeue batch.
 * Passing 0 uses defaults. POSIX accepts but otherwise ignores the hint. The
 * standard epoll_create() always ignores its positive legacy size, matching
 * Linux. */
WEPOLL_EX_API int  epoll_create_ex(int size, int flags);

/* epoll_ctl with an extra user_ctx pointer that will be surfaced in
 * epoll_event_ex results for this fd.  Pass NULL to clear.  On POSIX, any
 * overlapping extension metadata change suppresses user_ctx for the whole
 * returned batch; a shared opaque data value also cannot be decorated.
 * MOD/DEL on reused fd numbers return EOPNOTSUPP when the current fstat
 * fingerprint cannot identify one metadata registration unambiguously. On
 * Windows, socket ADD submits its initial AFD poll before returning and reports
 * submission errors synchronously; waitable and pipe ADD remain lazy when no
 * waiter is active. */
WEPOLL_EX_API int  epoll_ctl_ctx(int epfd, int op, epoll_fd_t fd,
                                 struct epoll_event *ev, void *user_ctx);

/* epoll_wait that returns extended events.  POSIX waits use a stable
 * duplicate of the tracked epoll descriptor so wepoll_close() can wake a
 * blocked extended wait and make it fail with EBADF. Legal maxevents values
 * are bounded by both the Linux UAPI record count and the addressable size of
 * the larger epoll_event_ex array; they are not proportional internal
 * allocation requests. POSIX writes the native packed result into the caller
 * array and expands it backward in place. */
WEPOLL_EX_API int  epoll_wait_ex(int epfd, struct epoll_event_ex *events,
                                 int maxevents, int timeout);

/* epoll_pwait2 that returns extended events and uses the same maxevents bounds
 * as epoll_wait_ex. POSIX applies a non-NULL signal mask through native
 * epoll_pwait2, or for each long-duration fallback chunk through epoll_pwait
 * with an inter-chunk signal bridge. Windows accepts and ignores a non-null
 * mask pointer. Positive finite Windows waits use an optional high-resolution
 * timer with a millisecond safety fallback; timer resolution does not
 * guarantee wake latency. */
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

/* Perform a zero-timeout readiness probe.  Windows may spend a bounded period
 * draining internal IOCP packets before returning; POSIX delegates directly
 * to native epoll_wait(..., 0). */
WEPOLL_EX_API int  epoll_drain(int epfd, struct epoll_event *events,
                               int maxevents);

/* Re-arm a tracked fd after an EPOLLONESHOT delivery while preserving its
 * saved event, data, and context.  On an explicit-rearm Windows port this is
 * also shorthand for rearming every disarmed class of a socket EPOLLET
 * registration.  Other Windows calls are a no-op when ONESHOT has not fired;
 * POSIX issues a native MOD. */
WEPOLL_EX_API int  epoll_rearm(int epfd, epoll_fd_t fd);

/* Rearm selected readiness classes on a socket EPOLLET registration owned by
 * a WEPOLL_EX_CREATE_EXPLICIT_REARM port.  The call is idempotent after the
 * ready node has been returned by a wait.  It reports EBUSY while that node is
 * still queued, and EOPNOTSUPP for other ports or registration kinds.  A
 * successful MOD starts a fresh observation and clears every class disarm.
 * For EPOLLONESHOT, acknowledging only some delivered classes keeps the
 * registration disabled; clearing the final delivered class also rearms the
 * one-shot.  epoll_rearm() acknowledges every delivered class at once.
 * Explicitly rearmed embedders must DEL before closesocket(), because a fully
 * disarmed registration may intentionally have no native AFD poll in flight.
 * POSIX reports EOPNOTSUPP. */
WEPOLL_EX_API int  epoll_rearm_classes(int epfd, epoll_fd_t fd,
                                       uint32_t classes);

/* Return the number of registrations tracked by this extension.  Windows
 * counts registrations in the virtual epoll instance.  POSIX counts entries
 * created/adopted through epoll_ctl_ctx or epoll_ctl_batch.  Native ADD is
 * outside this view until an extension MOD adopts it, and later native
 * MOD/DEL operations do not update extension metadata.  On POSIX, use
 * extension DEL before native close when an immediately accurate count is
 * required. */
WEPOLL_EX_API int  epoll_fd_count(int epfd);

/* Return WEPOLL_EX_VERSION_NUMBER (0x00MMmmpp: major, minor, patch) or a
 * descriptive string beginning with "wepoll-ex " WEPOLL_EX_VERSION_STRING. */
WEPOLL_EX_API uint32_t wepoll_ex_version(void);
WEPOLL_EX_API const char *wepoll_ex_version_string(void);
WEPOLL_EX_API int wepoll_ex_get_socket_lifetime_policy(void);

/* Copy the versioned capability snapshot for the selected backend. */
WEPOLL_EX_API int wepoll_ex_get_capabilities(
    wepoll_ex_capabilities *capabilities, size_t capabilities_size);

/* Copy a versioned operational snapshot into caller-provided storage.  The
 * supplied size must contain at least the version and struct_size prefix. */
WEPOLL_EX_API int wepoll_ex_get_stats(int epfd,
                                      wepoll_ex_stats *stats,
                                      size_t stats_size);
WEPOLL_EX_API int wepoll_ex_get_global_stats(
    wepoll_ex_global_stats *stats, size_t stats_size);

/* Copy the calling thread's error details for the most recent failed
 * wepoll-ex operation.  On POSIX, portable_error reflects errno and the
 * native fields are empty. */
WEPOLL_EX_API int wepoll_ex_get_last_error_info(
    wepoll_ex_error_info *error_info, size_t error_info_size);

/* Request one coalesced early return from a wait on this epoll instance.
 * Windows basic and extended waits return zero after already-ready events and
 * pending errors have taken priority.  Multiple requests before observation
 * coalesce into one return.  The POSIX wrapper does not intercept the host's
 * basic epoll wait family and therefore returns EOPNOTSUPP; callers must check
 * WEPOLL_EX_CAP_WAKE_* before using this extension. */
WEPOLL_EX_API int wepoll_ex_wake(int epfd);

/* Request one coalesced synthetic event from a wait on this epoll instance.
 * Windows snapshots the supplied event and returns it through the basic or
 * extended wait family after already-ready registrations and pending errors.
 * A tagged request upgrades a pending untagged wepoll_ex_wake(); once a tagged
 * request is pending, later wake requests coalesce without replacing its
 * payload. event->events must be a nonzero combination of ordinary delivered
 * EPOLL* readiness bits, not registration/control flags. Extended waits add
 * WEPOLL_FLAG_WAKE_EVENT. POSIX reports EOPNOTSUPP. */
WEPOLL_EX_API int wepoll_ex_wake_event(
    int epfd, const struct epoll_event *event);

/* Create another virtual descriptor for the same Windows epoll instance.
 * Registrations, ready state, waits, and statistics are shared.  Closing a
 * non-final alias removes only that alias; the final wepoll_close() wakes
 * waiters and destroys the underlying port.  POSIX callers should use dup()
 * on the native epoll descriptor and receive EOPNOTSUPP here. */
WEPOLL_EX_API int wepoll_ex_dup(int epfd);

/* Remove a socket from one epoll instance and then close it in that order.
 * An absent registration (ENOENT) is accepted after target validation.  Other
 * DEL failures leave the socket open.  The helper covers only this epfd;
 * callers that registered the socket in other instances must remove those
 * registrations first. */
WEPOLL_EX_API int wepoll_ex_close_socket(int epfd, epoll_fd_t fd);

/* Close an epoll fd created by epoll_create / epoll_create1 /
 * epoll_create_ex.  On POSIX this normally behaves like close(); tracked
 * extended waits are woken and fail with EBADF.  A detected stale tracked
 * generation returns -1/EBADF without closing the replacement.  Plain native
 * close() cannot wake an extended wait or retire its metadata, so callers
 * using the extension should use wepoll_close() and synchronize native
 * close/reuse against it.  On Windows the integer epfd is virtual and this
 * function is required.  A Windows teardown failure can return -1 after the
 * virtual epfd has been removed; that descriptor must not be retried. */
WEPOLL_EX_API int wepoll_close(int epfd);

#ifdef __cplusplus
}
#endif

#endif /* WEPOLL_EX_H_ */
