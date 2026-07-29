/*
 * wepoll_ex_internal.h — internal declarations shared across wepoll-ex
 * translation units.  Not part of the public API.
 */
#ifndef WEPOLL_EX_INTERNAL_H_
#define WEPOLL_EX_INTERNAL_H_

#ifdef _WIN32
/* The AFD implementation uses the Windows 8-era contract.  Keep the
 * library translation units consistent even when an embedding build (such
 * as nginx) supplies an older SDK target before including this header. */
# ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0602
# elif _WIN32_WINNT < 0x0602
#  undef _WIN32_WINNT
#  define _WIN32_WINNT 0x0602
# endif
#endif

#include "wepoll_ex.h"

#include <stdatomic.h>
#include <pthread.h>
#include <limits.h>
#include <errno.h>

#if defined(WEPOLL_EX_STRICT_SOCKET_IDENTITY) && \
    defined(WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME)
#  error "strict and synchronized socket lifetime modes are mutually exclusive"
#endif

#ifdef _WIN32
#  include <winsock2.h>
#  include <mswsock.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <winternl.h>
#else
/* POSIX shims — let the shared internal header compile on Linux
 * so the AFD buffer pool + ready queue (which are platform-
 * independent) can be used in both builds.  The Windows-only
 * fields are present but never touched at runtime on POSIX. */
#  ifndef HANDLE
   typedef void *HANDLE;
#  endif
#  ifndef ULONG
   typedef unsigned long ULONG;
#  endif
#  ifndef DWORD
   typedef unsigned long DWORD;
#  endif
#  ifndef NTSTATUS
   typedef long NTSTATUS;
#  endif
#  ifndef SOCKET
   typedef int SOCKET;
#  endif
#  ifndef LARGE_INTEGER
   typedef struct { long long QuadPart; } LARGE_INTEGER;
#  endif
#  ifndef PHANDLE
   typedef HANDLE *PHANDLE;
#  endif
#  ifndef ACCESS_MASK
   typedef unsigned long ACCESS_MASK;
#  endif
#  ifndef POBJECT_ATTRIBUTES
   typedef void *POBJECT_ATTRIBUTES;
#  endif
#  ifndef PLARGE_INTEGER
   typedef LARGE_INTEGER *PLARGE_INTEGER;
#  endif
#  ifndef NTAPI
#    define NTAPI
#  endif
   typedef struct { void *Pointer; unsigned long Internal, InternalHigh; } OVERLAPPED;
   typedef struct { void *lpCompletionKey; OVERLAPPED *lpOverlapped; unsigned long Internal; DWORD dwNumberOfBytesTransferred; } OVERLAPPED_ENTRY;
   typedef void *PVOID;
   typedef struct _IO_STATUS_BLOCK {
       union { NTSTATUS Status; PVOID Pointer; } DUMMYUNIONNAME;
       uintptr_t Information;
   } IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;
   typedef void (*PIO_APC_ROUTINE)(PVOID, PIO_STATUS_BLOCK, ULONG);
#  ifndef STATUS_PENDING
#    define STATUS_PENDING ((NTSTATUS)0x00000103)
#  endif
#  ifndef STATUS_SUCCESS
#    define STATUS_SUCCESS ((NTSTATUS)0x00000000)
#  endif
#  ifndef STATUS_CANCELLED
#    define STATUS_CANCELLED ((NTSTATUS)0xC0000120)
#  endif
#  ifndef STATUS_NOT_FOUND
#    define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225)
#  endif
#endif

/* winternl.h does not expose every NTSTATUS constant in all MinGW SDK
 * revisions.  Keep the values local and portable across those headers. */
#ifndef STATUS_SUCCESS
#  define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_PENDING
#  define STATUS_PENDING ((NTSTATUS)0x00000103L)
#endif
#ifndef STATUS_CANCELLED
#  define STATUS_CANCELLED ((NTSTATUS)0xC0000120L)
#endif
#ifndef STATUS_NOT_FOUND
#  define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225L)
#endif

/* ----------------------------------------------------------------------- */
/* Compile-time configuration.                                             */
/* ----------------------------------------------------------------------- */

/* Initial size of the per-port fd table.  Grows on demand. */
#define WEPOLL_INITIAL_FDS       256

