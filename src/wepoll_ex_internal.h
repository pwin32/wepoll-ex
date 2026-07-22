/*
 * wepoll_ex_internal.h — internal declarations shared across wepoll-ex
 * translation units.  Not part of the public API.
 */
#ifndef WEPOLL_EX_INTERNAL_H_
#define WEPOLL_EX_INTERNAL_H_

#include "wepoll_ex.h"

#include <stdatomic.h>
#include <pthread.h>
#include <limits.h>
#include <errno.h>

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

struct ep_sock {
    /* Pool linkage (all live sockets on this port). */
    struct ep_sock *next;
    struct ep_sock *prev;

    /* The descriptor supplied by the caller and the unwrapped provider
     * socket used in AFD_POLL. */
    SOCKET fd;
    SOCKET base_socket;

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

    /* Poll lifecycle.  The IO_STATUS_BLOCK is also used as the ApcContext
     * returned in the IOCP packet, so it must remain embedded until the
     * cancellation completion has been observed. */
    _Atomic uint32_t poll_status;
    _Atomic uint32_t delete_pending;
    _Atomic uint32_t ready_queued;
    uint8_t oneshot_fired;
    uint8_t needs_rearm;
    IO_STATUS_BLOCK io_status_block;
    uint64_t generation;

    /* The AFD poll buffer for this socket.  Allocated separately so the
     * buffer can be reused across re-arming without copying. */
    AFD_POLL_INFO *afd_info;

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

    /* Configuration snapshot. */
    int close_on_exec;

    /* Close/wait coordination.  A closing port cancels all outstanding AFD
     * polls and waits for their IOCP completions before storage is freed. */
    pthread_mutex_t wait_lock;
    _Atomic int closing;
    size_t pending_poll_count;
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
    int                     initialized;
    int                     wsa_initialized;
} ep_ntdll_t;

extern ep_ntdll_t g_ntdll;

/* ----------------------------------------------------------------------- */
/* Internal API.                                                           */
/* ----------------------------------------------------------------------- */

int  ep_global_init(void);
void ep_global_fini(void);

int  ep_port_create(int size_hint, int flags, ep_port_t **out);
void ep_port_destroy(ep_port_t *port);
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

void ep_sock_handle_completion(ep_sock_t *sock, DWORD bytes,
                               NTSTATUS status);

/* errno shim. */
void ep_set_errno(int e);
int  ep_last_err(void);

/* Map NTSTATUS -> errno. */
int  ep_status_to_errno(NTSTATUS s);
int  ep_winerr_to_errno(DWORD wsaerr);
DWORD ep_ntstatus_to_winerr(NTSTATUS status);

/* AFD/NT helpers implemented in wepoll_ex_afd.c. */
int      ep_afd_open(HANDLE iocp, HANDLE *out);
int      ep_afd_poll_submit(ep_sock_t *sock, uint32_t afd_events);
int      ep_afd_cancel(ep_sock_t *sock);
uint32_t ep_afd_to_epoll_events(ULONG afd_events);
uint32_t ep_epoll_to_afd_events(uint32_t epoll_events);
SOCKET   ep_socket_get_base(SOCKET socket);

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