/* Number of AFD poll buffers to keep in the per-port pool. */
#define WEPOLL_AFD_POOL_SIZE     64

/* Maximum number of events returned by a single epoll_wait call. */
#define WEPOLL_MAX_EVENTS        4096

/* ----------------------------------------------------------------------- */
/* Forward declarations.                                                   */
/* ----------------------------------------------------------------------- */

typedef struct ep_port      ep_port_t;
typedef struct ep_sock      ep_sock_t;
typedef struct ep_poll_ctx  ep_poll_ctx_t;

/* ----------------------------------------------------------------------- */
/* AFD — Ancillary Function Driver.                                        */
/*                                                                         */
/* wepoll uses NtDeviceIoControlFile against the AFD device to register    */
/* kernel-side poll requests on sockets.  AFD is undocumented but stable   */
/* since Windows 8 and is the same mechanism WSAPoll uses internally.      */
/* ----------------------------------------------------------------------- */

#define AFD_DEVICE_NAME   L"\\Device\\Afd\\Wepoll"
#define AFD_MAX_ADDRESS_LENGTH  32

typedef struct _AFD_POLL_HANDLE_INFO {
    HANDLE Handle;
    ULONG Events;
    NTSTATUS Status;
} AFD_POLL_HANDLE_INFO, *PAFD_POLL_HANDLE_INFO;

typedef struct _AFD_POLL_INFO {
    LARGE_INTEGER Timeout;
    ULONG NumberOfHandles;
    ULONG Exclusive;
    AFD_POLL_HANDLE_INFO Handles[1];
} AFD_POLL_INFO, *PAFD_POLL_INFO;

#define AFD_POLL_RECEIVE           0x0001UL
#define AFD_POLL_RECEIVE_EXPEDITED 0x0002UL
#define AFD_POLL_SEND              0x0004UL
#define AFD_POLL_DISCONNECT        0x0008UL
#define AFD_POLL_ABORT             0x0010UL
#define AFD_POLL_LOCAL_CLOSE       0x0020UL
#define AFD_POLL_ACCEPT            0x0080UL
#define AFD_POLL_CONNECT_FAIL      0x0100UL
#define AFD_POLL_ALL_EVENTS        (AFD_POLL_RECEIVE | \
                                    AFD_POLL_RECEIVE_EXPEDITED | \
                                    AFD_POLL_SEND | AFD_POLL_DISCONNECT | \
                                    AFD_POLL_ABORT | AFD_POLL_LOCAL_CLOSE | \
                                    AFD_POLL_ACCEPT | AFD_POLL_CONNECT_FAIL)

/* IoControlCode for AFD_POLL.  Reverse-engineered; matches Win8+. */
#define IOCTL_AFD_POLL  0x00012024

/* Function pointer types for the NTDLL entry points we resolve at startup. */
typedef NTSTATUS (NTAPI *PNtDeviceIoControlFile)(
    HANDLE FileHandle,
    HANDLE Event,
    PIO_APC_ROUTINE ApcRoutine,
    PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    ULONG IoControlCode,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength);

typedef NTSTATUS (NTAPI *PNtCreateFile)(
    PHANDLE FileHandle,
    ACCESS_MASK DesiredAccess,
    POBJECT_ATTRIBUTES ObjectAttributes,
    PIO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize,
    ULONG FileAttributes,
    ULONG ShareAccess,
    ULONG CreateDisposition,
    ULONG CreateOptions,
    PVOID EaBuffer,
    ULONG EaLength);

typedef NTSTATUS (NTAPI *PNtCancelIoFileEx)(
    HANDLE FileHandle,
    PIO_STATUS_BLOCK IoRequestToCancel,
    PIO_STATUS_BLOCK IoStatusBlock);

typedef NTSTATUS (NTAPI *PNtQueryObjectFn)(
    HANDLE Handle,
    ULONG ObjectInformationClass,
    PVOID ObjectInformation,
    ULONG ObjectInformationLength,
    ULONG *ReturnLength);

typedef NTSTATUS (NTAPI *PNtQueryEventFn)(
    HANDLE EventHandle,
    ULONG EventInformationClass,
    PVOID EventInformation,
    ULONG EventInformationLength,
    ULONG *ReturnLength);

#ifdef _WIN32
/* Indirect the completion dequeue primitive so fault-injection tests can
 * model a timer-granularity WAIT_TIMEOUT without intercepting kernel32.  The
 * field is internal and initialized to the native API for every port. */
typedef BOOL (WINAPI *PGetQueuedCompletionStatusEx)(
    HANDLE CompletionPort,
    OVERLAPPED_ENTRY *CompletionPortEntries,
    ULONG Count,
    PULONG NumEntriesRemoved,
    DWORD Milliseconds,
    BOOL Alertable);
typedef BOOL (WINAPI *PPostQueuedCompletionStatusFn)(
    HANDLE CompletionPort,
    DWORD NumberOfBytesTransferred,
    ULONG_PTR CompletionKey,
    LPOVERLAPPED Overlapped);
typedef HANDLE (WINAPI *PCreateWaitableTimerExWFn)(
    LPSECURITY_ATTRIBUTES TimerAttributes,
    LPCWSTR TimerName,
    DWORD Flags,
    DWORD DesiredAccess);
typedef VOID (WINAPI *PQueryUnbiasedInterruptTimePreciseFn)(
    PULONGLONG UnbiasedTime);
#endif

/* Internal wait representation.  Millisecond waits retain the public
 * epoll_wait/epoll_pwait contract, while timespec waits additionally carry
 * an upward-rounded 100-nanosecond duration for the optional Windows
 * high-resolution timer path. */
typedef struct ep_wait_timeout {
    uint64_t milliseconds;
    uint64_t intervals_100ns;
    uint8_t infinite;
    uint8_t precise;
} ep_wait_timeout_t;

typedef enum ep_timeout_capability {
    EP_TIMEOUT_CAPABILITY_UNKNOWN = 0,
    EP_TIMEOUT_CAPABILITY_AVAILABLE = 1,
    EP_TIMEOUT_CAPABILITY_UNAVAILABLE = 2
} ep_timeout_capability_t;

/* ----------------------------------------------------------------------- */
/* ep_sock_t — per-fd state.                                               */
/*                                                                         */
/* One of these is allocated for every fd registered with epoll_ctl and    */
/* kept alive until either EPOLL_CTL_DEL or close().  The poll request     */
/* is asynchronously pended against AFD; when it completes, the IOCP       */
/* delivers the IO_STATUS_BLOCK address embedded in this struct.           */
/* ----------------------------------------------------------------------- */

typedef enum ep_sock_state {
    EP_SOCK_INVALID = 0,
    EP_SOCK_REGISTERED,    /* epoll_ctl ADD succeeded, no poll pending */
    EP_SOCK_POLLING,       /* AFD poll request pended */
    EP_SOCK_READY,         /* poll completed, awaiting epoll_wait delivery */
    EP_SOCK_DELETED        /* pending EPOLL_CTL_DEL, awaiting teardown */
} ep_sock_state_t;

typedef enum ep_poll_status {
    EP_POLL_IDLE = 0,
    EP_POLL_PENDING,
    EP_POLL_CANCELLED
} ep_poll_status_t;

#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
/* SIO_QUERY_WFP_ALE_ENDPOINT_HANDLE identifies the transport endpoint behind
 * a Winsock handle without retaining another reference to the socket.  TCP
 * clients legitimately receive a new endpoint token when connect is started,
 * so unconnected streams remain transitional until they are connected. */
typedef enum ep_socket_identity_state {
    EP_SOCKET_ID_UNAVAILABLE = 0,
    EP_SOCKET_ID_TRANSITIONAL,
    EP_SOCKET_ID_STABLE
} ep_socket_identity_state_t;
#endif

typedef enum ep_reg_kind {
    EP_REG_SOCKET = 0,
    EP_REG_WAITABLE = 1,
    EP_REG_PIPE = 2
} ep_reg_kind_t;

/* Only protocols whose Winsock metadata is an exact UDP/IP match receive
 * protocol-specific AFD event handling.  Every unsupported, malformed, or
 * unavailable protocol description remains conservative. */
typedef enum ep_socket_protocol {
    EP_SOCKET_PROTOCOL_UNKNOWN = 0,
    EP_SOCKET_PROTOCOL_UDP = 1
} ep_socket_protocol_t;

typedef enum ep_waitable_semantics {
    EP_WAITABLE_NONE = 0,
    EP_WAITABLE_PERSISTENT = 1,
    EP_WAITABLE_TERMINAL = 2,
    EP_WAITABLE_CONSUMPTIVE = 3,
    EP_WAITABLE_ET_UNSUPPORTED = 4
} ep_waitable_semantics_t;

typedef enum ep_pipe_access {
    EP_PIPE_ACCESS_NONE = 0,
    EP_PIPE_ACCESS_READ = 1,
    EP_PIPE_ACCESS_WRITE = 2
} ep_pipe_access_t;

struct ep_sock {
    /* Pool linkage (all live sockets on this port). */
    struct ep_sock *next;
    struct ep_sock *prev;

    /* The descriptor supplied by the caller and the unwrapped provider
     * socket used in AFD_POLL.  Waitable registrations store the same
     * HANDLE value in fd/base_socket and set kind=EP_REG_WAITABLE or
     * EP_REG_PIPE. */
    SOCKET fd;
    SOCKET base_socket;
    uint8_t kind; /* ep_reg_kind_t */
    uint8_t waitable_semantics; /* ep_waitable_semantics_t */
    uint8_t pipe_access; /* ep_pipe_access_t */
    uint8_t socket_protocol; /* ep_socket_protocol_t */
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    uint64_t endpoint_id;
    uint8_t endpoint_id_state;
#endif

    /* User-supplied event mask + data + per-fd context. */
    uint32_t      user_events;    /* EPOLLIN | EPOLLOUT | … */
    uint32_t      user_flags;     /* EPOLLET | EPOLLONESHOT | … */
    epoll_data_t  user_data;
    void         *user_ctx;

    /* State machine. */
    _Atomic uint32_t state;

    /* Pending AFD events (events the kernel reported but we have not yet
     * delivered to user via epoll_wait). */
    uint32_t pending_events;
    /* Exact AFD interest mask used by the in-flight/last submission.  MOD
     * uses it to avoid cancelling a request whose mask already covers the
     * new interests. */
    uint32_t submitted_afd_events;
    /* Edge-triggered latch: interest bits already reported to the user while
     * the corresponding level remains continuously true.  Cleared for bits
     * that drop out of the latest AFD level snapshot so a later re-assert
     * can form a new edge. */
    uint32_t observed_events;

    /* Poll lifecycle.  The IO_STATUS_BLOCK is also used as the ApcContext
     * returned in the IOCP packet, so it must remain embedded until the
     * cancellation completion has been observed. */
    _Atomic uint32_t poll_status;
    _Atomic uint32_t delete_pending;
    _Atomic uint32_t ready_queued;
    /* Non-socket callbacks post the embedded IO_STATUS_BLOCK to IOCP.  These
     * atomics keep the registration alive until the callback has returned and
     * ensure cancellation produces exactly one completion packet. */
    _Atomic uint32_t callback_active;
    _Atomic uint32_t completion_posted;
    uint8_t aux_consumed_pending;
    uint8_t oneshot_fired;
    uint8_t needs_rearm;
    /* Empty ET observations and losing exclusive claims defer re-submission.
     * The wait loop clears this latch on the next API wait or after a short
     * internal retry interval, avoiding a tight immediate-completion loop. */
    uint8_t et_holdoff;

    /* Intrusive membership in the process-wide EPOLLEXCLUSIVE claim index.
     * A registration owns at most one node whose bitset represents its read,
     * write, and terminal claims.  The global exclusive lock protects these
     * links and fields; embedding the node avoids a fixed-capacity or
     * allocation-failure path while readiness is being delivered. */
    struct ep_sock *exclusive_next;
    struct ep_sock *exclusive_prev;
    SOCKET exclusive_claim_base;
    uint8_t exclusive_claim_classes;

    /* Exactly-once membership in the per-port deferred-work lists.  These
     * links are protected by fd_table_lock and are independent of the live
     * socket list above: a socket can be removed from either work list in
     * O(1) during MOD, DEL, native-close retirement, or final reclamation. */
    struct ep_sock *rearm_next;
    struct ep_sock *rearm_prev;
    struct ep_sock *oneshot_next;
    struct ep_sock *oneshot_prev;
    IO_STATUS_BLOCK io_status_block;
    uint64_t generation;

    /* The AFD poll buffer for this socket.  Allocated separately so the
     * buffer can be reused across re-arming without copying.  NULL for
     * waitable-HANDLE registrations. */
    AFD_POLL_INFO *afd_info;

    /* RegisterWaitForSingleObject cookie for waitable HANDLE registrations,
     * or a timer-queue timer for pipe polling.  NULL when idle. */
    HANDLE wait_registration;

    /* Back-pointer to owning port (for IOCP callback lookup). */
    ep_port_t *port;
};

/* ----------------------------------------------------------------------- */
/* ep_port_t — one per epoll instance.                                     */
/* ----------------------------------------------------------------------- */

/* ----------------------------------------------------------------------- */
/* MPSC ready queue.                                                     */
/*                                                                       */
/* Producers exchange the head and publish through their predecessor's   */
/* next link.  The sole consumer advances the non-atomic tail.  Queue     */
/* destruction therefore requires all producers and the consumer to be   */
/* quiescent.                                                            */
/* ----------------------------------------------------------------------- */

typedef struct ep_ready_node {
    _Atomic(struct ep_ready_node *) next;
    epoll_data_t data;        /* immutable registration snapshot */
    void         *user_ctx;
    SOCKET        fd;
    uint64_t      sock_generation;
    uint32_t      events;
    uint32_t      flags;      /* WEPOLL_FLAG_* delivery flags */
    uint64_t      timestamp;
} ep_ready_node_t;

typedef struct ep_ready_queue {
    /* Producers exchange `head` and publish through the predecessor's
     * `next` field.  The single consumer owns `tail` and advances it
     * only after a producer has published a successor.  This is the
     * standard intrusive MPSC queue hand-off; a producer never writes
     * to a node after the consumer is allowed to reclaim it. */
    _Atomic(ep_ready_node_t *) head;
    ep_ready_node_t          *tail;

    /* Sentinel node used by the consumer when it reaches the last
     * published node.  It is never returned to the caller. */
    ep_ready_node_t *stub;

    /* Number of real enqueue operations not yet consumed.  A producer
     * increments before publishing its link, so this may temporarily
     * include an in-flight node.  It is exact after producers quiesce. */
    _Atomic(size_t) queued;
    int             initialized;
} ep_ready_queue_t;

/* AFD buffer pool — pre-allocated AFD_POLL_INFO buffers and ready
 * queue nodes, recycled LIFO.  Eliminates the malloc/calloc on the
 * EPOLL_CTL_ADD hot path and on every IOCP completion. */
typedef struct ep_afd_pool {
    /* The freelist is protected by `lock`.  A mutex is deliberately
     * used here: unlike the old lock-free stack, it has no ABA window
     * and keeps ownership/accounting updates in one critical section. */
    void            *stack;
    void            *all_entries;
    size_t           buf_size;
    size_t           capacity;       /* initial reservation */
    size_t           allocated;      /* total entries ever allocated */
    _Atomic(size_t)  in_use;
    _Atomic(size_t)  peak;
    pthread_mutex_t  lock;
    int              initialized;
} ep_afd_pool_t;

struct ep_port {
    /* The IOCP handle that drives this epoll instance. */
    HANDLE iocp;

    /* Short lease protecting auxiliary/control posts from IOCP revocation.
     * `iocp` remains stable until final teardown; posts use the alias while
     * holding this lock, and close paths revoke the alias before CloseHandle. */
    HANDLE iocp_post_handle;
    pthread_mutex_t iocp_post_lock;

    /* Handle to AFD, opened once per port. */
    HANDLE afd;

    /* Per-port fd table — indexed by SOCKET value masked by fd_mask.
     * Grows when more than 75 % full.  NULL entries are unused slots. */
    ep_sock_t      **fd_table;
    size_t           fd_table_size;
    size_t           fd_table_count;
    pthread_mutex_t  fd_table_lock;

    /* All live socks on this port, chained for cleanup. */
    ep_sock_t       *sock_list_head;

    /* Ready queue — MPSC lock-free.  Drained by epoll_wait. */
    ep_ready_queue_t ready_queue;

    /* AFD buffer pool — pre-allocated AFD_POLL_INFO buffers and
     * ready-queue nodes, recycled LIFO.  Eliminates malloc on the
     * EPOLL_CTL_ADD and IOCP completion hot paths. */
    ep_afd_pool_t    afd_info_pool;   /* AFD_POLL_INFO buffers */
    ep_afd_pool_t    ready_node_pool; /* ep_ready_node_t nodes   */

    /* Batched IOCP delivery.  GetQueuedCompletionStatusEx returns
     * up to `iocp_batch_size` completions in one call, amortising
     * the syscall cost across multiple events. */
    OVERLAPPED_ENTRY *iocp_entries;
    ULONG              iocp_batch_size;
#ifdef _WIN32
    PGetQueuedCompletionStatusEx get_queued_completion_status_ex;
    PPostQueuedCompletionStatusFn post_queued_completion_status;

    /* Positive finite epoll_pwait2 waits may use a high-resolution waitable
     * timer.  The timer and threadpool wait are initialized lazily so the
     * ordinary integer-millisecond API path remains unchanged.  Timeout
     * callbacks post the sentinel below to IOCP with the immutable arm
     * generation in the completion key; stale packets are harmless. */
    PCreateWaitableTimerExWFn create_waitable_timer_ex_w;
    PQueryUnbiasedInterruptTimePreciseFn
        query_unbiased_interrupt_time_precise;
    HANDLE precise_timeout_timer;
    PTP_WAIT precise_timeout_wait;
    OVERLAPPED precise_timeout_overlapped;
    ULONG_PTR precise_timeout_generation;
    _Atomic ULONG_PTR precise_timeout_active_generation;
    _Atomic int precise_timeout_armed;
    _Atomic uint64_t precise_timeout_post_failures;
    uint8_t precise_timeout_capability; /* ep_timeout_capability_t */
#endif

    /* Configuration snapshot. */
    int close_on_exec;

    /* Close/wait coordination.  A closing port cancels all outstanding AFD
     * polls and waits for their IOCP completions before storage is freed. */
    pthread_mutex_t wait_lock;
    _Atomic int waiter_active;
    _Atomic int closing;
    _Atomic int iocp_closed;
    _Atomic int iocp_post_error;
    _Atomic uint64_t iocp_post_failures;
    size_t pending_poll_count;

    /* Deferred AFD submissions and fired-oneshot identity probes.  Counts
     * are diagnostic invariants; arm_pending consumes the intrusive lists
     * directly and therefore does no work proportional to all registrations. */
    ep_sock_t *rearm_head;
    ep_sock_t *rearm_tail;
    ep_sock_t *oneshot_head;
    ep_sock_t *oneshot_tail;
    size_t needs_rearm_count;
    size_t oneshot_fired_count;
    uint64_t rearm_work_visits;
    uint64_t oneshot_probe_visits;
    uint64_t stale_events_dropped;
    uint64_t identity_failures;
    uint64_t asynchronous_errors;
    uint64_t zero_timeout_budget_hits;
    int async_error;
    uint64_t close_drain_timeout_ms;
    uint64_t quarantine_drain_timeout_ms;
    uint64_t next_sock_generation;

    /* Atomic generation counter bumped on every ADD/DEL/MOD.  Used by
     * epoll_wait to detect ABA races when the ready queue is drained. */
    _Atomic uint64_t generation;
};

/* ----------------------------------------------------------------------- */
/* Resolved NTDLL symbols (singleton).                                     */
/* ----------------------------------------------------------------------- */

typedef struct ep_ntdll {
    PNtDeviceIoControlFile  NtDeviceIoControlFile;
    PNtCreateFile           NtCreateFile;
    PNtCancelIoFileEx       NtCancelIoFileEx;
    PNtQueryObjectFn        NtQueryObject;
    PNtQueryEventFn         NtQueryEvent;
    int                     initialized;
    int                     wsa_initialized;
} ep_ntdll_t;

extern ep_ntdll_t g_ntdll;

/* ----------------------------------------------------------------------- */
/* Deterministic internal fault injection.                                 */
/*                                                                         */
/* A configured point fails exactly once, on its Nth hit.  Production      */
/* builds leave every point disabled; the initial mask check is then the   */
/* only work performed by ep_fault_hit().  These symbols are intentionally  */
/* absent from the public header and shared-library export surface.         */
/* ----------------------------------------------------------------------- */

typedef enum ep_fault_point {
    EP_FAULT_POOL_INIT_ALLOC = 0,
    EP_FAULT_POOL_GROW,
    EP_FAULT_AFD_OPEN,
    EP_FAULT_AFD_SUBMIT,
    EP_FAULT_AFD_CANCEL,
    EP_FAULT_ENDPOINT_IDENTITY,
    EP_FAULT_ENDPOINT_UNAVAILABLE,
    EP_FAULT_PROVIDER_BASE,
    EP_FAULT_READY_NODE_ALLOC,
    EP_FAULT_IOCP_CREATE,
    EP_FAULT_IOCP_POST,
    EP_FAULT_AUX_POST,
    EP_FAULT_IOCP_DEQUEUE,
    EP_FAULT_AUX_DISARM,
    EP_FAULT_TIMEOUT_INIT,
    EP_FAULT_TIMEOUT_ARM,
    EP_FAULT_TIMEOUT_POST,
    EP_FAULT_POINT_COUNT
} ep_fault_point_t;

#ifdef WEPOLL_EX_ENABLE_FAULT_INJECTION
int      ep_fault_configure(ep_fault_point_t point, uint64_t fail_at,
                            int error);
void     ep_fault_reset(void);
int      ep_fault_hit(ep_fault_point_t point);
uint64_t ep_fault_hits(ep_fault_point_t point);
#else
/* Keep production and embedded builds free of fault-injection branches and
 * link dependencies.  The dedicated fault-test library enables the real
 * implementation for deterministic regression coverage. */
#  define ep_fault_hit(point) ((void)(point), 0)
#endif

/* ----------------------------------------------------------------------- */
/* Internal API.                                                           */
/* ----------------------------------------------------------------------- */

int  ep_global_init(void);
void ep_global_fini(void);

int  ep_port_create(int size_hint, int flags, ep_port_t **out);
int  ep_port_destroy(ep_port_t *port);
void ep_port_begin_close(ep_port_t *port);

int  ep_port_register(ep_port_t *port, SOCKET fd,
                      uint32_t events, uint32_t flags,
                      epoll_data_t data, void *ctx);
int  ep_port_modify(ep_port_t *port, SOCKET fd,
                    uint32_t events, uint32_t flags,
                    epoll_data_t data, void *ctx);
int  ep_port_unregister(ep_port_t *port, SOCKET fd);
int  ep_port_rearm(ep_port_t *port, SOCKET fd);

int  ep_port_wait(ep_port_t *port, epoll_event_ex *out, int maxevents,
                  int timeout_ms, const wepoll_sigset_t *sigmask);
#ifdef _WIN32
void ep_wait_timeout_from_milliseconds(int timeout_ms,
                                       ep_wait_timeout_t *timeout);
int  ep_wait_timeout_from_timespec(const struct timespec *timespec,
                                   ep_wait_timeout_t *timeout);
int  ep_port_wait_timeout(ep_port_t *port, epoll_event_ex *out,
                          int maxevents, const ep_wait_timeout_t *timeout,
                          const wepoll_sigset_t *sigmask);
#endif

void ep_sock_handle_completion(ep_sock_t *sock, DWORD bytes,
                               NTSTATUS status);
int  ep_port_get_stats(ep_port_t *port, wepoll_ex_stats *stats);
void ep_get_global_stats(wepoll_ex_global_stats *stats);
uint64_t ep_api_close_timeout_count(void);

#ifdef _WIN32
/* Test/debug invariant checker.  The caller must hold fd_table_lock. */
int ep_port_worklists_valid_locked(const ep_port_t *port);
#endif

/* errno shim. */
void ep_set_errno(int e);
int  ep_last_err(void);

/* Map NTSTATUS -> errno. */
int  ep_status_to_errno(NTSTATUS s);
int  ep_winerr_to_errno(DWORD wsaerr);
DWORD ep_ntstatus_to_winerr(NTSTATUS status);

/* AFD/NT helpers implemented in wepoll_ex_afd.c. */
int      ep_afd_open(HANDLE iocp, HANDLE *out);
int      ep_afd_poll_submit(ep_sock_t *sock, uint32_t afd_events,
                            int *pending_out);
int      ep_afd_cancel(ep_sock_t *sock);
uint32_t ep_afd_to_epoll_events(ULONG afd_events, NTSTATUS afd_status,
                                uint8_t socket_protocol);
uint32_t ep_epoll_to_afd_events(uint32_t epoll_events);
#ifdef _WIN32
uint8_t  ep_socket_protocol_from_info(const WSAPROTOCOL_INFOW *protocol_info,
                                      int protocol_info_length);
/* Internal callback form used to test provider-chain resolution without
 * installing a process- or system-wide Winsock layered service provider. */
typedef SOCKET (*ep_socket_ioctl_fn)(SOCKET socket, DWORD ioctl,
                                     int *error_out, void *context);
SOCKET   ep_socket_get_base_with_ioctl(SOCKET socket,
                                       ep_socket_ioctl_fn ioctl_fn,
                                       void *context);
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
/* Endpoint tokens are always 64-bit, including on 32-bit Windows.  This
 * callback form keeps provider response handling deterministic in tests
 * without replacing WSAIoctl process-wide. */
typedef int (*ep_socket_endpoint_ioctl_fn)(
    SOCKET socket, DWORD ioctl, uint64_t *result_out, DWORD result_size,
    DWORD *bytes_out, int *error_out, void *context);
int      ep_socket_get_endpoint_id_with_ioctl(
    SOCKET socket, uint64_t *endpoint_id,
    ep_socket_endpoint_ioctl_fn ioctl_fn, void *context);
#endif
#endif
SOCKET   ep_socket_get_base(SOCKET socket);
uint8_t  ep_socket_get_protocol(SOCKET socket);
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
int      ep_socket_get_endpoint_id(SOCKET socket, uint64_t *endpoint_id);
#endif

/* ----------------------------------------------------------------------- */
/* AFD buffer pool — LIFO stack of pre-allocated buffers.                 */
/*                                                                       */
/* ep_afd_pool_init   : allocate `capacity` buffers of `buf_size` bytes. */
/* ep_afd_pool_destroy: free all returned buffers.  Callers must first   */
/*                      return every outstanding buffer and quiesce all */
/*                      threads that can access the pool.               */
/* ep_afd_pool_take   : pop a buffer, or malloc a fresh one if pool is   */
/*                      exhausted.  Fresh entries are accounted exactly */
/*                      like pre-allocated entries.                     */
/* ep_afd_pool_give   : return a buffer to the pool; duplicate returns   */
/*                      are ignored and do not underflow `in_use`.       */
/* ----------------------------------------------------------------------- */
int   ep_afd_pool_init(ep_afd_pool_t *p, size_t buf_size, size_t capacity);
void  ep_afd_pool_destroy(ep_afd_pool_t *p);
void *ep_afd_pool_take(ep_afd_pool_t *p);
void  ep_afd_pool_give(ep_afd_pool_t *p, void *buf);

/* ----------------------------------------------------------------------- */
/* MPSC lock-free ready queue.                                           */
/* ----------------------------------------------------------------------- */
void  ep_ready_init(ep_ready_queue_t *q);
/* The sole consumer must drain/reclaim every node, and all producers    */
/* must be quiescent, before destroy.  A non-empty queue is left intact  */
/* and reported through errno=EBUSY.                                    */
void  ep_ready_destroy(ep_ready_queue_t *q);
void  ep_ready_push(ep_ready_queue_t *q, ep_ready_node_t *node);
/* Drain up to maxevents nodes.  Returns a singly-linked chain that
 * the caller walks via the `next` field.  Nodes are NOT freed —
 * caller must return each node to the ready_node_pool. */
ep_ready_node_t *ep_ready_drain(ep_ready_queue_t *q, int maxevents);

/* Convenience wrappers around the ready_node_pool: alloc returns a
 * node from the pool (or freshly malloc'd), free returns one. */
ep_ready_node_t *ep_ready_node_alloc(ep_port_t *port);
void             ep_ready_node_free(ep_port_t *port, ep_ready_node_t *n);

#endif /* WEPOLL_EX_INTERNAL_H_ */
