/*
 * wepoll_ex_port.c -- Windows IOCP/AFD port and socket lifecycle.
 *
 * All mutable socket state is serialized by fd_table_lock.  AFD requests
 * keep ep_sock_t storage alive until their IOCP completion is observed;
 * deletion therefore removes a socket from public lookup immediately but
 * defers reclamation while a poll is pending.  Ready nodes contain immutable
 * registration snapshots plus a socket generation, never a reclaimable raw
 * socket pointer.
 */
#include "wepoll_ex_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define EP_CLOSE_DRAIN_TIMEOUT_MS UINT64_C(5000)
#define EP_QUARANTINE_DRAIN_TIMEOUT_MS UINT64_C(60000)
#define EP_CLOSE_DRAIN_SLICE_MS   100U
#define EP_ZERO_TIMEOUT_DRAIN_BUDGET_MS 10U
#define EP_ZERO_TIMEOUT_MIN_DEQUEUES     16U
#define EP_DEFERRED_REARM_RETRY_MS       1U
#define EP_MAX_ACTIVE_QUARANTINES        4U
#define EP_100NS_PER_MILLISECOND         UINT64_C(10000)
#define EP_MAX_FINITE_IOCP_WAIT_MS       (INFINITE - 1U)

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#  define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002UL
#endif

#define EP_FILE_PIPE_LOCAL_INFORMATION_CLASS 24U
#define EP_PIPE_STATE_DISCONNECTED 1U
#define EP_PIPE_STATE_LISTENING     2U
#define EP_PIPE_STATE_CONNECTED     3U
#define EP_PIPE_STATE_CLOSING       4U
#define EP_PIPE_SERVER_END          1U
#define EP_STATUS_INVALID_HANDLE    UINT32_C(0xC0000008)

static _Atomic uint64_t g_quarantined_ports;

#ifdef _WIN32
static void ep_port_store_proc(void *target, size_t target_size, FARPROC proc)
{
    size_t copy_size = target_size < sizeof(proc) ? target_size : sizeof(proc);

    memset(target, 0, target_size);
    memcpy(target, &proc, copy_size);
}
#endif

/* EPOLLEXCLUSIVE wake uniqueness among wepoll-ex instances.  AFD Exclusive
 * cancels peer polls when it can, but already-queued completions can still
 * race.  Claim each provider base/readiness class for one exclusive owner
 * while its reported AFD level remains active.  A later STATUS_PENDING
 * submission that covers a claimed class proves quiescence and releases it.
 *
 * Each live registration supplies its own intrusive claim node, so claim
 * capacity grows with registrations and delivery never needs to allocate.
 * Fixed hash buckets partition lookup work without bounding the number of
 * claims; one process-wide mutex serializes ownership across independent
 * ports. */
#define EP_EXCLUSIVE_CLAIM_BUCKETS 128U
#define EP_EXCLUSIVE_CLASS_READ      UINT8_C(0x01)
#define EP_EXCLUSIVE_CLASS_WRITE     UINT8_C(0x02)
#define EP_EXCLUSIVE_CLASS_TERMINAL  UINT8_C(0x04)
static ep_sock_t *g_exclusive_claim_buckets[EP_EXCLUSIVE_CLAIM_BUCKETS];
static pthread_mutex_t g_exclusive_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned ep_exclusive_bucket(SOCKET base)
{
    uintptr_t value = (uintptr_t)base;
    value ^= value >> 17;
    value *= UINT64_C(0x9e3779b97f4a7c15);
    return (unsigned)(value % EP_EXCLUSIVE_CLAIM_BUCKETS);
}

static uint8_t ep_exclusive_classes(uint32_t delivered)
{
    uint8_t classes = 0;

    if ((delivered & (EPOLLIN | EPOLLRDNORM | EPOLLRDHUP |
                      EPOLLPRI | EPOLLRDBAND)) != 0) {
        classes |= EP_EXCLUSIVE_CLASS_READ;
    }
    if ((delivered & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)) != 0) {
        classes |= EP_EXCLUSIVE_CLASS_WRITE;
    }
    if ((delivered & (EPOLLERR | EPOLLHUP)) != 0) {
        classes |= EP_EXCLUSIVE_CLASS_TERMINAL;
    }
    return classes;
}

static uint32_t ep_exclusive_class_afd_events(uint8_t readiness_class)
{
    switch (readiness_class) {
    case EP_EXCLUSIVE_CLASS_READ:
        return AFD_POLL_RECEIVE | AFD_POLL_ACCEPT | AFD_POLL_DISCONNECT;
    case EP_EXCLUSIVE_CLASS_WRITE:
        return AFD_POLL_SEND;
    case EP_EXCLUSIVE_CLASS_TERMINAL:
        return AFD_POLL_ABORT | AFD_POLL_CONNECT_FAIL | AFD_POLL_LOCAL_CLOSE;
    default:
        return 0;
    }
}

static void ep_exclusive_filter_classes(uint32_t *delivered,
                                        uint8_t denied_classes)
{
    if ((denied_classes & EP_EXCLUSIVE_CLASS_READ) != 0) {
        *delivered &= ~(EPOLLIN | EPOLLRDNORM | EPOLLRDHUP |
                        EPOLLPRI | EPOLLRDBAND);
    }
    if ((denied_classes & EP_EXCLUSIVE_CLASS_WRITE) != 0) {
        *delivered &= ~(EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND);
    }
    if ((denied_classes & EP_EXCLUSIVE_CLASS_TERMINAL) != 0) {
        *delivered &= ~(EPOLLERR | EPOLLHUP);
    }
}

static void ep_exclusive_unlink_locked(ep_sock_t *sock)
{
    unsigned bucket;

    if (sock->exclusive_claim_classes == 0) {
        assert(sock->exclusive_next == NULL);
        assert(sock->exclusive_prev == NULL);
        return;
    }

    bucket = ep_exclusive_bucket(sock->exclusive_claim_base);
    if (sock->exclusive_prev != NULL) {
        sock->exclusive_prev->exclusive_next = sock->exclusive_next;
    } else {
        assert(g_exclusive_claim_buckets[bucket] == sock);
        g_exclusive_claim_buckets[bucket] = sock->exclusive_next;
    }
    if (sock->exclusive_next != NULL) {
        sock->exclusive_next->exclusive_prev = sock->exclusive_prev;
    }
    sock->exclusive_next = NULL;
    sock->exclusive_prev = NULL;
    sock->exclusive_claim_base = INVALID_SOCKET;
    sock->exclusive_claim_classes = 0;
}

static void ep_exclusive_remove_classes_locked(ep_sock_t *sock,
                                                uint8_t classes)
{
    uint8_t remaining;

    if ((sock->exclusive_claim_classes & classes) == 0) {
        return;
    }
    remaining = sock->exclusive_claim_classes & (uint8_t)~classes;
    if (remaining == 0) {
        ep_exclusive_unlink_locked(sock);
    } else {
        sock->exclusive_claim_classes = remaining;
    }
}

static void ep_exclusive_add_classes_locked(ep_sock_t *sock, SOCKET base,
                                             uint8_t classes)
{
    unsigned bucket;

    if (classes == 0) {
        return;
    }
    if (sock->exclusive_claim_classes != 0 &&
        sock->exclusive_claim_base != base) {
        ep_exclusive_unlink_locked(sock);
    }
    if (sock->exclusive_claim_classes == 0) {
        ep_sock_t *head;

        bucket = ep_exclusive_bucket(base);
        head = g_exclusive_claim_buckets[bucket];
        sock->exclusive_claim_base = base;
        sock->exclusive_prev = NULL;
        sock->exclusive_next = head;
        if (head != NULL) {
            head->exclusive_prev = sock;
        }
        g_exclusive_claim_buckets[bucket] = sock;
    }
    sock->exclusive_claim_classes |= classes;
}

static uint8_t ep_exclusive_quiescent_classes(uint32_t submitted_afd_events)
{
    uint8_t classes = 0;

    for (uint8_t bit = EP_EXCLUSIVE_CLASS_READ;
         bit <= EP_EXCLUSIVE_CLASS_TERMINAL; bit <<= 1) {
        uint32_t class_events = ep_exclusive_class_afd_events(bit);

        if ((class_events & ~submitted_afd_events) == 0) {
            classes |= bit;
        }
    }
    return classes;
}

static int ep_exclusive_try_claim(ep_sock_t *sock, uint32_t *delivered)
{
    SOCKET base = sock->base_socket;
    uint8_t requested_classes = ep_exclusive_classes(*delivered);
    uint8_t denied_classes = 0;
    uint8_t owned_classes = 0;
    uint8_t granted_classes;
    uint8_t missing_classes;
    int owner_terminal = 0;
    int peer_terminal = 0;
    unsigned bucket = ep_exclusive_bucket(base);
    ep_sock_t *claim;

    pthread_mutex_lock(&g_exclusive_lock);
    if (sock->exclusive_claim_classes != 0 &&
        sock->exclusive_claim_base != base) {
        ep_exclusive_unlink_locked(sock);
    }
    for (claim = g_exclusive_claim_buckets[bucket]; claim != NULL;
         claim = claim->exclusive_next) {
        uint8_t claim_classes;

        if (claim->exclusive_claim_base != base) {
            continue;
        }
        claim_classes = claim->exclusive_claim_classes;
        if (claim == sock) {
            owned_classes |= claim_classes;
            if ((claim_classes & EP_EXCLUSIVE_CLASS_TERMINAL) != 0) {
                owner_terminal = 1;
            }
        } else if ((claim_classes & EP_EXCLUSIVE_CLASS_TERMINAL) != 0) {
            peer_terminal = 1;
        } else if ((requested_classes & EP_EXCLUSIVE_CLASS_TERMINAL) == 0) {
            denied_classes |= claim_classes & requested_classes;
        }
    }

    if ((requested_classes & EP_EXCLUSIVE_CLASS_TERMINAL) != 0) {
        if (peer_terminal) {
            denied_classes = requested_classes;
        } else {
            /* The first terminal completion supersedes directional claims.
             * Clearing them under the global lock lets one owner win without
             * deadlocking read and write owners against each other. */
            claim = g_exclusive_claim_buckets[bucket];
            while (claim != NULL) {
                ep_sock_t *next = claim->exclusive_next;

                if (claim->exclusive_claim_base == base) {
                    ep_exclusive_remove_classes_locked(
                        claim, EP_EXCLUSIVE_CLASS_READ |
                                   EP_EXCLUSIVE_CLASS_WRITE);
                }
                claim = next;
            }
        }
    } else if (peer_terminal) {
        denied_classes = requested_classes;
    }

    granted_classes = requested_classes & (uint8_t)~denied_classes;
    if (granted_classes == 0 && requested_classes != 0) {
        pthread_mutex_unlock(&g_exclusive_lock);
        return 0;
    }
    if (owner_terminal) {
        missing_classes = 0;
    } else if ((granted_classes & EP_EXCLUSIVE_CLASS_TERMINAL) != 0) {
        /* One terminal claim covers every bit in this delivered snapshot. */
        missing_classes = EP_EXCLUSIVE_CLASS_TERMINAL;
    } else {
        missing_classes = granted_classes & (uint8_t)~owned_classes;
    }
    ep_exclusive_add_classes_locked(sock, base, missing_classes);
    ep_exclusive_filter_classes(delivered, denied_classes);
    pthread_mutex_unlock(&g_exclusive_lock);
    return 1;
}

/*
 * Keep the owner-release and quiescent-release paths separate: logical detach
 * owns the former, while only a genuinely pending covering AFD request may
 * release another registration's class claim for the same provider socket.
 */
static void ep_exclusive_release_owner(ep_sock_t *sock)
{
    pthread_mutex_lock(&g_exclusive_lock);
    ep_exclusive_unlink_locked(sock);
    pthread_mutex_unlock(&g_exclusive_lock);
}

static void ep_exclusive_release_quiescent(ep_sock_t *sock,
                                           uint32_t submitted_afd_events)
{
    SOCKET base = sock->base_socket;
    uint8_t quiescent_classes =
        ep_exclusive_quiescent_classes(submitted_afd_events);
    unsigned bucket = ep_exclusive_bucket(base);
    ep_sock_t *claim;

    pthread_mutex_lock(&g_exclusive_lock);
    if (sock->exclusive_claim_classes != 0 &&
        sock->exclusive_claim_base != base) {
        /* Claims are keyed by provider base.  Once a provider-chain refresh
         * changes that key, no class on the previous base remains owned by
         * this registration. */
        ep_exclusive_unlink_locked(sock);
    }
    claim = g_exclusive_claim_buckets[bucket];
    while (claim != NULL) {
        ep_sock_t *next = claim->exclusive_next;

        if (claim->exclusive_claim_base == base) {
            ep_exclusive_remove_classes_locked(claim, quiescent_classes);
        }
        claim = next;
    }
    pthread_mutex_unlock(&g_exclusive_lock);
}

static void ep_exclusive_release_inactive(ep_sock_t *sock,
                                          uint8_t inactive_classes)
{
    SOCKET base = sock->base_socket;
    unsigned bucket = ep_exclusive_bucket(base);
    ep_sock_t *claim;

    pthread_mutex_lock(&g_exclusive_lock);
    if (sock->exclusive_claim_classes != 0 &&
        sock->exclusive_claim_base != base) {
        ep_exclusive_unlink_locked(sock);
    }
    claim = g_exclusive_claim_buckets[bucket];
    while (claim != NULL) {
        ep_sock_t *next = claim->exclusive_next;

        if (claim->exclusive_claim_base == base) {
            ep_exclusive_remove_classes_locked(claim, inactive_classes);
        }
        claim = next;
    }
    pthread_mutex_unlock(&g_exclusive_lock);
}

#ifdef _WIN32
static int ep_port_post_iocp(ep_port_t *port, ULONG_PTR completion_key,
                             LPOVERLAPPED overlapped,
                             ep_fault_point_t fault_point, DWORD *error_out)
{
    HANDLE handle;
    DWORD error = ERROR_SUCCESS;
    int posted = 0;

    pthread_mutex_lock(&port->iocp_post_lock);
    handle = port->iocp_post_handle;
    if (handle == NULL) {
        error = ERROR_INVALID_HANDLE;
    } else if (ep_fault_hit(fault_point) != 0) {
        error = ERROR_GEN_FAILURE;
    } else if (port->post_queued_completion_status(
                   handle, 0, completion_key, overlapped)) {
        posted = 1;
    } else {
        error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_GEN_FAILURE;
        }
    }
    pthread_mutex_unlock(&port->iocp_post_lock);

    if (!posted && error_out != NULL) {
        *error_out = error;
    }
    return posted;
}

/* Revoke future posts and return the one HANDLE that this caller owns for
 * closing.  The post lock is the lease: no callback can retain the old
 * numeric HANDLE after this function publishes the revoked alias. */
static HANDLE ep_port_revoke_iocp_posts(ep_port_t *port)
{
    HANDLE handle;
    HANDLE close_handle = NULL;

    pthread_mutex_lock(&port->iocp_post_lock);
    handle = port->iocp_post_handle;
    port->iocp_post_handle = NULL;
    if (atomic_exchange_explicit(&port->iocp_closed, 1,
                                 memory_order_acq_rel) == 0) {
        close_handle = handle;
    }
    pthread_mutex_unlock(&port->iocp_post_lock);
    return close_handle;
}

static void ep_port_fail_iocp_post(ep_port_t *port, DWORD win_error)
{
    HANDLE close_handle;
    int error;
    int expected = 0;

    if (atomic_load_explicit(&port->closing, memory_order_acquire)) {
        return;
    }
    error = ep_winerr_to_errno(win_error);
    if (error <= 0) error = EIO;
    (void)atomic_compare_exchange_strong_explicit(
        &port->iocp_post_error, &expected, error,
        memory_order_release, memory_order_relaxed);
    atomic_fetch_add_explicit(&port->iocp_post_failures, 1,
                              memory_order_relaxed);
    atomic_store_explicit(&port->closing, 1, memory_order_release);
    close_handle = ep_port_revoke_iocp_posts(port);
    if (close_handle != NULL) {
        (void)CloseHandle(close_handle);
    }
}

static int ep_aux_post_completion(ep_sock_t *sock, NTSTATUS status)
{
    DWORD error = ERROR_SUCCESS;

    sock->io_status_block.Status = status;
    sock->io_status_block.Information = 0;
    atomic_store_explicit(&sock->completion_posted, 1,
                          memory_order_release);
    if (!ep_port_post_iocp(sock->port, 0,
                           (LPOVERLAPPED)&sock->io_status_block,
                           EP_FAULT_AUX_POST, &error)) {
        atomic_store_explicit(&sock->completion_posted, 0,
                              memory_order_release);
        ep_port_fail_iocp_post(sock->port, error);
        SetLastError(error);
        return 0;
    }
    return 1;
}

static VOID CALLBACK ep_precise_timeout_callback(
    PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WAIT wait,
    TP_WAIT_RESULT wait_result)
{
    ep_port_t *port = (ep_port_t *)context;
    ULONG_PTR generation;
    DWORD error = ERROR_SUCCESS;

    (void)instance;
    (void)wait;
    (void)wait_result;
    if (port == NULL) {
        return;
    }
    generation = atomic_load_explicit(
        &port->precise_timeout_active_generation, memory_order_acquire);
    if (generation == 0) {
        return;
    }
    if (!ep_port_post_iocp(
            port, generation, &port->precise_timeout_overlapped,
            EP_FAULT_TIMEOUT_POST, &error)) {
        /* The millisecond IOCP deadline remains armed as a backstop.  A
         * timeout callback post is therefore an optimization failure, not a
         * fatal completion-port failure. */
        atomic_fetch_add_explicit(&port->precise_timeout_post_failures, 1,
                                  memory_order_relaxed);
        SetLastError(error);
    }
}

/* The caller owns wait_lock.  Capability failure is cached and is invisible
 * to the public wait: positive timespec waits then use the upward-rounded
 * millisecond IOCP deadline. */
static int ep_port_precise_timeout_initialize(ep_port_t *port)
{
    HANDLE timer = NULL;
    PTP_WAIT wait = NULL;
    int saved_errno = ep_last_err();
    DWORD saved_error = GetLastError();

    if (port->precise_timeout_capability ==
        EP_TIMEOUT_CAPABILITY_AVAILABLE) {
        return 1;
    }
    if (port->precise_timeout_capability ==
        EP_TIMEOUT_CAPABILITY_UNAVAILABLE) {
        return 0;
    }
    if (port->create_waitable_timer_ex_w != NULL &&
        port->query_unbiased_interrupt_time_precise != NULL &&
        ep_fault_hit(EP_FAULT_TIMEOUT_INIT) == 0) {
        timer = port->create_waitable_timer_ex_w(
            NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (timer != NULL) {
            wait = CreateThreadpoolWait(ep_precise_timeout_callback,
                                        port, NULL);
        }
    }
    if (timer == NULL || wait == NULL) {
        if (wait != NULL) {
            CloseThreadpoolWait(wait);
        }
        if (timer != NULL) {
            (void)CloseHandle(timer);
        }
        port->precise_timeout_capability =
            EP_TIMEOUT_CAPABILITY_UNAVAILABLE;
        ep_set_errno(saved_errno);
        SetLastError(saved_error);
        return 0;
    }

    port->precise_timeout_timer = timer;
    port->precise_timeout_wait = wait;
    port->precise_timeout_capability = EP_TIMEOUT_CAPABILITY_AVAILABLE;
    ep_set_errno(saved_errno);
    SetLastError(saved_error);
    return 1;
}

/* Arm one relative high-resolution deadline.  Generation zero is reserved
 * for non-timeout/control IOCP packets.  The caller owns wait_lock. */
static ULONG_PTR ep_port_precise_timeout_arm(ep_port_t *port,
                                             uint64_t delay_100ns)
{
    LARGE_INTEGER due_time;
    ULONG_PTR generation;
    uint64_t timer_delay = delay_100ns;
    int saved_errno = ep_last_err();
    DWORD saved_error = GetLastError();

    if (delay_100ns == 0 ||
        !ep_port_precise_timeout_initialize(port)) {
        return 0;
    }
    generation = ++port->precise_timeout_generation;
    if (generation == 0) {
        generation = ++port->precise_timeout_generation;
    }
    if (timer_delay > (uint64_t)INT64_MAX) {
        timer_delay = (uint64_t)INT64_MAX;
    }
    due_time.QuadPart = -(LONGLONG)timer_delay;
    atomic_store_explicit(&port->precise_timeout_active_generation,
                          generation, memory_order_release);

    if (ep_fault_hit(EP_FAULT_TIMEOUT_ARM) != 0 ||
        !SetWaitableTimer(port->precise_timeout_timer, &due_time, 0,
                          NULL, NULL, FALSE)) {
        atomic_store_explicit(&port->precise_timeout_active_generation, 0,
                              memory_order_release);
        ep_set_errno(saved_errno);
        SetLastError(saved_error);
        return 0;
    }
    SetThreadpoolWait(port->precise_timeout_wait,
                      port->precise_timeout_timer, NULL);
    atomic_store_explicit(&port->precise_timeout_armed, 1,
                          memory_order_release);
    ep_set_errno(saved_errno);
    SetLastError(saved_error);
    return generation;
}

/* Synchronous disassociation is mandatory: epfd_put() may make this wait the
 * last public reference and destroy the port immediately after return. */
static void ep_port_precise_timeout_disarm(ep_port_t *port)
{
    if (!atomic_load_explicit(&port->precise_timeout_armed,
                              memory_order_acquire)) {
        return;
    }
    (void)CancelWaitableTimer(port->precise_timeout_timer);
    SetThreadpoolWait(port->precise_timeout_wait, NULL, NULL);
    WaitForThreadpoolWaitCallbacks(port->precise_timeout_wait, TRUE);
    atomic_store_explicit(&port->precise_timeout_active_generation, 0,
                          memory_order_release);
    atomic_store_explicit(&port->precise_timeout_armed, 0,
                          memory_order_release);
}

static void ep_port_precise_timeout_destroy(ep_port_t *port)
{
    ep_port_precise_timeout_disarm(port);
    if (port->precise_timeout_wait != NULL) {
        CloseThreadpoolWait(port->precise_timeout_wait);
        port->precise_timeout_wait = NULL;
    }
    if (port->precise_timeout_timer != NULL) {
        (void)CloseHandle(port->precise_timeout_timer);
        port->precise_timeout_timer = NULL;
    }
}

static void ep_aux_wait_callback_idle(ep_sock_t *sock)
{
    while (atomic_load_explicit(&sock->callback_active,
                                memory_order_acquire) != 0) {
        Sleep(0);
    }
}

static VOID CALLBACK ep_waitable_callback(PVOID parameter, BOOLEAN timer_or_wait_fired)
{
    ep_sock_t *sock = (ep_sock_t *)parameter;

    (void)timer_or_wait_fired;
    if (sock == NULL || sock->port == NULL) {
        return;
    }
    atomic_store_explicit(&sock->callback_active, 1, memory_order_release);
    if (sock->waitable_semantics != EP_WAITABLE_PERSISTENT &&
        sock->waitable_semantics != EP_WAITABLE_TERMINAL) {
        /* Auto-reset events, semaphores, and mode-unknown waitables may have
         * been consumed by the wait that entered this callback.  Publish
         * ownership before the IOCP packet so MOD/cancellation cannot lose
         * the observation. */
        atomic_store_explicit(&sock->waitable_notification_owned, 1,
                              memory_order_release);
    }
    if (!ep_aux_post_completion(sock, STATUS_SUCCESS)) {
        /* Best-effort wake; close/drain paths recover outstanding state. */
    }
    atomic_store_explicit(&sock->callback_active, 0, memory_order_release);
}

static int ep_object_type_equals(const UNICODE_STRING *type_name,
                                 const wchar_t *expected,
                                 size_t expected_bytes)
{
    return type_name->Buffer != NULL &&
        type_name->Length == expected_bytes &&
        memcmp(type_name->Buffer, expected, expected_bytes) == 0;
}

typedef struct ep_event_basic_information {
    ULONG event_type;
    LONG event_state;
} ep_event_basic_information_t;

static uint8_t ep_event_waitable_semantics(HANDLE handle)
{
    ep_event_basic_information_t info;
    ULONG return_length = 0;
    NTSTATUS status;

    if (g_ntdll.NtQueryEvent == NULL) {
        return EP_WAITABLE_ET_UNSUPPORTED;
    }
    memset(&info, 0, sizeof(info));
    status = g_ntdll.NtQueryEvent(handle, 0, &info, (ULONG)sizeof(info),
                                  &return_length);
    if (status < 0) {
        return EP_WAITABLE_ET_UNSUPPORTED;
    }
    if (info.event_type == 0) {
        return EP_WAITABLE_PERSISTENT; /* manual-reset notification event */
    }
    if (info.event_type == 1) {
        return EP_WAITABLE_CONSUMPTIVE; /* auto-reset synchronization event */
    }
    return EP_WAITABLE_ET_UNSUPPORTED;
}

/* Return 1 when a HANDLE may be passed to the Windows wait APIs, 0 when its
 * access mask cannot be queried, and -2 when it lacks SYNCHRONIZE. */
static int ep_handle_wait_access(HANDLE handle)
{
    PUBLIC_OBJECT_BASIC_INFORMATION info;
    ULONG return_length = 0;
    NTSTATUS status;

    memset(&info, 0, sizeof(info));
    status = g_ntdll.NtQueryObject(
        handle, (ULONG)ObjectBasicInformation, &info, (ULONG)sizeof(info),
        &return_length);
    if (status < 0) {
        return 0;
    }
    return (info.GrantedAccess & SYNCHRONIZE) != 0 ? 1 : -2;
}

/* Return 1 for a supported waitable object, 0 for an invalid/unrecognized
 * handle, -1 for a valid object type whose wait semantics are unsafe or
 * unsupported (for example a mutex, which a wait would acquire), and -2 for
 * a supported object whose HANDLE lacks SYNCHRONIZE access. */
static int ep_handle_waitability(HANDLE handle, DWORD file_type,
                                 uint8_t *semantics_out)
{
    ULONG_PTR storage[128];
    PUBLIC_OBJECT_TYPE_INFORMATION *type_info;
    ULONG return_length = 0;
    NTSTATUS status;

    *semantics_out = EP_WAITABLE_ET_UNSUPPORTED;

    if (handle == NULL || handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    if (file_type == FILE_TYPE_UNKNOWN) {
        if (g_ntdll.NtQueryObject == NULL) {
            return 0;
        }
        memset(storage, 0, sizeof(storage));
        status = g_ntdll.NtQueryObject(
            handle, (ULONG)ObjectTypeInformation, storage,
            (ULONG)sizeof(storage), &return_length);
        if (status < 0) {
            return 0;
        }
        type_info = (PUBLIC_OBJECT_TYPE_INFORMATION *)storage;
        if (ep_object_type_equals(&type_info->TypeName, L"Event",
                                  sizeof(L"Event") - sizeof(wchar_t))) {
            *semantics_out = ep_event_waitable_semantics(handle);
            return ep_handle_wait_access(handle);
        }
        if (ep_object_type_equals(&type_info->TypeName, L"Semaphore",
                                  sizeof(L"Semaphore") - sizeof(wchar_t))) {
            *semantics_out = EP_WAITABLE_CONSUMPTIVE;
            return ep_handle_wait_access(handle);
        }
        if (ep_object_type_equals(&type_info->TypeName, L"Timer",
                                  sizeof(L"Timer") - sizeof(wchar_t))) {
            /* The native query surface does not expose manual-reset versus
             * synchronization-timer mode, so ET cannot be sampled safely. */
            *semantics_out = EP_WAITABLE_ET_UNSUPPORTED;
            return ep_handle_wait_access(handle);
        }
        if (ep_object_type_equals(&type_info->TypeName, L"Process",
                                  sizeof(L"Process") - sizeof(wchar_t)) ||
            ep_object_type_equals(&type_info->TypeName, L"Thread",
                                  sizeof(L"Thread") - sizeof(wchar_t))) {
            *semantics_out = EP_WAITABLE_TERMINAL;
            return ep_handle_wait_access(handle);
        }
        return -1;
    }
    /* Pipes and disk files are classified before this helper.  Do not probe
     * other file-like handles with a wait that may consume object state or
     * admit types outside RegisterWaitForSingleObject's contract. */
    return -1;
}

static uint32_t ep_waitable_interest_events(uint32_t user_events)
{
    return user_events &
        (EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLRDNORM | EPOLLWRNORM |
         EPOLLRDBAND | EPOLLWRBAND | EPOLLRDHUP);
}

static uint32_t ep_waitable_level_events(const ep_sock_t *sock)
{
    return ep_waitable_interest_events(sock->user_events);
}

static int ep_waitable_may_consume(const ep_sock_t *sock)
{
    return sock->kind == EP_REG_WAITABLE &&
        sock->waitable_semantics != EP_WAITABLE_PERSISTENT &&
        sock->waitable_semantics != EP_WAITABLE_TERMINAL;
}

static int ep_waitable_is_dormant(const ep_sock_t *sock)
{
    return sock->kind == EP_REG_WAITABLE &&
        ep_waitable_interest_events(sock->user_events) == 0;
}

static int ep_socket_select_ready(SOCKET fd, int writable)
{
    fd_set descriptors;
    struct timeval timeout;
    int result;

    FD_ZERO(&descriptors);
    FD_SET(fd, &descriptors);
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    result = writable
        ? select(0, NULL, &descriptors, NULL, &timeout)
        : select(0, &descriptors, NULL, NULL, &timeout);
    if (result == SOCKET_ERROR) {
        return -1;
    }
    return result > 0 && FD_ISSET(fd, &descriptors);
}

static int ep_waitable_register_locked(ep_sock_t *sock)
{
    HANDLE registration = NULL;

    if (sock->wait_registration != NULL) {
        return 0;
    }
    if (!RegisterWaitForSingleObject(
            &registration,
            (HANDLE)sock->fd,
            ep_waitable_callback,
            sock,
            INFINITE,
            WT_EXECUTEONLYONCE | WT_EXECUTEINWAITTHREAD)) {
        ep_set_errno(ep_winerr_to_errno(GetLastError()));
        return -1;
    }
    sock->wait_registration = registration;
    return 0;
}

static int ep_waitable_unregister_locked(ep_sock_t *sock)
{
    HANDLE registration = sock->wait_registration;

    if (registration == NULL) {
        ep_aux_wait_callback_idle(sock);
        return 0;
    }
    if (ep_fault_hit(EP_FAULT_AUX_DISARM) != 0) {
        int error = ep_last_err();

        ep_aux_wait_callback_idle(sock);
        ep_set_errno(error);
        return -1;
    }
    if (!UnregisterWaitEx(registration, INVALID_HANDLE_VALUE)) {
        DWORD error = GetLastError();
        if (error != ERROR_INVALID_HANDLE) {
            ep_aux_wait_callback_idle(sock);
            ep_set_errno(ep_winerr_to_errno(error));
            return -1;
        }
    }
    ep_aux_wait_callback_idle(sock);
    sock->wait_registration = NULL;
    return 0;
}

static int ep_pipe_query_access(HANDLE handle, uint8_t *access_out)
{
    PUBLIC_OBJECT_BASIC_INFORMATION info;
    ULONG return_length = 0;
    NTSTATUS status;
    uint8_t access = EP_PIPE_ACCESS_NONE;

    memset(&info, 0, sizeof(info));
    status = g_ntdll.NtQueryObject(
        handle, (ULONG)ObjectBasicInformation, &info, (ULONG)sizeof(info),
        &return_length);
    if (status < 0) {
        ep_set_errno(ep_winerr_to_errno(ep_ntstatus_to_winerr(status)));
        return -1;
    }
    if ((info.GrantedAccess &
         (FILE_READ_DATA | GENERIC_READ | GENERIC_ALL)) != 0) {
        access |= EP_PIPE_ACCESS_READ;
    }
    if ((info.GrantedAccess &
         (FILE_WRITE_DATA | GENERIC_WRITE | GENERIC_ALL)) != 0) {
        access |= EP_PIPE_ACCESS_WRITE;
    }
    if (access == EP_PIPE_ACCESS_NONE) {
        ep_set_errno(EACCES);
        return -1;
    }
    *access_out = access;
    return 0;
}

typedef struct ep_pipe_local_information {
    ULONG named_pipe_type;
    ULONG named_pipe_configuration;
    ULONG maximum_instances;
    ULONG current_instances;
    ULONG inbound_quota;
    ULONG read_data_available;
    ULONG outbound_quota;
    ULONG write_quota_available;
    ULONG named_pipe_state;
    ULONG named_pipe_end;
} ep_pipe_local_information_t;

typedef struct ep_pipe_snapshot {
    uint32_t events;
    uint8_t local_closed;
    uint8_t valid;
    uint8_t native_information;
    uint8_t server_end;
} ep_pipe_snapshot_t;

static uint32_t ep_pipe_read_events(const ep_sock_t *sock)
{
    return sock->user_events & (EPOLLIN | EPOLLRDNORM);
}

static uint32_t ep_pipe_write_events(const ep_sock_t *sock)
{
    return sock->user_events & (EPOLLOUT | EPOLLWRNORM);
}

static uint32_t ep_pipe_peer_closed_events(const ep_sock_t *sock,
                                            ULONG available)
{
    uint32_t events = 0;

    if ((sock->pipe_access & EP_PIPE_ACCESS_READ) != 0) {
        if (available > 0) {
            events |= ep_pipe_read_events(sock);
        }
        events |= EPOLLHUP;
    }
    if ((sock->pipe_access & EP_PIPE_ACCESS_WRITE) != 0) {
        events |= ep_pipe_write_events(sock) | EPOLLERR;
    }
    return events;
}

static ep_pipe_snapshot_t ep_pipe_fallback_snapshot(const ep_sock_t *sock)
{
    ep_pipe_snapshot_t snapshot = {0};
    DWORD available = 0;

    if (PeekNamedPipe((HANDLE)sock->fd, NULL, 0, NULL, &available, NULL)) {
        snapshot.valid = 1;
        if ((sock->pipe_access & EP_PIPE_ACCESS_READ) != 0 &&
            available > 0) {
            snapshot.events |= ep_pipe_read_events(sock);
        }
        if ((sock->pipe_access & EP_PIPE_ACCESS_WRITE) != 0) {
            snapshot.events |= ep_pipe_write_events(sock);
        }
        return snapshot;
    }

    switch (GetLastError()) {
    case ERROR_INVALID_HANDLE:
        snapshot.local_closed = 1;
        break;
    case ERROR_BROKEN_PIPE:
    case ERROR_PIPE_NOT_CONNECTED:
    case ERROR_BAD_PIPE:
        snapshot.valid = 1;
        snapshot.events = ep_pipe_peer_closed_events(sock, 0);
        break;
    case ERROR_ACCESS_DENIED:
        /* Pure write-only pipe handles reject PeekNamedPipe.  Without the
         * optional native query, retain advisory writable readiness; peer
         * closure cannot be distinguished on this fallback path. */
        snapshot.valid = 1;
        if ((sock->pipe_access & EP_PIPE_ACCESS_WRITE) != 0) {
            snapshot.events |= ep_pipe_write_events(sock);
        }
        break;
    default:
        break;
    }
    return snapshot;
}

static ep_pipe_snapshot_t ep_pipe_snapshot(const ep_sock_t *sock)
{
    ep_pipe_local_information_t info;
    ep_pipe_snapshot_t snapshot = {0};
    IO_STATUS_BLOCK io_status_block;
    NTSTATUS status;

    if (g_ntdll.NtQueryInformationFile == NULL) {
        return ep_pipe_fallback_snapshot(sock);
    }

    memset(&info, 0, sizeof(info));
    memset(&io_status_block, 0, sizeof(io_status_block));
    /* NtQueryInformationFile is a synchronous metadata query: its documented
     * contract returns STATUS_SUCCESS or an error, unlike the read/write NT
     * routines that can leave caller storage live behind STATUS_PENDING. */
    status = g_ntdll.NtQueryInformationFile(
        (HANDLE)sock->fd, &io_status_block, &info, (ULONG)sizeof(info),
        EP_FILE_PIPE_LOCAL_INFORMATION_CLASS);
    if (status != STATUS_SUCCESS ||
        io_status_block.Status != STATUS_SUCCESS ||
        io_status_block.Information != sizeof(info)) {
        DWORD error;

        if ((uint32_t)status == EP_STATUS_INVALID_HANDLE) {
            snapshot.local_closed = 1;
            return snapshot;
        }
        error = ep_ntstatus_to_winerr(
            status != STATUS_SUCCESS ? status : io_status_block.Status);
        if (error == ERROR_INVALID_HANDLE) {
            snapshot.local_closed = 1;
            return snapshot;
        }
        if (error == ERROR_BROKEN_PIPE ||
            error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_BAD_PIPE) {
            snapshot.valid = 1;
            snapshot.events = ep_pipe_peer_closed_events(sock, 0);
            return snapshot;
        }
        if (error == ERROR_ACCESS_DENIED) {
            return ep_pipe_fallback_snapshot(sock);
        }
        /* A transient or provider-specific query error is not evidence of
         * readiness.  The timer path retries without fabricating writable
         * state; only the explicit access-denied compatibility path remains
         * advisory. */
        return snapshot;
    }

    snapshot.valid = 1;
    snapshot.native_information = 1;
    snapshot.server_end = info.named_pipe_end == EP_PIPE_SERVER_END;

    switch (info.named_pipe_state) {
    case EP_PIPE_STATE_CONNECTED:
        if ((sock->pipe_access & EP_PIPE_ACCESS_READ) != 0 &&
            info.read_data_available > 0) {
            snapshot.events |= ep_pipe_read_events(sock);
        }
        if ((sock->pipe_access & EP_PIPE_ACCESS_WRITE) != 0 &&
            info.write_quota_available > 0) {
            snapshot.events |= ep_pipe_write_events(sock);
        }
        break;
    case EP_PIPE_STATE_DISCONNECTED:
    case EP_PIPE_STATE_CLOSING:
        snapshot.events = ep_pipe_peer_closed_events(
            sock, info.read_data_available);
        break;
    case EP_PIPE_STATE_LISTENING:
    default:
        break;
    }
    return snapshot;
}

static int ep_pipe_et_events_have_final_shape(const ep_sock_t *sock,
                                              uint32_t events)
{
    if ((events & EPOLLHUP) != 0 &&
        (events & (EPOLLIN | EPOLLRDNORM)) == 0) {
        return 1;
    }
    return (events & EPOLLERR) != 0 &&
        (sock->pipe_access & EP_PIPE_ACCESS_READ) == 0;
}

static int ep_pipe_et_snapshot_is_final(const ep_sock_t *sock,
                                        uint32_t events,
                                        const ep_pipe_snapshot_t *snapshot)
{
    if (!snapshot->valid || !snapshot->native_information ||
        snapshot->server_end) {
        /* A named-pipe server HANDLE can be disconnected and connected to a
         * later client without changing its numeric value.  Keep it eligible
         * for resampling after terminal ET delivery.  Unknown fallback ends
         * are treated conservatively for the same reason. */
        return 0;
    }
    return ep_pipe_et_events_have_final_shape(sock, events);
}

static uint32_t ep_pipe_et_events(ep_sock_t *sock,
                                  const ep_pipe_snapshot_t *snapshot)
{
    uint32_t level;
    uint32_t previous;
    uint32_t rising;
    uint32_t falling;
    uint32_t delivered;

    if (!snapshot->valid) {
        /* An unavailable metadata sample is not evidence that readiness fell.
         * Retain the edge latch so a later successful retry cannot duplicate
         * an unchanged level. */
        return 0;
    }

    level = snapshot->events;
    previous = sock->observed_events;
    rising = level & ~previous;
    falling = previous & ~level;

    if ((rising & (EPOLLHUP | EPOLLERR)) != 0) {
        /* Linux includes the current normal aliases when a terminal condition
         * first appears (for example IN -> IN|HUP and OUT -> OUT|ERR). */
        delivered = level;
    } else if ((level & EPOLLHUP) != 0 &&
               (falling & (EPOLLIN | EPOLLRDNORM)) != 0) {
        /* Draining buffered pipe data after EOF produces one final HUP-only
         * snapshot.  This is the one falling transition that forms another
         * pipe event; ordinary IN/OUT disappearance must stay silent. */
        delivered = level;
    } else {
        delivered = rising;
    }

    sock->observed_events = level;
    return delivered;
}

static VOID CALLBACK ep_pipe_timer_callback(PVOID parameter, BOOLEAN fired)
{
    ep_sock_t *sock = (ep_sock_t *)parameter;

    (void)fired;
    if (sock == NULL || sock->port == NULL) {
        return;
    }
    atomic_store_explicit(&sock->callback_active, 1, memory_order_release);
    if (!ep_aux_post_completion(sock, STATUS_SUCCESS)) {
        /* Best-effort; cancel/close recover outstanding poll state. */
    }
    atomic_store_explicit(&sock->callback_active, 0, memory_order_release);
}

static int ep_pipe_delete_timer_locked(ep_sock_t *sock)
{
    HANDLE timer = sock->wait_registration;

    if (timer == NULL) {
        ep_aux_wait_callback_idle(sock);
        return 0;
    }
    if (ep_fault_hit(EP_FAULT_AUX_DISARM) != 0) {
        int error = ep_last_err();

        ep_aux_wait_callback_idle(sock);
        ep_set_errno(error);
        return -1;
    }
    if (!DeleteTimerQueueTimer(NULL, timer, INVALID_HANDLE_VALUE)) {
        DWORD error = GetLastError();
        if (error != ERROR_INVALID_HANDLE) {
            ep_aux_wait_callback_idle(sock);
            ep_set_errno(ep_winerr_to_errno(error));
            return -1;
        }
    }
    ep_aux_wait_callback_idle(sock);
    sock->wait_registration = NULL;
    return 0;
}

static int ep_pipe_schedule_locked(ep_sock_t *sock, uint32_t level)
{
    HANDLE timer = NULL;

    if (sock->wait_registration != NULL) {
        return 0;
    }
    if (level != 0) {
        if (!ep_aux_post_completion(sock, STATUS_SUCCESS)) {
            ep_set_errno(ep_winerr_to_errno(GetLastError()));
            return -1;
        }
        return 0;
    }
    /* Anonymous pipes are not waitable.  Poll with a short one-shot timer
     * while armed; completion re-evaluates readiness. */
    if (!CreateTimerQueueTimer(&timer, NULL, ep_pipe_timer_callback, sock,
                              1, 0, WT_EXECUTEONLYONCE)) {
        ep_set_errno(ep_winerr_to_errno(GetLastError()));
        return -1;
    }
    sock->wait_registration = timer;
    return 0;
}
#endif

static _Atomic uint64_t g_active_quarantines;
static _Atomic uint64_t g_reaped_ports;
static _Atomic uint64_t g_irrecoverable_ports;

static void ep_port_record_async_error_locked(ep_port_t *port, int error)
{
    if (error <= 0) error = EIO;
    port->asynchronous_errors++;
    if (port->async_error != 0) return;

    port->async_error = error;
    if (!atomic_load_explicit(&port->closing, memory_order_acquire) &&
        !atomic_load_explicit(&port->waiter_active, memory_order_acquire) &&
        !atomic_load_explicit(&port->iocp_closed, memory_order_acquire)) {
        DWORD win_error = ERROR_SUCCESS;

        if (!ep_port_post_iocp(port, 0, NULL, EP_FAULT_IOCP_POST,
                               &win_error)) {
            ep_port_fail_iocp_post(port, win_error);
        }
    }
}

static int ep_port_take_async_error_locked(ep_port_t *port)
{
    int error = port->async_error;

    port->async_error = 0;
    return error;
}

static int ep_port_take_iocp_post_error(ep_port_t *port)
{
    return atomic_exchange_explicit(&port->iocp_post_error, 0,
                                    memory_order_acq_rel);
}

void ep_get_global_stats(wepoll_ex_global_stats *stats)
{
    memset(stats, 0, sizeof(*stats));
    stats->version = WEPOLL_EX_STATS_VERSION;
    stats->struct_size = (uint32_t)sizeof(*stats);
    stats->quarantined_ports = atomic_load_explicit(
        &g_quarantined_ports, memory_order_relaxed);
    stats->active_quarantines = atomic_load_explicit(
        &g_active_quarantines, memory_order_relaxed);
    stats->reaped_ports = atomic_load_explicit(
        &g_reaped_ports, memory_order_relaxed);
    stats->irrecoverable_ports = atomic_load_explicit(
        &g_irrecoverable_ports, memory_order_relaxed);
    stats->api_close_timeouts = ep_api_close_timeout_count();
}

int ep_port_get_stats(ep_port_t *port, wepoll_ex_stats *stats)
{
    if (port == NULL || stats == NULL) {
        ep_set_errno(EFAULT);
        return -1;
    }

    memset(stats, 0, sizeof(*stats));
    stats->version = WEPOLL_EX_STATS_VERSION;
    stats->struct_size = (uint32_t)sizeof(*stats);
#if defined(WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME)
    stats->socket_lifetime_policy =
        WEPOLL_EX_SOCKET_LIFETIME_SYNCHRONIZED;
#elif defined(WEPOLL_EX_STRICT_SOCKET_IDENTITY)
    stats->socket_lifetime_policy = WEPOLL_EX_SOCKET_LIFETIME_STRICT;
#else
    stats->socket_lifetime_policy = WEPOLL_EX_SOCKET_LIFETIME_BEST_EFFORT;
#endif

    pthread_mutex_lock(&port->fd_table_lock);
    stats->active_registrations = port->fd_table_count;
    stats->pending_polls = port->pending_poll_count;
    stats->rearm_queue_depth = port->needs_rearm_count;
    stats->oneshot_probe_queue_depth = port->oneshot_fired_count;
    stats->rearm_work_items = port->rearm_work_visits +
                              port->oneshot_probe_visits;
    stats->stale_events_dropped = port->stale_events_dropped;
    stats->identity_failures = port->identity_failures;
    stats->asynchronous_errors = port->asynchronous_errors +
        atomic_load_explicit(&port->iocp_post_failures,
                             memory_order_relaxed);
    stats->zero_timeout_budget_hits = port->zero_timeout_budget_hits;
    pthread_mutex_unlock(&port->fd_table_lock);

    stats->ready_queue_depth = atomic_load_explicit(
        &port->ready_queue.queued, memory_order_relaxed);
    stats->afd_pool_in_use = atomic_load_explicit(
        &port->afd_info_pool.in_use, memory_order_relaxed);
    stats->afd_pool_peak = atomic_load_explicit(
        &port->afd_info_pool.peak, memory_order_relaxed);
    stats->ready_pool_in_use = atomic_load_explicit(
        &port->ready_node_pool.in_use, memory_order_relaxed);
    stats->ready_pool_peak = atomic_load_explicit(
        &port->ready_node_pool.peak, memory_order_relaxed);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Time and fd-table helpers.                                                */
/* ------------------------------------------------------------------------- */

static uint64_t ep_saturating_mul_add_u64(uint64_t value, uint64_t multiplier,
                                          uint64_t addend)
{
    if (value > (UINT64_MAX - addend) / multiplier) {
        return UINT64_MAX;
    }
    return value * multiplier + addend;
}

void ep_wait_timeout_from_milliseconds(int timeout_ms,
                                       ep_wait_timeout_t *timeout)
{
    memset(timeout, 0, sizeof(*timeout));
    if (timeout_ms < 0) {
        timeout->infinite = 1;
        return;
    }
    timeout->milliseconds = (uint64_t)timeout_ms;
    timeout->intervals_100ns =
        (uint64_t)timeout_ms * EP_100NS_PER_MILLISECOND;
}

int ep_wait_timeout_from_timespec(const struct timespec *timespec,
                                  ep_wait_timeout_t *timeout)
{
    uint64_t seconds;
    uint64_t millis_remainder;
    uint64_t intervals_remainder;
    int intervals_fit;

    if (timeout == NULL) {
        ep_set_errno(EFAULT);
        return -1;
    }
    memset(timeout, 0, sizeof(*timeout));
    if (timespec == NULL) {
        timeout->infinite = 1;
        return 0;
    }
    if (timespec->tv_sec < 0 || timespec->tv_nsec < 0 ||
        timespec->tv_nsec >= 1000000000L) {
        ep_set_errno(EINVAL);
        return -1;
    }

    seconds = (uint64_t)timespec->tv_sec;
    millis_remainder =
        ((uint64_t)timespec->tv_nsec + UINT64_C(999999)) /
        UINT64_C(1000000);
    intervals_remainder =
        ((uint64_t)timespec->tv_nsec + UINT64_C(99)) / UINT64_C(100);
    timeout->milliseconds = ep_saturating_mul_add_u64(
        seconds, UINT64_C(1000), millis_remainder);
    intervals_fit =
        seconds <= (UINT64_MAX - intervals_remainder) / UINT64_C(10000000);
    timeout->intervals_100ns = ep_saturating_mul_add_u64(
        seconds, UINT64_C(10000000), intervals_remainder);
    /* Keep the exact millisecond/chunked path for durations whose 100-ns
     * representation would saturate.  Treating UINT64_MAX as the precise
     * deadline would otherwise expire a still-representable long wait early. */
    timeout->precise = intervals_fit;
    return 0;
}

static uint64_t ep_now_ns(void)
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    uint64_t seconds;
    uint64_t remainder;

    if (!QueryPerformanceFrequency(&frequency)) {
        return 0;
    }
    if (!QueryPerformanceCounter(&counter) || frequency.QuadPart <= 0) {
        return 0;
    }

    seconds = (uint64_t)(counter.QuadPart / frequency.QuadPart);
    remainder = (uint64_t)(counter.QuadPart % frequency.QuadPart);
    return seconds * UINT64_C(1000000000) +
           (remainder * UINT64_C(1000000000)) /
               (uint64_t)frequency.QuadPart;
}

static int ep_fd_table_grow(ep_port_t *port, size_t new_size)
{
    ep_sock_t **new_table;

    if (new_size == 0 || new_size > SIZE_MAX / sizeof(*new_table)) {
        ep_set_errno(ENOMEM);
        return -1;
    }
    new_table = (ep_sock_t **)calloc(new_size, sizeof(*new_table));
    if (new_table == NULL) {
        ep_set_errno(ENOMEM);
        return -1;
    }

    for (size_t i = 0; i < port->fd_table_size; i++) {
        ep_sock_t *sock = port->fd_table[i];
        if (sock != NULL) {
            size_t slot = (size_t)sock->fd % new_size;
            while (new_table[slot] != NULL) {
                slot = (slot + 1) % new_size;
            }
            new_table[slot] = sock;
        }
    }

    free(port->fd_table);
    port->fd_table = new_table;
    port->fd_table_size = new_size;
    return 0;
}

static ep_sock_t *ep_fd_table_lookup(ep_port_t *port, SOCKET fd)
{
    if (port->fd_table_size == 0) {
        return NULL;
    }

    size_t slot = (size_t)fd % port->fd_table_size;
    for (size_t probes = 0; probes < port->fd_table_size; probes++) {
        ep_sock_t *sock = port->fd_table[slot];
        if (sock == NULL) {
            return NULL;
        }
        if (sock->fd == fd) {
            return sock;
        }
        slot = (slot + 1) % port->fd_table_size;
    }
    return NULL;
}

static int ep_fd_table_insert(ep_port_t *port, ep_sock_t *sock)
{
    if (port->fd_table_size == 0 ||
        port->fd_table_count * 4 >= port->fd_table_size * 3) {
        size_t new_size;

        if (port->fd_table_size == 0) {
            new_size = WEPOLL_INITIAL_FDS;
        } else {
            if (port->fd_table_size > SIZE_MAX / 2) {
                ep_set_errno(ENOMEM);
                return -1;
            }
            new_size = port->fd_table_size * 2;
        }
        if (ep_fd_table_grow(port, new_size) != 0) {
            return -1;
        }
    }

    size_t slot = (size_t)sock->fd % port->fd_table_size;
    while (port->fd_table[slot] != NULL) {
        slot = (slot + 1) % port->fd_table_size;
    }
    port->fd_table[slot] = sock;
    port->fd_table_count++;
    return 0;
}

static void ep_fd_table_remove(ep_port_t *port, ep_sock_t *sock)
{
    if (port->fd_table_size == 0) {
        return;
    }

    size_t slot = (size_t)sock->fd % port->fd_table_size;
    while (port->fd_table[slot] != sock) {
        if (port->fd_table[slot] == NULL) {
            return;
        }
        slot = (slot + 1) % port->fd_table_size;
    }

    port->fd_table[slot] = NULL;
    port->fd_table_count--;

    for (;;) {
        size_t scan = (slot + 1) % port->fd_table_size;
        ep_sock_t *moved = port->fd_table[scan];
        if (moved == NULL) {
            break;
        }

        port->fd_table[scan] = NULL;
        size_t target = (size_t)moved->fd % port->fd_table_size;
        while (port->fd_table[target] != NULL) {
            target = (target + 1) % port->fd_table_size;
        }
        port->fd_table[target] = moved;
        slot = scan;
    }
}

/* ------------------------------------------------------------------------- */
/* Socket allocation and poll lifecycle.                                    */
/* ------------------------------------------------------------------------- */

static void ep_sock_list_add_locked(ep_port_t *port, ep_sock_t *sock)
{
    sock->prev = NULL;
    sock->next = port->sock_list_head;
    if (port->sock_list_head != NULL) {
        port->sock_list_head->prev = sock;
    }
    port->sock_list_head = sock;
}

static void ep_sock_list_remove_locked(ep_port_t *port, ep_sock_t *sock)
{
    if (sock->prev != NULL) {
        sock->prev->next = sock->next;
    } else {
        port->sock_list_head = sock->next;
    }
    if (sock->next != NULL) {
        sock->next->prev = sock->prev;
    }
    sock->next = NULL;
    sock->prev = NULL;
}

static void ep_rearm_list_append_locked(ep_port_t *port, ep_sock_t *sock)
{
    assert(sock->rearm_next == NULL);
    assert(sock->rearm_prev == NULL);

    sock->rearm_prev = port->rearm_tail;
    if (port->rearm_tail != NULL) {
        port->rearm_tail->rearm_next = sock;
    } else {
        port->rearm_head = sock;
    }
    port->rearm_tail = sock;
}

static void ep_rearm_list_remove_locked(ep_port_t *port, ep_sock_t *sock)
{
    if (sock->rearm_prev != NULL) {
        sock->rearm_prev->rearm_next = sock->rearm_next;
    } else {
        assert(port->rearm_head == sock);
        port->rearm_head = sock->rearm_next;
    }
    if (sock->rearm_next != NULL) {
        sock->rearm_next->rearm_prev = sock->rearm_prev;
    } else {
        assert(port->rearm_tail == sock);
        port->rearm_tail = sock->rearm_prev;
    }
    sock->rearm_next = NULL;
    sock->rearm_prev = NULL;
}

static void ep_rearm_list_rotate_locked(ep_port_t *port, ep_sock_t *sock)
{
    assert(port->rearm_head == sock);
    if (port->rearm_tail == sock) {
        return;
    }

    port->rearm_head = sock->rearm_next;
    port->rearm_head->rearm_prev = NULL;
    sock->rearm_next = NULL;
    sock->rearm_prev = port->rearm_tail;
    port->rearm_tail->rearm_next = sock;
    port->rearm_tail = sock;
}

static int ep_port_has_deferred_rearm_locked(const ep_port_t *port)
{
    const ep_sock_t *sock;

    for (sock = port->rearm_head; sock != NULL; sock = sock->rearm_next) {
        if (sock->et_holdoff) {
            return 1;
        }
    }
    return 0;
}

static void ep_port_release_deferred_rearms_locked(ep_port_t *port)
{
    ep_sock_t *sock;

    for (sock = port->rearm_head; sock != NULL; sock = sock->rearm_next) {
        sock->et_holdoff = 0;
    }
}

static void ep_oneshot_list_append_locked(ep_port_t *port, ep_sock_t *sock)
{
    assert(sock->oneshot_next == NULL);
    assert(sock->oneshot_prev == NULL);

    sock->oneshot_prev = port->oneshot_tail;
    if (port->oneshot_tail != NULL) {
        port->oneshot_tail->oneshot_next = sock;
    } else {
        port->oneshot_head = sock;
    }
    port->oneshot_tail = sock;
}

static void ep_oneshot_list_remove_locked(ep_port_t *port, ep_sock_t *sock)
{
    if (sock->oneshot_prev != NULL) {
        sock->oneshot_prev->oneshot_next = sock->oneshot_next;
    } else {
        assert(port->oneshot_head == sock);
        port->oneshot_head = sock->oneshot_next;
    }
    if (sock->oneshot_next != NULL) {
        sock->oneshot_next->oneshot_prev = sock->oneshot_prev;
    } else {
        assert(port->oneshot_tail == sock);
        port->oneshot_tail = sock->oneshot_prev;
    }
    sock->oneshot_next = NULL;
    sock->oneshot_prev = NULL;
}

static void ep_oneshot_list_rotate_locked(ep_port_t *port, ep_sock_t *sock)
{
    assert(port->oneshot_head == sock);
    if (port->oneshot_tail == sock) {
        return;
    }

    port->oneshot_head = sock->oneshot_next;
    port->oneshot_head->oneshot_prev = NULL;
    sock->oneshot_next = NULL;
    sock->oneshot_prev = port->oneshot_tail;
    port->oneshot_tail->oneshot_next = sock;
    port->oneshot_tail = sock;
}

/* Keep re-arm state, exactly-once worklist membership, and the diagnostic
 * count in lockstep.  All callers hold fd_table_lock. */
static void ep_sock_set_needs_rearm_locked(ep_sock_t *sock, int needs_rearm)
{
    ep_port_t *port = sock->port;
    uint8_t requested = needs_rearm != 0;

    if (sock->needs_rearm == requested) {
        return;
    }
    if (requested) {
        sock->needs_rearm = 1;
        ep_rearm_list_append_locked(port, sock);
        port->needs_rearm_count++;
    } else {
        assert(port->needs_rearm_count > 0);
        ep_rearm_list_remove_locked(port, sock);
        sock->needs_rearm = 0;
        port->needs_rearm_count--;
    }
}

/* Fired oneshot registrations are not re-armed, but the wait path still
 * probes their provider handles so a native closesocket() retires them.  The
 * intrusive list limits those probes to registrations that actually fired. */
static void ep_sock_set_oneshot_fired_locked(ep_sock_t *sock, int fired)
{
    ep_port_t *port = sock->port;
    uint8_t requested = fired != 0;

    if (sock->oneshot_fired == requested) {
        return;
    }
    if (requested) {
        sock->oneshot_fired = 1;
        ep_oneshot_list_append_locked(port, sock);
        port->oneshot_fired_count++;
    } else {
        assert(port->oneshot_fired_count > 0);
        ep_oneshot_list_remove_locked(port, sock);
        sock->oneshot_fired = 0;
        port->oneshot_fired_count--;
    }
}

int ep_port_worklists_valid_locked(const ep_port_t *port)
{
    const ep_sock_t *slow;
    const ep_sock_t *fast;
    const ep_sock_t *previous;
    size_t live_rearm_count = 0;
    size_t live_oneshot_count = 0;
    size_t list_count = 0;

    if ((port->rearm_head == NULL) != (port->rearm_tail == NULL) ||
        (port->oneshot_head == NULL) != (port->oneshot_tail == NULL)) {
        return 0;
    }

    slow = port->sock_list_head;
    fast = port->sock_list_head;
    while (fast != NULL && fast->next != NULL) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            return 0;
        }
    }

    previous = NULL;
    for (const ep_sock_t *sock = port->sock_list_head;
         sock != NULL;
         sock = sock->next) {
        int rearm_linked = sock->rearm_prev != NULL ||
            sock->rearm_next != NULL || port->rearm_head == sock;
        int oneshot_linked = sock->oneshot_prev != NULL ||
            sock->oneshot_next != NULL || port->oneshot_head == sock;

        if (sock->prev != previous || sock->port != port ||
            (sock->needs_rearm != 0) != rearm_linked ||
            (sock->oneshot_fired != 0) != oneshot_linked ||
            (sock->needs_rearm && sock->oneshot_fired) ||
            (atomic_load_explicit(&sock->delete_pending,
                                  memory_order_relaxed) &&
             (sock->needs_rearm || sock->oneshot_fired))) {
            return 0;
        }
        if (sock->needs_rearm) {
            live_rearm_count++;
        }
        if (sock->oneshot_fired) {
            live_oneshot_count++;
        }
        previous = sock;
    }

    previous = NULL;
    list_count = 0;
    for (const ep_sock_t *sock = port->rearm_head;
         sock != NULL;
         sock = sock->rearm_next) {
        int found_live = 0;

        for (const ep_sock_t *live = port->sock_list_head;
             live != NULL;
             live = live->next) {
            if (live == sock) {
                found_live = 1;
                break;
            }
        }
        if (!sock->needs_rearm || sock->port != port ||
            !found_live ||
            sock->rearm_prev != previous ||
            ++list_count > port->needs_rearm_count) {
            return 0;
        }
        previous = sock;
    }
    if (previous != port->rearm_tail ||
        list_count != port->needs_rearm_count ||
        list_count != live_rearm_count) {
        return 0;
    }

    previous = NULL;
    list_count = 0;
    for (const ep_sock_t *sock = port->oneshot_head;
         sock != NULL;
         sock = sock->oneshot_next) {
        int found_live = 0;

        for (const ep_sock_t *live = port->sock_list_head;
             live != NULL;
             live = live->next) {
            if (live == sock) {
                found_live = 1;
                break;
            }
        }
        if (!sock->oneshot_fired || sock->port != port ||
            !found_live ||
            sock->oneshot_prev != previous ||
            ++list_count > port->oneshot_fired_count) {
            return 0;
        }
        previous = sock;
    }
    return previous == port->oneshot_tail &&
        list_count == port->oneshot_fired_count &&
        list_count == live_oneshot_count;
}

#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
typedef enum ep_identity_check {
    EP_IDENTITY_ERROR = -1,
    EP_IDENTITY_MATCH = 0,
    EP_IDENTITY_STALE,
    EP_IDENTITY_CLOSED
} ep_identity_check_t;

static int ep_socket_identity_is_stable(SOCKET fd)
{
    int socket_type = 0;
    int option_length = (int)sizeof(socket_type);

    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, (char *)&socket_type,
                   &option_length) == SOCKET_ERROR) {
        return 0;
    }
    if (socket_type != SOCK_STREAM) {
        return 1;
    }

    {
        int accepting = 0;
        option_length = (int)sizeof(accepting);
        if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, (char *)&accepting,
                       &option_length) == 0 && accepting != 0) {
            return 1;
        }
    }

    {
        struct sockaddr_storage peer;
        int peer_length = (int)sizeof(peer);
        return getpeername(fd, (struct sockaddr *)&peer, &peer_length) == 0;
    }
}

static ep_identity_check_t ep_sock_validate_identity_locked(
    ep_sock_t *sock, int allow_transition)
{
    uint64_t endpoint_id;
    int identity_result;

    if (sock->endpoint_id_state == EP_SOCKET_ID_UNAVAILABLE) {
        return EP_IDENTITY_MATCH;
    }

    identity_result = ep_socket_get_endpoint_id(sock->fd, &endpoint_id);
    if (identity_result == 0) {
        /* A provider that never exposed an ALE token remains supported with
         * legacy numeric-handle semantics.  Losing a token that was already
         * observed is not a safe identity match. */
        ep_set_errno(EIO);
        sock->port->identity_failures++;
        return EP_IDENTITY_ERROR;
    }
    if (identity_result < 0) {
        int error = ep_last_err();
        if (error == 0) {
            error = EIO;
            ep_set_errno(error);
        }
        if (error == ENOTSOCK || error == EBADF) {
            return EP_IDENTITY_CLOSED;
        }
        sock->port->identity_failures++;
        return EP_IDENTITY_ERROR;
    }

    if (endpoint_id == sock->endpoint_id) {
        if (sock->endpoint_id_state == EP_SOCKET_ID_TRANSITIONAL &&
            ep_socket_identity_is_stable(sock->fd)) {
            sock->endpoint_id_state = EP_SOCKET_ID_STABLE;
        }
        return EP_IDENTITY_MATCH;
    }

    if (sock->endpoint_id_state == EP_SOCKET_ID_TRANSITIONAL &&
        allow_transition) {
        /* An AFD completion submitted before connect is evidence that this
         * registration survived the legitimate ALE endpoint transition.
         * Control-path mismatches have no such evidence and are treated as
         * native numeric-handle reuse instead. */
        sock->endpoint_id = endpoint_id;
        if (ep_socket_identity_is_stable(sock->fd)) {
            sock->endpoint_id_state = EP_SOCKET_ID_STABLE;
        }
        return EP_IDENTITY_MATCH;
    }

    return EP_IDENTITY_STALE;
}
#endif

static ep_sock_t *ep_sock_alloc_locked(ep_port_t *port, SOCKET fd)
{
    ep_sock_t *sock = (ep_sock_t *)calloc(1, sizeof(*sock));
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    int identity_result;
#endif
    int base_error = 0;
    if (sock == NULL) {
        ep_set_errno(ENOMEM);
        return NULL;
    }

    sock->kind = EP_REG_SOCKET;
    sock->wait_registration = NULL;
    sock->exclusive_claim_base = INVALID_SOCKET;
    sock->base_socket = ep_socket_get_base(fd);
    if (sock->base_socket == INVALID_SOCKET) {
        DWORD file_type;
        int waitability;
        int handle_candidate;
        uint8_t waitable_semantics = EP_WAITABLE_ET_UNSUPPORTED;

        base_error = ep_last_err();
        file_type = GetFileType((HANDLE)fd);
        handle_candidate = base_error == ENOTSOCK || base_error == EBADF ||
            base_error == 0;
        waitability = file_type == FILE_TYPE_PIPE ||
            file_type == FILE_TYPE_DISK ? 0 :
            ep_handle_waitability((HANDLE)fd, file_type,
                                  &waitable_semantics);
        if (handle_candidate && file_type == FILE_TYPE_PIPE) {
            if (ep_pipe_query_access((HANDLE)fd, &sock->pipe_access) != 0) {
                free(sock);
                return NULL;
            }
            sock->kind = EP_REG_PIPE;
            sock->base_socket = fd;
            sock->afd_info = NULL;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
            sock->endpoint_id = 0;
            sock->endpoint_id_state = EP_SOCKET_ID_UNAVAILABLE;
#endif
        } else if (file_type == FILE_TYPE_DISK) {
            free(sock);
            ep_set_errno(EPERM);
            return NULL;
        } else if (handle_candidate && waitability == -2) {
            free(sock);
            ep_set_errno(EACCES);
            return NULL;
        } else if (handle_candidate && waitability > 0) {
            sock->kind = EP_REG_WAITABLE;
            sock->waitable_semantics = waitable_semantics;
            sock->base_socket = fd;
            sock->afd_info = NULL;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
            sock->endpoint_id = 0;
            sock->endpoint_id_state = EP_SOCKET_ID_UNAVAILABLE;
#endif
        } else {
            free(sock);
            if (handle_candidate && waitability < 0) {
                ep_set_errno(EPERM);
            } else if (base_error != 0) {
                ep_set_errno(base_error);
            } else {
                ep_set_errno(ENOTSOCK);
            }
            return NULL;
        }
    } else {
        sock->socket_protocol = ep_socket_get_protocol(fd);
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
        identity_result = ep_socket_get_endpoint_id(fd, &sock->endpoint_id);
        if (identity_result < 0) {
            port->identity_failures++;
            free(sock);
            return NULL;
        }
        if (identity_result == 0) {
#ifdef WEPOLL_EX_STRICT_SOCKET_IDENTITY
            port->identity_failures++;
            free(sock);
            ep_set_errno(EOPNOTSUPP);
            return NULL;
#else
            sock->endpoint_id_state = EP_SOCKET_ID_UNAVAILABLE;
#endif
        } else if (ep_socket_identity_is_stable(fd)) {
            sock->endpoint_id_state = EP_SOCKET_ID_STABLE;
        } else {
            sock->endpoint_id_state = EP_SOCKET_ID_TRANSITIONAL;
        }
#endif
        sock->afd_info = (AFD_POLL_INFO *)ep_afd_pool_take(&port->afd_info_pool);
        if (sock->afd_info == NULL) {
            free(sock);
            return NULL;
        }
    }

    sock->fd = fd;
    sock->port = port;
    sock->generation = ++port->next_sock_generation;
    if (port->next_sock_generation == 0) {
        port->next_sock_generation = 1;
        sock->generation = 1;
    }
    atomic_init(&sock->state, EP_SOCK_REGISTERED);
    atomic_init(&sock->poll_status, EP_POLL_IDLE);
    atomic_init(&sock->delete_pending, 0);
    atomic_init(&sock->ready_queued, 0);
    atomic_init(&sock->callback_active, 0);
    atomic_init(&sock->completion_posted, 0);
    atomic_init(&sock->waitable_notification_owned, 0);
    return sock;
}

static void ep_sock_free_locked(ep_port_t *port, ep_sock_t *sock)
{
    ep_sock_set_needs_rearm_locked(sock, 0);
    ep_sock_set_oneshot_fired_locked(sock, 0);
    if ((sock->user_flags & EPOLLEXCLUSIVE) != 0) {
        ep_exclusive_release_owner(sock);
    }
    sock->observed_events = 0;
    sock->pipe_terminal_delivered = 0;
    sock->et_holdoff = 0;
    if (sock->kind == EP_REG_WAITABLE || sock->kind == EP_REG_PIPE) {
        assert(sock->wait_registration == NULL);
        assert(atomic_load_explicit(&sock->callback_active,
                                    memory_order_relaxed) == 0);
        assert(atomic_load_explicit(&sock->completion_posted,
                                    memory_order_relaxed) == 0);
    }
    ep_sock_list_remove_locked(port, sock);
    if (sock->afd_info != NULL) {
        ep_afd_pool_give(&port->afd_info_pool, sock->afd_info);
        sock->afd_info = NULL;
    }
    free(sock);
}

/* A registered socket may be closed by the application without an explicit
 * EPOLL_CTL_DEL.  Once AFD reports that the provider handle is gone, remove
 * the public registration but keep any in-flight request alive until its
 * completion has been consumed. */
static void ep_sock_drop_closed_locked(ep_port_t *port, ep_sock_t *sock)
{
    if (ep_fd_table_lookup(port, sock->fd) == sock) {
        ep_fd_table_remove(port, sock);
    }
    if ((sock->user_flags & EPOLLEXCLUSIVE) != 0) {
        ep_exclusive_release_owner(sock);
    }
    atomic_store_explicit(&sock->delete_pending, 1, memory_order_relaxed);
    atomic_store_explicit(&sock->state, EP_SOCK_DELETED,
                          memory_order_relaxed);
    ep_sock_set_needs_rearm_locked(sock, 0);
    ep_sock_set_oneshot_fired_locked(sock, 0);
    sock->generation = ++port->next_sock_generation;
    if (port->next_sock_generation == 0) {
        port->next_sock_generation = 1;
        sock->generation = 1;
    }
    if (atomic_load_explicit(&sock->poll_status, memory_order_relaxed) ==
        EP_POLL_IDLE) {
        ep_sock_free_locked(port, sock);
    }
}

static uint32_t ep_sock_afd_events_locked(ep_sock_t *sock,
                                          uint32_t user_events)
{
    uint32_t afd_events = ep_epoll_to_afd_events(user_events);

#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    if (sock->endpoint_id_state == EP_SOCKET_ID_TRANSITIONAL) {
        /* Keep the pre-connect request alive across every supported MOD.
         * Its eventual completion is the evidence that permits adopting the
         * legitimate endpoint-token transition performed by connect(). */
        afd_events = AFD_POLL_ALL_EVENTS;
    }
#else
    (void)sock;
#endif
    return afd_events;
}

#ifdef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
#define EP_SOCK_SUBMIT_LOCKED(sock, identity_validated) \
    ep_sock_submit_locked(sock)
static int ep_sock_cancel_locked(ep_sock_t *sock);
static int ep_sock_submit_locked(ep_sock_t *sock)
#else
#define EP_SOCK_SUBMIT_LOCKED(sock, identity_validated) \
    ep_sock_submit_locked((sock), (identity_validated))
static int ep_sock_cancel_locked(ep_sock_t *sock);
static int ep_sock_submit_locked(ep_sock_t *sock, int identity_validated)
#endif
{
    ep_port_t *port = sock->port;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    ep_identity_check_t identity_check;
#endif
    uint32_t poll_status =
        atomic_load_explicit(&sock->poll_status, memory_order_relaxed);

    if (atomic_load_explicit(&port->closing, memory_order_acquire) ||
        atomic_load_explicit(&sock->delete_pending, memory_order_relaxed) ||
        atomic_load_explicit(&sock->ready_queued, memory_order_relaxed)) {
        return 0;
    }
    if (ep_waitable_is_dormant(sock)) {
        if (poll_status == EP_POLL_PENDING &&
            ep_sock_cancel_locked(sock) != 0) {
            return -1;
        }
        ep_sock_set_needs_rearm_locked(sock, 0);
        sock->et_holdoff = 0;
        if (atomic_load_explicit(&sock->poll_status,
                                 memory_order_relaxed) == EP_POLL_IDLE) {
            atomic_store_explicit(&sock->state, EP_SOCK_REGISTERED,
                                  memory_order_relaxed);
        }
        return 0;
    }
    if ((sock->kind == EP_REG_WAITABLE || sock->kind == EP_REG_PIPE) &&
        poll_status == EP_POLL_PENDING && sock->needs_rearm) {
        int error;

        if (ep_sock_cancel_locked(sock) != 0) {
            error = ep_last_err();
            /* A repeated retirement failure makes this registration
             * unusable. Detach it publicly but preserve storage and pending
             * accounting for DEL/close-time recovery or quarantine. */
            ep_sock_drop_closed_locked(port, sock);
            ep_set_errno(error);
            return -1;
        }
        poll_status = atomic_load_explicit(&sock->poll_status,
                                           memory_order_relaxed);
        if (poll_status != EP_POLL_IDLE) {
            /* A readiness packet was already queued.  Its completion owns
             * the accounting transition and ordinary rearm. */
            return 0;
        }
        /* begin_close publishes before waiting for fd_table_lock.  It may
         * have raced the blocking auxiliary disarm above, so do not arm a new
         * callback after close has started. */
        if (atomic_load_explicit(&port->closing, memory_order_acquire)) {
            return 0;
        }
    }
    if (poll_status != EP_POLL_IDLE) {
        return 0;
    }
    if (sock->et_holdoff) {
        return 0;
    }

#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    if (!identity_validated) {
        identity_check = ep_sock_validate_identity_locked(sock, 0);
        if (identity_check == EP_IDENTITY_ERROR) {
            return -1;
        }
        if (identity_check == EP_IDENTITY_STALE ||
            identity_check == EP_IDENTITY_CLOSED) {
            int error = identity_check == EP_IDENTITY_STALE
                ? ENOENT : ENOTSOCK;
            ep_sock_drop_closed_locked(port, sock);
            ep_set_errno(error);
            return 1;
        }
    }
#endif

    if (sock->kind == EP_REG_WAITABLE) {
        DWORD wait_result;
        int may_consume;

        if (sock->oneshot_fired) {
            /* Do not probe while oneshot is disabled: a zero-time wait would
             * consume an auto-reset event or semaphore count before rearm. */
            return 0;
        }
        if (!sock->needs_rearm) {
            return 0;
        }
        sock->et_holdoff = 0;
        if (atomic_load_explicit(&sock->waitable_notification_owned,
                                 memory_order_acquire) != 0) {
            /* A prior callback/probe consumed this notification, but no live
             * ready node represents it.  Replay ownership before touching the
             * HANDLE again so a later signal/count cannot overtake it. */
            if (!ep_aux_post_completion(sock, STATUS_SUCCESS)) {
                ep_set_errno(ep_winerr_to_errno(GetLastError()));
                return -1;
            }
            ep_sock_set_needs_rearm_locked(sock, 0);
            atomic_store_explicit(&sock->poll_status, EP_POLL_PENDING,
                                  memory_order_relaxed);
            atomic_store_explicit(&sock->state, EP_SOCK_POLLING,
                                  memory_order_relaxed);
            port->pending_poll_count++;
            return 0;
        }
        may_consume = ep_waitable_may_consume(sock);
        if (may_consume) {
            /* Claim ownership before the zero-time wait can consume an
             * auto-reset signal, semaphore count, or mode-unknown object. */
            atomic_store_explicit(&sock->waitable_notification_owned, 1,
                                  memory_order_release);
        }
        wait_result = WaitForSingleObject((HANDLE)sock->fd, 0);
        if (wait_result == WAIT_FAILED) {
            DWORD error = GetLastError();

            if (may_consume) {
                atomic_store_explicit(&sock->waitable_notification_owned, 0,
                                      memory_order_release);
            }
            if (error == ERROR_INVALID_HANDLE) {
                ep_sock_drop_closed_locked(port, sock);
                ep_set_errno(EBADF);
                return 1;
            }
            ep_set_errno(ep_winerr_to_errno(error));
            return -1;
        }
        if (wait_result != WAIT_OBJECT_0 && may_consume) {
            atomic_store_explicit(&sock->waitable_notification_owned, 0,
                                  memory_order_release);
        }
        if (wait_result != WAIT_OBJECT_0 &&
            (sock->user_flags & EPOLLET) != 0) {
            /* Handle is unsignaled: reopen the edge latch. */
            sock->observed_events = 0;
        }
        if (wait_result == WAIT_OBJECT_0) {
            if (!ep_aux_post_completion(sock, STATUS_SUCCESS)) {
                ep_set_errno(ep_winerr_to_errno(GetLastError()));
                return -1;
            }
        } else if (ep_waitable_register_locked(sock) != 0) {
            return -1;
        }
        ep_sock_set_needs_rearm_locked(sock, 0);
        atomic_store_explicit(&sock->poll_status, EP_POLL_PENDING,
                              memory_order_relaxed);
        atomic_store_explicit(&sock->state, EP_SOCK_POLLING,
                              memory_order_relaxed);
        port->pending_poll_count++;
        return 0;
    }

    if (sock->kind == EP_REG_PIPE) {
        ep_pipe_snapshot_t snapshot = ep_pipe_snapshot(sock);

        if (snapshot.local_closed) {
            ep_sock_drop_closed_locked(port, sock);
            ep_set_errno(EBADF);
            return 1;
        }
        if (sock->oneshot_fired) {
            /* Peer shutdown is persistent pipe state, not local handle
             * invalidation.  Keep a fired one-shot registration available
             * for MOD/epoll_rearm to sample that terminal state again. */
            return 0;
        }
        if (!sock->needs_rearm) {
            return 0;
        }
        sock->et_holdoff = 0;
        if (snapshot.valid && snapshot.events == 0 &&
            (sock->user_flags & EPOLLET) != 0) {
            sock->observed_events = 0;
            sock->pipe_terminal_delivered = 0;
        }
        if (ep_pipe_schedule_locked(sock, snapshot.events) != 0) {
            return -1;
        }
        ep_sock_set_needs_rearm_locked(sock, 0);
        atomic_store_explicit(&sock->poll_status, EP_POLL_PENDING,
                              memory_order_relaxed);
        atomic_store_explicit(&sock->state, EP_SOCK_POLLING,
                              memory_order_relaxed);
        port->pending_poll_count++;
        return 0;
    }

    if (sock->oneshot_fired) {
        /* No AFD request remains after a oneshot delivery.  Probe the
         * provider handle so a native closesocket() cannot leave a stale
         * registration forever. */
        SOCKET base = ep_socket_get_base(sock->fd);
        if (base == INVALID_SOCKET) {
            if (ep_last_err() == ENOTSOCK || ep_last_err() == EBADF) {
                ep_sock_drop_closed_locked(port, sock);
                ep_set_errno(ENOTSOCK);
                return 1;
            }
            return -1;
        }
        sock->base_socket = base;
        return 0;
    }
    if (!sock->needs_rearm) {
        return 0;
    }

    /* A deferred edge-triggered re-arm is eligible again once a wait is
     * preparing submissions. */
    sock->et_holdoff = 0;
    uint32_t afd_events = ep_sock_afd_events_locked(sock, sock->user_events);
    int poll_pending = 0;
    if (ep_afd_poll_submit(sock, afd_events, &poll_pending) != 0) {
        if (ep_last_err() == ENOTSOCK || ep_last_err() == EBADF) {
            ep_sock_drop_closed_locked(port, sock);
            ep_set_errno(ENOTSOCK);
            return 1;
        }
        return -1;
    }
    if (poll_pending) {
        ep_exclusive_release_quiescent(sock, afd_events);
    }

    ep_sock_set_needs_rearm_locked(sock, 0);
    atomic_store_explicit(&sock->poll_status, EP_POLL_PENDING,
                          memory_order_relaxed);
    atomic_store_explicit(&sock->state, EP_SOCK_POLLING,
                          memory_order_relaxed);
    port->pending_poll_count++;
    return 0;
}

static int ep_sock_complete_pending_locked(ep_sock_t *sock)
{
    ep_port_t *port = sock->port;
    uint32_t poll_status =
        atomic_load_explicit(&sock->poll_status, memory_order_relaxed);

    if ((poll_status != EP_POLL_PENDING &&
         poll_status != EP_POLL_CANCELLED) ||
        port->pending_poll_count == 0) {
        assert(!"pending poll accounting invariant violated");
        atomic_store_explicit(&sock->poll_status, EP_POLL_IDLE,
                              memory_order_relaxed);
        ep_port_record_async_error_locked(port, EIO);
        ep_set_errno(EIO);
        return -1;
    }
    atomic_store_explicit(&sock->poll_status, EP_POLL_IDLE,
                          memory_order_relaxed);
    port->pending_poll_count--;
    return 0;
}

static int ep_sock_cancel_locked(ep_sock_t *sock)
{
    if (atomic_load_explicit(&sock->poll_status, memory_order_relaxed) !=
        EP_POLL_PENDING) {
        return 0;
    }
    if (sock->kind == EP_REG_WAITABLE || sock->kind == EP_REG_PIPE) {
        int disarm_result = sock->kind == EP_REG_WAITABLE
            ? ep_waitable_unregister_locked(sock)
            : ep_pipe_delete_timer_locked(sock);

        if (disarm_result != 0) {
            return -1;
        }
        sock->pending_events = 0;
        if (atomic_load_explicit(&sock->completion_posted,
                                 memory_order_acquire) != 0) {
            /* The queued packet still owns the embedded status block.  Mark
             * it cancelled so completion consumes the pending count without
             * surfacing stale readiness. */
            atomic_store_explicit(&sock->poll_status, EP_POLL_CANCELLED,
                                  memory_order_relaxed);
        } else {
            /* Blocking disarm has joined any callback, and no successful
             * post exists.  No external actor can now reference this
             * registration, so retire it without manufacturing an IOCP
             * packet that a future wait or close would have to consume. */
            assert(sock->wait_registration == NULL);
            assert(atomic_load_explicit(&sock->callback_active,
                                        memory_order_relaxed) == 0);
            if (ep_sock_complete_pending_locked(sock) != 0) {
                return -1;
            }
            if (!atomic_load_explicit(&sock->delete_pending,
                                      memory_order_relaxed) &&
                !atomic_load_explicit(&sock->port->closing,
                                      memory_order_acquire)) {
                atomic_store_explicit(&sock->state, EP_SOCK_REGISTERED,
                                      memory_order_relaxed);
            }
        }
        return 0;
    }
    if (ep_afd_cancel(sock) != 0) {
        return -1;
    }

    sock->pending_events = 0;
    atomic_store_explicit(&sock->poll_status, EP_POLL_CANCELLED,
                          memory_order_relaxed);
    return 0;
}

#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
static void ep_sock_retire_stale_locked(ep_port_t *port, ep_sock_t *sock)
{
    int saved_errno = ep_last_err();

    if (atomic_load_explicit(&sock->poll_status, memory_order_relaxed) ==
        EP_POLL_PENDING) {
        (void)ep_sock_cancel_locked(sock);
    }
    atomic_fetch_add_explicit(&port->generation, 1, memory_order_relaxed);
    port->stale_events_dropped++;
    ep_sock_drop_closed_locked(port, sock);
    ep_set_errno(saved_errno);
}
#endif

/* Return 0 for the registered socket, 1 after retiring a stale/closed
 * numeric-handle entry, and -1 for an identity-query failure. */
static int ep_sock_validate_control_locked(ep_port_t *port, ep_sock_t *sock)
{
    if (sock->kind == EP_REG_WAITABLE || sock->kind == EP_REG_PIPE) {
        (void)port;
        return 0;
    }
#ifdef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    (void)port;
    (void)sock;
    return 0;
#else
    ep_identity_check_t identity_check =
        ep_sock_validate_identity_locked(sock, 0);

    if (identity_check == EP_IDENTITY_MATCH) {
        return 0;
    }
    if (identity_check == EP_IDENTITY_ERROR) {
        return -1;
    }

    {
        int error = identity_check == EP_IDENTITY_STALE
            ? ENOENT : ENOTSOCK;
        ep_sock_retire_stale_locked(port, sock);
        ep_set_errno(error);
    }
    return 1;
#endif
}

static int ep_port_arm_pending_locked(ep_port_t *port)
{
    size_t rearm_budget = port->needs_rearm_count;
    size_t oneshot_budget = port->oneshot_fired_count;
    int first_error = 0;

    /* Snapshot the starting queue lengths.  A deferred item that cannot be
     * submitted yet (for example while an AFD cancellation is pending) is
     * rotated to the tail and retried by a later wait iteration, not spun on
     * indefinitely here.  Successful submission removes it in O(1). */
    while (rearm_budget-- > 0) {
        ep_sock_t *sock = port->rearm_head;
        int submit_result;

        assert(sock != NULL);
        port->rearm_work_visits++;
        submit_result = EP_SOCK_SUBMIT_LOCKED(sock, 0);
        if (submit_result < 0) {
            int error = ep_last_err();

            if (first_error == 0) first_error = error == 0 ? EIO : error;
            ep_port_record_async_error_locked(port, error);
            if (sock->needs_rearm) {
                ep_rearm_list_rotate_locked(port, sock);
            }
            continue;
        }
        if (submit_result == 0 && sock->needs_rearm) {
            ep_rearm_list_rotate_locked(port, sock);
        }
    }

    /* Fired oneshots have no AFD request to re-arm.  Keep each one queued so
     * subsequent waits continue to detect native closesocket(), but probe it
     * only once per arm pass and only among fired oneshots. */
    while (oneshot_budget-- > 0) {
        ep_sock_t *sock = port->oneshot_head;
        int submit_result;

        assert(sock != NULL);
        port->oneshot_probe_visits++;
        submit_result = EP_SOCK_SUBMIT_LOCKED(sock, 0);
        if (submit_result < 0) {
            int error = ep_last_err();

            if (first_error == 0) first_error = error == 0 ? EIO : error;
            ep_port_record_async_error_locked(port, error);
            if (sock->oneshot_fired) {
                ep_oneshot_list_rotate_locked(port, sock);
            }
            continue;
        }
        if (submit_result == 0 && sock->oneshot_fired) {
            ep_oneshot_list_rotate_locked(port, sock);
        }
    }
    if (first_error != 0) {
        ep_set_errno(first_error);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* IOCP completion handling.                                                */
/* ------------------------------------------------------------------------- */

void ep_sock_handle_completion(ep_sock_t *sock, DWORD bytes, NTSTATUS status)
{
    ep_port_t *port = sock->port;
    ep_ready_node_t *node = NULL;
    ep_pipe_snapshot_t pipe_snapshot = {0};
    uint32_t delivered = 0;
    uint32_t old_observed_events = 0;
    uint32_t old_poll_status;

    (void)bytes;
    pthread_mutex_lock(&port->fd_table_lock);

    old_poll_status =
        atomic_load_explicit(&sock->poll_status, memory_order_relaxed);
    if (old_poll_status != EP_POLL_PENDING &&
        old_poll_status != EP_POLL_CANCELLED) {
        pthread_mutex_unlock(&port->fd_table_lock);
        return;
    }
    if (old_poll_status == EP_POLL_PENDING && status >= 0 &&
        ep_waitable_may_consume(sock)) {
        /* Callback and immediate-probe paths publish this before posting.  Set
         * it defensively here as well so every successful consumptive packet
         * retains ownership until a ready node accepts the notification. */
        atomic_store_explicit(&sock->waitable_notification_owned, 1,
                              memory_order_release);
    }
    if (sock->kind == EP_REG_WAITABLE || sock->kind == EP_REG_PIPE) {
        int disarm_result = sock->kind == EP_REG_WAITABLE
            ? ep_waitable_unregister_locked(sock)
            : ep_pipe_delete_timer_locked(sock);

        if (disarm_result != 0) {
            int error = ep_last_err();

            /* The dequeued packet is consumed, but the logical pending slot
             * stays pinned until retirement succeeds.  Preserve readiness
             * that an auto-reset event, semaphore, or mode-unknown wait may
             * already have consumed before this cleanup failure. */
            atomic_store_explicit(&sock->completion_posted, 0,
                                  memory_order_release);
            ep_port_record_async_error_locked(port, error);
            if (atomic_load_explicit(&sock->delete_pending,
                                     memory_order_relaxed) ||
                atomic_load_explicit(&port->closing,
                                     memory_order_acquire)) {
                int cancel_result = ep_sock_cancel_locked(sock);

                if (cancel_result == 0 &&
                    atomic_load_explicit(&sock->poll_status,
                                         memory_order_relaxed) ==
                        EP_POLL_IDLE &&
                    atomic_load_explicit(&sock->delete_pending,
                                         memory_order_relaxed)) {
                    ep_sock_free_locked(port, sock);
                    pthread_mutex_unlock(&port->fd_table_lock);
                    return;
                }
            } else {
                ep_sock_set_needs_rearm_locked(sock, 1);
            }
            pthread_mutex_unlock(&port->fd_table_lock);
            return;
        }
        atomic_store_explicit(&sock->completion_posted, 0,
                              memory_order_release);
    }
    if (ep_sock_complete_pending_locked(sock) != 0) {
        if (atomic_load_explicit(&sock->delete_pending,
                                 memory_order_relaxed)) {
            ep_sock_free_locked(port, sock);
        }
        pthread_mutex_unlock(&port->fd_table_lock);
        return;
    }

    if (atomic_load_explicit(&sock->delete_pending, memory_order_relaxed)) {
        ep_sock_free_locked(port, sock);
        pthread_mutex_unlock(&port->fd_table_lock);
        return;
    }
    if (atomic_load_explicit(&port->closing, memory_order_acquire)) {
        atomic_store_explicit(&sock->state, EP_SOCK_REGISTERED,
                              memory_order_relaxed);
        pthread_mutex_unlock(&port->fd_table_lock);
        return;
    }

    if (old_poll_status == EP_POLL_CANCELLED || status == STATUS_CANCELLED) {
        if (EP_SOCK_SUBMIT_LOCKED(sock, 0) < 0) {
            int error = ep_last_err();
            ep_sock_set_needs_rearm_locked(sock, 1);
            ep_port_record_async_error_locked(port, error);
        }
        pthread_mutex_unlock(&port->fd_table_lock);
        return;
    }

    if (ep_waitable_is_dormant(sock)) {
        /* MOD-to-zero may have linearized after the wait consumed a
         * notification.  Logical dormancy starts immediately even though its
         * already-posted packet still had to retire the pending accounting. */
        ep_sock_set_needs_rearm_locked(sock, 0);
        sock->et_holdoff = 0;
        atomic_store_explicit(&sock->state, EP_SOCK_REGISTERED,
                              memory_order_relaxed);
        pthread_mutex_unlock(&port->fd_table_lock);
        return;
    }

    if (status >= 0 && sock->afd_info != NULL &&
        sock->afd_info->NumberOfHandles > 0) {
        if ((sock->afd_info->Handles[0].Events & AFD_POLL_LOCAL_CLOSE) != 0) {
            /* The application closed this socket without EPOLL_CTL_DEL.
             * There is no usable registration left to report; retire it now
             * rather than trying to re-arm a stale numeric SOCKET later. */
            ep_sock_drop_closed_locked(port, sock);
            pthread_mutex_unlock(&port->fd_table_lock);
            return;
        }
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
        if (sock->endpoint_id_state == EP_SOCKET_ID_TRANSITIONAL) {
            ep_identity_check_t identity_check =
                ep_sock_validate_identity_locked(sock, 1);
            if (identity_check == EP_IDENTITY_STALE ||
                identity_check == EP_IDENTITY_CLOSED) {
                ep_sock_drop_closed_locked(port, sock);
                pthread_mutex_unlock(&port->fd_table_lock);
                return;
            }
#ifdef WEPOLL_EX_STRICT_SOCKET_IDENTITY
            if (identity_check == EP_IDENTITY_ERROR) {
                int error = ep_last_err();

                atomic_store_explicit(&sock->state, EP_SOCK_REGISTERED,
                                      memory_order_relaxed);
                ep_sock_set_needs_rearm_locked(sock, 1);
                ep_port_record_async_error_locked(port, error);
                pthread_mutex_unlock(&port->fd_table_lock);
                return;
            }
#endif
        }
#endif
        delivered = ep_afd_to_epoll_events(
            sock->afd_info->Handles[0].Events,
            sock->afd_info->Handles[0].Status,
            sock->socket_protocol);
    } else if (sock->kind == EP_REG_WAITABLE) {
        /* Consumptive waits use the callback/immediate wait itself as the
         * readiness observation.  Persistent objects can be sampled safely;
         * doing so discards a stale queued callback after ResetEvent and
         * reopens the ET latch before a later signal. */
        if (status >= 0 &&
            (sock->waitable_semantics == EP_WAITABLE_PERSISTENT ||
             sock->waitable_semantics == EP_WAITABLE_TERMINAL)) {
            DWORD wait_result = WaitForSingleObject((HANDLE)sock->fd, 0);

            if (wait_result == WAIT_OBJECT_0) {
                delivered = ep_waitable_level_events(sock);
            } else if (wait_result == WAIT_FAILED) {
                delivered = EPOLLERR | EPOLLHUP;
            }
        } else if (status >= 0) {
            delivered = ep_waitable_level_events(sock);
        } else {
            delivered = EPOLLERR | EPOLLHUP;
        }
    } else if (sock->kind == EP_REG_PIPE) {
        pipe_snapshot = ep_pipe_snapshot(sock);

        if (pipe_snapshot.local_closed) {
            ep_sock_drop_closed_locked(port, sock);
            pthread_mutex_unlock(&port->fd_table_lock);
            return;
        }
        delivered = pipe_snapshot.events;
    } else if (status < 0) {
        delivered = EPOLLERR;
    }
    if (sock->kind == EP_REG_SOCKET &&
        (sock->user_flags & (EPOLLET | EPOLLEXCLUSIVE)) != 0) {
        int sample_all = (sock->user_flags & EPOLLEXCLUSIVE) != 0;
        int read_ready = -1;
        int write_ready = -1;
        uint8_t inactive_classes = 0;

        if (sample_all ||
            (delivered & (EPOLLIN | EPOLLRDNORM | EPOLLRDHUP)) != 0) {
            read_ready = ep_socket_select_ready(sock->fd, 0);
        }
        if (sample_all ||
            (delivered & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)) != 0) {
            write_ready = ep_socket_select_ready(sock->fd, 1);
        }
        if (read_ready == 0) {
            delivered &= ~(EPOLLIN | EPOLLRDNORM | EPOLLRDHUP);
            inactive_classes |= EP_EXCLUSIVE_CLASS_READ;
        }
        if (write_ready == 0) {
            delivered &= ~(EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND);
            inactive_classes |= EP_EXCLUSIVE_CLASS_WRITE;
        }
        if (sample_all && inactive_classes != 0) {
            ep_exclusive_release_inactive(sock, inactive_classes);
        }
    }
    delivered &= sock->user_events | EPOLLERR | EPOLLHUP;

    if ((sock->user_flags & EPOLLET) != 0) {
        uint32_t level = delivered;

        old_observed_events = sock->observed_events;
        if (sock->kind == EP_REG_PIPE) {
            delivered = ep_pipe_et_events(sock, &pipe_snapshot);
        } else {
            uint32_t interest = sock->user_events | EPOLLERR | EPOLLHUP;
            uint32_t edge;

            /* Bits no longer present in the latest level snapshot become
             * eligible for a future edge when they reappear. */
            sock->observed_events &= level & interest;
            edge = level & ~sock->observed_events;
            delivered = edge;
            if (edge != 0) {
                sock->observed_events |= edge;
            }
        }
    } else {
        sock->observed_events = 0;
        sock->et_holdoff = 0;
    }

    if (delivered == 0) {
        if (sock->kind == EP_REG_PIPE &&
            (sock->user_flags & EPOLLET) != 0 && pipe_snapshot.valid &&
            sock->pipe_terminal_delivered &&
            ep_pipe_et_snapshot_is_final(
                sock, pipe_snapshot.events, &pipe_snapshot)) {
            /* The final-shape event was already delivered, but its drain-time
             * metadata confirmation was unavailable.  A later valid unchanged
             * native client snapshot can now finish the final idle transition
             * without manufacturing a duplicate terminal event. */
            sock->et_holdoff = 0;
            ep_sock_set_needs_rearm_locked(sock, 0);
            atomic_store_explicit(&sock->state, EP_SOCK_REGISTERED,
                                  memory_order_relaxed);
            pthread_mutex_unlock(&port->fd_table_lock);
            return;
        }
        /* Level-triggered empty reports re-arm immediately.  Edge-triggered
         * empty reports mean the level is still true but already observed;
         * defer and throttle re-arming so permanently ready sockets do not
         * spin inside completion handling. */
        sock->et_holdoff = (sock->user_flags & EPOLLET) != 0;
        ep_sock_set_needs_rearm_locked(sock, 1);
        if ((sock->user_flags & EPOLLET) == 0) {
            if (EP_SOCK_SUBMIT_LOCKED(sock, 0) < 0) {
                int error = ep_last_err();
                ep_sock_set_needs_rearm_locked(sock, 1);
                ep_port_record_async_error_locked(port, error);
            }
        }
        pthread_mutex_unlock(&port->fd_table_lock);
        return;
    }

    node = ep_fault_hit(EP_FAULT_READY_NODE_ALLOC) == 0
        ? ep_ready_node_alloc(port) : NULL;
    if (node == NULL) {
        int error = ep_last_err();

        if ((sock->user_flags & EPOLLET) != 0) {
            /* No ready node owns this edge yet.  Restore the prior latch so a
             * successful retry can report the same level instead of losing it
             * after the transient allocation failure. */
            sock->observed_events = old_observed_events;
        }
        ep_sock_set_needs_rearm_locked(sock, 1);
        ep_port_record_async_error_locked(port, error == 0 ? ENOMEM : error);
        pthread_mutex_unlock(&port->fd_table_lock);
        return;
    }

    node->data = sock->user_data;
    node->user_ctx = sock->user_ctx;
    node->fd = sock->fd;
    node->sock_generation = sock->generation;
    node->events = delivered;
    node->flags = 0;
    if ((sock->user_flags & EPOLLET) != 0) {
        node->flags |= WEPOLL_FLAG_ET_DELIVERED | WEPOLL_FLAG_EDGE_ARMED;
    }
    if ((sock->user_flags & EPOLLONESHOT) != 0) {
        node->flags |= WEPOLL_FLAG_ONESHOT_FIRED;
        ep_sock_set_oneshot_fired_locked(sock, 1);
    }
    node->timestamp = ep_now_ns();

    if ((sock->user_flags & EPOLLEXCLUSIVE) != 0 &&
        !ep_exclusive_try_claim(sock, &delivered)) {
        /* Another exclusive registration already owns this wake. */
        ep_ready_node_free(port, node);
        sock->et_holdoff = 1;
        ep_sock_set_needs_rearm_locked(sock, 1);
        pthread_mutex_unlock(&port->fd_table_lock);
        return;
    }
    node->events = delivered;

    sock->pending_events = delivered;
    sock->et_holdoff = 0;
    atomic_store_explicit(&sock->ready_queued, 1, memory_order_relaxed);
    atomic_store_explicit(&sock->state, EP_SOCK_READY, memory_order_relaxed);
    ep_ready_push(&port->ready_queue, node);
    if (ep_waitable_may_consume(sock)) {
        /* The ready node now owns the consumed notification. */
        atomic_store_explicit(&sock->waitable_notification_owned, 0,
                              memory_order_release);
    }
    pthread_mutex_unlock(&port->fd_table_lock);
}

/* ------------------------------------------------------------------------- */
/* Port lifecycle.                                                          */
/* ------------------------------------------------------------------------- */

int ep_port_create(int size_hint, int flags, ep_port_t **out)
{
    ep_port_t *port;
    size_t pool_capacity = WEPOLL_AFD_POOL_SIZE;
    int fd_lock_initialized = 0;
    int wait_lock_initialized = 0;
    int iocp_post_lock_initialized = 0;
    int ready_initialized = 0;
    int afd_pool_initialized = 0;
    int node_pool_initialized = 0;

    if (out == NULL) {
        ep_set_errno(EFAULT);
        return -1;
    }
    *out = NULL;
    if (ep_global_init() != 0) {
        return -1;
    }

    port = (ep_port_t *)calloc(1, sizeof(*port));
    if (port == NULL) {
        ep_set_errno(ENOMEM);
        return -1;
    }

    int mutex_error = pthread_mutex_init(&port->fd_table_lock, NULL);
    if (mutex_error != 0) {
        ep_set_errno(mutex_error);
        goto fail;
    }
    fd_lock_initialized = 1;
    mutex_error = pthread_mutex_init(&port->wait_lock, NULL);
    if (mutex_error != 0) {
        ep_set_errno(mutex_error);
        goto fail;
    }
    wait_lock_initialized = 1;
    mutex_error = pthread_mutex_init(&port->iocp_post_lock, NULL);
    if (mutex_error != 0) {
        ep_set_errno(mutex_error);
        goto fail;
    }
    iocp_post_lock_initialized = 1;

    ep_ready_init(&port->ready_queue);
    if (!port->ready_queue.initialized) {
        goto fail;
    }
    ready_initialized = 1;

    if (size_hint > 0) {
        size_t hint = (size_t)size_hint;
        if (hint > SIZE_MAX / 2) {
            ep_set_errno(ENOMEM);
            goto fail;
        }
        hint *= 2;
        if (hint < WEPOLL_INITIAL_FDS) {
            hint = WEPOLL_INITIAL_FDS;
        }
        if (ep_fd_table_grow(port, hint) != 0) {
            goto fail;
        }

        if ((size_t)size_hint > pool_capacity) {
            pool_capacity = (size_t)size_hint;
            if (pool_capacity > 4096) {
                pool_capacity = 4096;
            }
        }
    }

    if (ep_afd_pool_init(&port->afd_info_pool,
                         sizeof(AFD_POLL_INFO), pool_capacity) != 0) {
        goto fail;
    }
    afd_pool_initialized = 1;
    if (ep_afd_pool_init(&port->ready_node_pool,
                         sizeof(ep_ready_node_t), pool_capacity) != 0) {
        goto fail;
    }
    node_pool_initialized = 1;

    port->iocp_batch_size = 64;
    port->iocp_entries = (OVERLAPPED_ENTRY *)calloc(
        port->iocp_batch_size, sizeof(*port->iocp_entries));
    if (port->iocp_entries == NULL) {
        ep_set_errno(ENOMEM);
        goto fail;
    }
    port->get_queued_completion_status_ex = GetQueuedCompletionStatusEx;
    port->post_queued_completion_status = PostQueuedCompletionStatus;
    {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");

        if (kernel32 != NULL) {
            ep_port_store_proc(
                &port->create_waitable_timer_ex_w,
                sizeof(port->create_waitable_timer_ex_w),
                GetProcAddress(kernel32, "CreateWaitableTimerExW"));
            ep_port_store_proc(
                &port->query_unbiased_interrupt_time_precise,
                sizeof(port->query_unbiased_interrupt_time_precise),
                GetProcAddress(kernel32,
                               "QueryUnbiasedInterruptTimePrecise"));
        }
        if (port->query_unbiased_interrupt_time_precise == NULL) {
            HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");

            if (kernelbase != NULL) {
                ep_port_store_proc(
                    &port->query_unbiased_interrupt_time_precise,
                    sizeof(port->query_unbiased_interrupt_time_precise),
                    GetProcAddress(
                        kernelbase, "QueryUnbiasedInterruptTimePrecise"));
            }
        }
    }

    if (ep_fault_hit(EP_FAULT_IOCP_CREATE) != 0) {
        goto fail;
    }
    port->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (port->iocp == NULL) {
        ep_set_errno(ep_winerr_to_errno(GetLastError()));
        goto fail;
    }
    port->iocp_post_handle = port->iocp;
    if (ep_afd_open(port->iocp, &port->afd) != 0) {
        goto fail;
    }

    port->close_on_exec = (flags & EPOLL_CLOEXEC) != 0;
    atomic_init(&port->waiter_active, 0);
    atomic_init(&port->closing, 0);
    atomic_init(&port->iocp_closed, 0);
    atomic_init(&port->iocp_post_error, 0);
    atomic_init(&port->iocp_post_failures, 0);
    atomic_init(&port->precise_timeout_active_generation, 0);
    atomic_init(&port->precise_timeout_armed, 0);
    atomic_init(&port->precise_timeout_post_failures, 0);
    atomic_init(&port->generation, 0);
    port->close_drain_timeout_ms = EP_CLOSE_DRAIN_TIMEOUT_MS;
    port->quarantine_drain_timeout_ms = EP_QUARANTINE_DRAIN_TIMEOUT_MS;
    port->next_sock_generation = 0;
    *out = port;
    return 0;

fail:
    {
        int saved_errno = ep_last_err();
        if (port->afd != NULL) {
            CloseHandle(port->afd);
        }
        if (port->iocp != NULL) {
            CloseHandle(port->iocp);
        }
        free(port->iocp_entries);
        free(port->fd_table);
        if (node_pool_initialized) {
            ep_afd_pool_destroy(&port->ready_node_pool);
        }
        if (afd_pool_initialized) {
            ep_afd_pool_destroy(&port->afd_info_pool);
        }
        if (ready_initialized) {
            ep_ready_destroy(&port->ready_queue);
        }
        if (wait_lock_initialized) {
            pthread_mutex_destroy(&port->wait_lock);
        }
        if (iocp_post_lock_initialized) {
            pthread_mutex_destroy(&port->iocp_post_lock);
        }
        if (fd_lock_initialized) {
            pthread_mutex_destroy(&port->fd_table_lock);
        }
        free(port);
        ep_set_errno(saved_errno);
        return -1;
    }
}

void ep_port_begin_close(ep_port_t *port)
{
    HANDLE iocp_to_close = NULL;

    if (port == NULL) {
        return;
    }
    if (atomic_exchange_explicit(&port->closing, 1,
                                 memory_order_acq_rel) == 0) {
        /* The post lock is a short HANDLE lease shared by callbacks and
         * control/error wakeups.  Revoke the alias while holding it before
         * closing a failed completion port; callbacks then fail without
         * retaining a stale numeric HANDLE. */
        pthread_mutex_lock(&port->iocp_post_lock);
        if (port->iocp_post_handle == NULL ||
            ep_fault_hit(EP_FAULT_IOCP_POST) != 0 ||
            !port->post_queued_completion_status(
                port->iocp_post_handle, 0, 0, NULL)) {
            /* A failed wake must not leave an API waiter holding the port
             * reference forever.  Closing the completion port wakes blocked
             * GetQueuedCompletionStatusEx calls with an abandoned-wait error.
             * Pending request storage is quarantined later if it cannot be
             * drained safely. */
            iocp_to_close = port->iocp_post_handle;
            port->iocp_post_handle = NULL;
            atomic_store_explicit(&port->iocp_closed, 1,
                                  memory_order_release);
        }
        pthread_mutex_unlock(&port->iocp_post_lock);
        if (iocp_to_close != NULL) {
            (void)CloseHandle(iocp_to_close);
        }
    }
}

static int ep_port_pending_count(ep_port_t *port, size_t *pending_out)
{
    pthread_mutex_lock(&port->fd_table_lock);
    *pending_out = port->pending_poll_count;
    pthread_mutex_unlock(&port->fd_table_lock);
    return 0;
}

/* Return one only for the current wait's timeout packet.  Stale timeout
 * generations and NULL control/wakeup packets are consumed internally. */
static int ep_port_dispatch_iocp_entry(ep_port_t *port,
                                       const OVERLAPPED_ENTRY *entry,
                                       ULONG_PTR timeout_generation)
{
    OVERLAPPED *overlapped = entry->lpOverlapped;

    if (overlapped == NULL) {
        return 0;
    }
    if (overlapped == &port->precise_timeout_overlapped) {
        ULONG_PTR packet_generation = (ULONG_PTR)entry->lpCompletionKey;

        return timeout_generation != 0 &&
               packet_generation == timeout_generation;
    }

    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)overlapped;
    ep_sock_t *completed = (ep_sock_t *)(
        (unsigned char *)iosb - offsetof(ep_sock_t, io_status_block));
    ep_sock_handle_completion(completed,
                              entry->dwNumberOfBytesTransferred,
                              iosb->Status);
    return 0;
}

/* Continue consuming cancellation completions until every kernel request has
 * released its embedded socket storage, the deadline expires, or IOCP becomes
 * unusable.  The caller owns wait_lock. */
static int ep_port_drain_pending_until(ep_port_t *port, uint64_t deadline)
{
    for (;;) {
        size_t pending;

        (void)ep_port_pending_count(port, &pending);
        if (pending == 0) return 0;
        if (atomic_load_explicit(&port->iocp_closed,
                                 memory_order_acquire)) {
            ep_set_errno(EIO);
            return -1;
        }

        uint64_t now = GetTickCount64();
        if (now >= deadline) {
            ep_set_errno(ETIMEDOUT);
            return 1;
        }
        uint64_t remaining = deadline - now;
        DWORD wait_ms = remaining < EP_CLOSE_DRAIN_SLICE_MS
            ? (DWORD)remaining : EP_CLOSE_DRAIN_SLICE_MS;
        ULONG removed = 0;
        if (ep_fault_hit(EP_FAULT_IOCP_DEQUEUE) != 0) {
            return -1;
        }
        BOOL ok = port->get_queued_completion_status_ex(
            port->iocp, port->iocp_entries, port->iocp_batch_size,
            &removed, wait_ms, FALSE);

        if (!ok) {
            DWORD error = GetLastError();
            if (error == WAIT_TIMEOUT) continue;
            ep_set_errno(ep_winerr_to_errno(error));
            return -1;
        }
        for (ULONG i = 0; i < removed; i++) {
            (void)ep_port_dispatch_iocp_entry(
                port, &port->iocp_entries[i], 0);
        }
    }
}

/* Final reclamation after pending_poll_count reaches zero.  The caller owns
 * wait_lock and no public API reference can reach the port. */
static void ep_port_finish_destroy_locked(ep_port_t *port)
{
    pthread_mutex_lock(&port->fd_table_lock);
    ep_sock_t *sock = port->sock_list_head;
    while (sock != NULL) {
        ep_sock_t *next = sock->next;
        ep_sock_free_locked(port, sock);
        sock = next;
    }
    pthread_mutex_unlock(&port->fd_table_lock);

    for (;;) {
        ep_ready_node_t *node = ep_ready_drain(&port->ready_queue, INT_MAX);
        if (node == NULL) break;
        while (node != NULL) {
            ep_ready_node_t *next = atomic_load_explicit(
                &node->next, memory_order_relaxed);
            ep_ready_node_free(port, node);
            node = next;
        }
    }
    ep_ready_destroy(&port->ready_queue);

    if (port->afd != NULL) {
        (void)CloseHandle(port->afd);
        port->afd = NULL;
    }
    ep_port_precise_timeout_destroy(port);
    {
        HANDLE iocp_to_close = ep_port_revoke_iocp_posts(port);

        if (iocp_to_close != NULL) {
            (void)CloseHandle(iocp_to_close);
        }
    }
    port->iocp = NULL;
    ep_afd_pool_destroy(&port->afd_info_pool);
    ep_afd_pool_destroy(&port->ready_node_pool);
    free(port->fd_table);
    free(port->iocp_entries);

    pthread_mutex_unlock(&port->wait_lock);
    pthread_mutex_destroy(&port->wait_lock);
    pthread_mutex_destroy(&port->iocp_post_lock);
    pthread_mutex_destroy(&port->fd_table_lock);
    free(port);
}

static void ep_port_abandon_locked(ep_port_t *port)
{
    if (port->afd != NULL) {
        (void)CloseHandle(port->afd);
        port->afd = NULL;
    }
    ep_port_precise_timeout_destroy(port);
    {
        HANDLE iocp_to_close = ep_port_revoke_iocp_posts(port);

        if (iocp_to_close != NULL) {
            (void)CloseHandle(iocp_to_close);
        }
    }
    port->iocp = NULL;
    atomic_fetch_add_explicit(&g_irrecoverable_ports, 1,
                              memory_order_relaxed);

    /* Outstanding AFD operations still reference sock->io_status_block and
     * afd_info.  Leaking this unreachable port is safer than freeing storage
     * that the kernel may complete asynchronously. */
    pthread_mutex_unlock(&port->wait_lock);
}

typedef struct ep_port_reaper_context {
    ep_port_t *port;
    HMODULE module;
} ep_port_reaper_context_t;

static int ep_port_pin_containing_module(HMODULE *module_out)
{
    HMODULE module = NULL;

    *module_out = NULL;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(const void *)&g_quarantined_ports,
            &module)) {
        return -1;
    }
    if (module == GetModuleHandleW(NULL)) {
        /* Code linked directly into the process image cannot be unloaded
         * while the process is alive, so it needs no reference pin. */
        return 0;
    }
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            (LPCWSTR)(const void *)&g_quarantined_ports,
            &module)) {
        return -1;
    }
    *module_out = module;
    return 0;
}

static int ep_port_reaper_admit(void)
{
    uint64_t active = atomic_load_explicit(
        &g_active_quarantines, memory_order_relaxed);

    while (active < EP_MAX_ACTIVE_QUARANTINES) {
        if (atomic_compare_exchange_weak_explicit(
                &g_active_quarantines, &active, active + 1,
                memory_order_acq_rel, memory_order_relaxed)) {
            return 1;
        }
    }
    return 0;
}

static DWORD WINAPI ep_port_reaper_thread(void *opaque)
{
    ep_port_reaper_context_t *context =
        (ep_port_reaper_context_t *)opaque;
    ep_port_t *port = context->port;
    HMODULE module = context->module;
    int drain_result;

    free(context);
    pthread_mutex_lock(&port->wait_lock);
    drain_result = ep_port_drain_pending_until(
        port, GetTickCount64() + port->quarantine_drain_timeout_ms);
    if (drain_result == 0) {
        ep_port_finish_destroy_locked(port);
        atomic_fetch_add_explicit(&g_reaped_ports, 1, memory_order_relaxed);
    } else {
        ep_port_abandon_locked(port);
    }
    atomic_fetch_sub_explicit(&g_active_quarantines, 1,
                              memory_order_release);

    if (module != NULL) {
        FreeLibraryAndExitThread(module, 0);
    }
    return 0;
}

static int ep_port_quarantine_recoverable_locked(ep_port_t *port, int error)
{
    ep_port_reaper_context_t *context =
        (ep_port_reaper_context_t *)calloc(1, sizeof(*context));
    HANDLE thread = NULL;

    atomic_fetch_add_explicit(&g_quarantined_ports, 1,
                              memory_order_relaxed);
    if (!ep_port_reaper_admit()) {
        ep_port_abandon_locked(port);
        ep_set_errno(error);
        return -1;
    }
    if (context != NULL) {
        context->port = port;
        if (ep_port_pin_containing_module(&context->module) != 0) {
            free(context);
            context = NULL;
        }
    }
    if (context != NULL) {
        thread = CreateThread(NULL, 0, ep_port_reaper_thread,
                              context, 0, NULL);
    }
    if (thread == NULL) {
        if (context != NULL && context->module != NULL) {
            (void)FreeLibrary(context->module);
        }
        free(context);
        atomic_fetch_sub_explicit(&g_active_quarantines, 1,
                                  memory_order_release);
        ep_port_abandon_locked(port);
    } else {
        (void)CloseHandle(thread);
        pthread_mutex_unlock(&port->wait_lock);
    }
    ep_set_errno(error);
    return -1;
}

int ep_port_destroy(ep_port_t *port)
{
    if (port == NULL) {
        return 0;
    }

    ep_port_begin_close(port);
    pthread_mutex_lock(&port->wait_lock);

    int cancel_failed = 0;
    pthread_mutex_lock(&port->fd_table_lock);
    for (ep_sock_t *sock = port->sock_list_head;
         sock != NULL;
         sock = sock->next) {
        if ((sock->user_flags & EPOLLEXCLUSIVE) != 0) {
            ep_exclusive_release_owner(sock);
        }
        atomic_store_explicit(&sock->delete_pending, 1, memory_order_relaxed);
        atomic_store_explicit(&sock->state, EP_SOCK_DELETED,
                              memory_order_relaxed);
        ep_sock_set_needs_rearm_locked(sock, 0);
        ep_sock_set_oneshot_fired_locked(sock, 0);
        if (ep_sock_cancel_locked(sock) != 0) {
            cancel_failed = 1;
        }
    }
    if (cancel_failed && port->afd != NULL) {
        /* Closing the AFD control handle is a bulk cancellation fallback.
         * Completions remain asynchronous and must still be drained. */
        (void)CloseHandle(port->afd);
        port->afd = NULL;
    }
    if (port->fd_table != NULL) {
        memset(port->fd_table, 0,
               port->fd_table_size * sizeof(*port->fd_table));
    }
    port->fd_table_count = 0;

    ep_sock_t *sock = port->sock_list_head;
    while (sock != NULL) {
        ep_sock_t *next = sock->next;
        if (atomic_load_explicit(&sock->poll_status,
                                 memory_order_relaxed) == EP_POLL_IDLE) {
            ep_sock_free_locked(port, sock);
        }
        sock = next;
    }
    pthread_mutex_unlock(&port->fd_table_lock);

    int drain_result = ep_port_drain_pending_until(
        port, GetTickCount64() + port->close_drain_timeout_ms);
    if (drain_result != 0) {
        int error = ep_last_err();

        if (drain_result > 0 &&
            !atomic_load_explicit(&port->iocp_closed,
                                  memory_order_acquire)) {
            return ep_port_quarantine_recoverable_locked(port, error);
        }
        atomic_fetch_add_explicit(&g_quarantined_ports, 1,
                                  memory_order_relaxed);
        ep_port_abandon_locked(port);
        ep_set_errno(error);
        return -1;
    }

    ep_port_finish_destroy_locked(port);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* epoll_ctl operations.                                                    */
/* ------------------------------------------------------------------------- */

int ep_port_register(ep_port_t *port, SOCKET fd,
                     uint32_t events, uint32_t flags,
                     epoll_data_t data, void *ctx)
{
    ep_sock_t *existing;
    ep_sock_t *sock;

    if (fd == INVALID_SOCKET) {
        ep_set_errno(EBADF);
        return -1;
    }
    pthread_mutex_lock(&port->fd_table_lock);
    if (atomic_load_explicit(&port->closing, memory_order_acquire)) {
        pthread_mutex_unlock(&port->fd_table_lock);
        ep_set_errno(EBADF);
        return -1;
    }
    existing = ep_fd_table_lookup(port, fd);
    if (existing != NULL) {
        int identity_result =
            ep_sock_validate_control_locked(port, existing);
        if (identity_result == 0) {
            pthread_mutex_unlock(&port->fd_table_lock);
            ep_set_errno(EEXIST);
            return -1;
        }
        if (identity_result < 0 || ep_last_err() != ENOENT) {
            pthread_mutex_unlock(&port->fd_table_lock);
            return -1;
        }
        /* A stable endpoint mismatch means this numeric SOCKET now names a
         * different object.  The stale entry is retired above; ADD may create
         * the new registration while the old cancellation completion drains. */
    }

    sock = ep_sock_alloc_locked(port, fd);
    if (sock == NULL) {
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }
    sock->user_events = events;
    sock->user_flags = flags;
    sock->user_data = data;
    sock->user_ctx = ctx;
    sock->observed_events = 0;
    sock->pipe_terminal_delivered = 0;
    sock->et_holdoff = 0;
    if ((sock->kind == EP_REG_WAITABLE || sock->kind == EP_REG_PIPE) &&
        (flags & EPOLLEXCLUSIVE) != 0) {
        /* Exclusive wake is defined for AFD socket polls only. */
        if (sock->afd_info != NULL) {
            ep_afd_pool_give(&port->afd_info_pool, sock->afd_info);
        }
        free(sock);
        pthread_mutex_unlock(&port->fd_table_lock);
        ep_set_errno(EINVAL);
        return -1;
    }
    if (sock->kind == EP_REG_WAITABLE &&
        (flags & EPOLLET) != 0 &&
        ep_waitable_interest_events(events) != 0 &&
        sock->waitable_semantics == EP_WAITABLE_ET_UNSUPPORTED) {
        free(sock);
        pthread_mutex_unlock(&port->fd_table_lock);
        ep_set_errno(EINVAL);
        return -1;
    }

    if (ep_fd_table_insert(port, sock) != 0) {
        if (sock->afd_info != NULL) {
            ep_afd_pool_give(&port->afd_info_pool, sock->afd_info);
        }
        free(sock);
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }
    ep_sock_list_add_locked(port, sock);
    if (!ep_waitable_is_dormant(sock)) {
        ep_sock_set_needs_rearm_locked(sock, 1);
    }

    /* When no waiter is blocked, defer the AFD request until the next wait.
     * This coalesces registration changes and, critically, lets independent
     * epoll instances arm the same socket from their own wait paths.  A port
     * with an active waiter must arm immediately so future readiness can wake
     * the already-blocked GetQueuedCompletionStatusEx call. */
    if (!ep_waitable_is_dormant(sock) &&
        (atomic_load_explicit(&port->waiter_active, memory_order_acquire)
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
        || sock->endpoint_id_state == EP_SOCKET_ID_TRANSITIONAL
#endif
        )) {
        int submit_result = EP_SOCK_SUBMIT_LOCKED(sock, 1);
        if (submit_result < 0) {
            ep_fd_table_remove(port, sock);
            ep_sock_free_locked(port, sock);
            pthread_mutex_unlock(&port->fd_table_lock);
            return -1;
        }
        if (submit_result > 0) {
            int saved_errno = ep_last_err();
            pthread_mutex_unlock(&port->fd_table_lock);
            ep_set_errno(saved_errno);
            return -1;
        }
    }
    atomic_fetch_add_explicit(&port->generation, 1, memory_order_relaxed);
    pthread_mutex_unlock(&port->fd_table_lock);
    return 0;
}

int ep_port_modify(ep_port_t *port, SOCKET fd,
                   uint32_t events, uint32_t flags,
                   epoll_data_t data, void *ctx)
{
    uint32_t old_pending_events;
    uint32_t old_ready_queued;
    uint32_t old_state;
    uint32_t old_observed_events;
    uint32_t old_user_events;
    uint32_t old_user_flags;
    uint32_t new_afd_events;
    uint32_t new_waitable_interest = 0;
    uint32_t poll_status;
    epoll_data_t old_user_data;
    void *old_user_ctx;
    SOCKET old_base_socket;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    uint64_t old_endpoint_id;
    uint8_t old_endpoint_id_state;
#endif
    uint64_t old_generation;
    uint8_t old_needs_rearm;
    uint8_t old_oneshot_fired;
    uint8_t old_pipe_terminal_delivered;
    uint8_t old_et_holdoff;
    uint32_t old_waitable_notification_owned;
    int pending_poll_covers_request = 0;
    int new_waitable_dormant = 0;

    pthread_mutex_lock(&port->fd_table_lock);
    ep_sock_t *sock = ep_fd_table_lookup(port, fd);
    if (sock == NULL) {
        pthread_mutex_unlock(&port->fd_table_lock);
        ep_set_errno(ENOENT);
        return -1;
    }
    if (ep_sock_validate_control_locked(port, sock) != 0) {
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }
    if ((sock->user_flags & EPOLLEXCLUSIVE) != 0) {
        pthread_mutex_unlock(&port->fd_table_lock);
        ep_set_errno(EINVAL);
        return -1;
    }
    if (sock->kind == EP_REG_WAITABLE) {
        new_waitable_interest = ep_waitable_interest_events(events);
        new_waitable_dormant = new_waitable_interest == 0;
        if ((flags & EPOLLET) != 0 && !new_waitable_dormant &&
            sock->waitable_semantics == EP_WAITABLE_ET_UNSUPPORTED) {
            pthread_mutex_unlock(&port->fd_table_lock);
            ep_set_errno(EINVAL);
            return -1;
        }
    }

    new_afd_events = ep_sock_afd_events_locked(sock, events);
    poll_status = atomic_load_explicit(&sock->poll_status,
                                       memory_order_relaxed);
    if (poll_status == EP_POLL_PENDING) {
        if (sock->kind == EP_REG_WAITABLE && new_waitable_dormant) {
            /* A zero-interest waitable is logically dormant.  Join and disarm
             * a registered callback before publishing the new mask.  If a
             * callback already posted, cancellation leaves only its IOCP
             * accounting pending and preserves any consumed notification. */
            if (ep_sock_cancel_locked(sock) != 0) {
                pthread_mutex_unlock(&port->fd_table_lock);
                return -1;
            }
        } else if (sock->kind == EP_REG_WAITABLE ||
                   sock->kind == EP_REG_PIPE) {
            /* Auxiliary waits observe generic HANDLE/pipe readiness and
             * translate it only after completion.  Their in-flight operation
             * therefore covers every MOD mask.  Keeping it alive is also
             * required for consumptive waits: a successful callback may have
             * consumed a signal/count and posted its completion already. */
            pending_poll_covers_request = 1;
        } else if ((new_afd_events & ~sock->submitted_afd_events) == 0) {
            /* The in-flight request already covers this mask.  Keep it alive
             * and let completion snapshot the latest data/context/generation.
             * Narrowing is filtered against user_events at delivery time. */
            pending_poll_covers_request = 1;
        } else {
            if (ep_sock_cancel_locked(sock) != 0) {
                pthread_mutex_unlock(&port->fd_table_lock);
                return -1;
            }
        }
    }

    old_user_events = sock->user_events;
    old_user_flags = sock->user_flags;
    old_user_data = sock->user_data;
    old_user_ctx = sock->user_ctx;
    old_pending_events = sock->pending_events;
    old_observed_events = sock->observed_events;
    old_oneshot_fired = sock->oneshot_fired;
    old_needs_rearm = sock->needs_rearm;
    old_pipe_terminal_delivered = sock->pipe_terminal_delivered;
    old_et_holdoff = sock->et_holdoff;
    old_waitable_notification_owned = atomic_load_explicit(
        &sock->waitable_notification_owned, memory_order_acquire);
    old_base_socket = sock->base_socket;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    old_endpoint_id = sock->endpoint_id;
    old_endpoint_id_state = sock->endpoint_id_state;
#endif
    old_generation = sock->generation;
    old_ready_queued = atomic_load_explicit(&sock->ready_queued,
                                             memory_order_relaxed);
    old_state = atomic_load_explicit(&sock->state, memory_order_relaxed);

    ep_ready_node_t *replacement_node = NULL;
    uint32_t replacement_events = 0;
    int preserve_waitable_ready = 0;
    int transfer_waitable_notification = 0;

    if (old_ready_queued && sock->kind == EP_REG_WAITABLE &&
        sock->waitable_semantics != EP_WAITABLE_PERSISTENT &&
        sock->waitable_semantics != EP_WAITABLE_TERMINAL &&
        (old_pending_events & ~(EPOLLERR | EPOLLHUP)) != 0) {
        replacement_events = new_waitable_interest;
        if (replacement_events != 0) {
            replacement_node = ep_ready_node_alloc(port);
            if (replacement_node == NULL) {
                pthread_mutex_unlock(&port->fd_table_lock);
                return -1;
            }
            preserve_waitable_ready = 1;
        } else {
            /* Invalidating the old generation removes the only ready node that
             * represents this consumed notification.  Return ownership to the
             * dormant registration for replay when interest is restored. */
            transfer_waitable_notification = 1;
        }
    }

    sock->user_events = events;
    sock->user_flags = flags;
    sock->user_data = data;
    sock->user_ctx = ctx;
    sock->pending_events = preserve_waitable_ready ? replacement_events : 0;
    /* MOD resets edge observation so a newly requested interest can form a
     * fresh edge against the current level. */
    sock->observed_events = 0;
    sock->pipe_terminal_delivered = 0;
    sock->et_holdoff = 0;
    ep_sock_set_oneshot_fired_locked(sock, 0);
    if (transfer_waitable_notification) {
        atomic_store_explicit(&sock->waitable_notification_owned, 1,
                              memory_order_release);
    }
    if (new_waitable_dormant) {
        ep_sock_set_needs_rearm_locked(sock, 0);
    } else if (preserve_waitable_ready) {
        ep_sock_set_needs_rearm_locked(sock, 0);
    } else if (!pending_poll_covers_request) {
        ep_sock_set_needs_rearm_locked(sock, 1);
    }
    atomic_store_explicit(&sock->ready_queued,
                          preserve_waitable_ready ? 1U : 0U,
                          memory_order_relaxed);
    sock->generation = ++port->next_sock_generation;
    if (port->next_sock_generation == 0) {
        port->next_sock_generation = 1;
        sock->generation = 1;
    }

    if (preserve_waitable_ready) {
        if ((flags & EPOLLET) != 0) {
            sock->observed_events = replacement_events;
        }
        if ((flags & EPOLLONESHOT) != 0) {
            ep_sock_set_oneshot_fired_locked(sock, 1);
        }
        atomic_store_explicit(&sock->state, EP_SOCK_READY,
                              memory_order_relaxed);

        replacement_node->data = data;
        replacement_node->user_ctx = ctx;
        replacement_node->fd = sock->fd;
        replacement_node->sock_generation = sock->generation;
        replacement_node->events = replacement_events;
        replacement_node->flags = 0;
        if ((flags & EPOLLET) != 0) {
            replacement_node->flags |=
                WEPOLL_FLAG_ET_DELIVERED | WEPOLL_FLAG_EDGE_ARMED;
        }
        if ((flags & EPOLLONESHOT) != 0) {
            replacement_node->flags |= WEPOLL_FLAG_ONESHOT_FIRED;
        }
        replacement_node->timestamp = ep_now_ns();
        ep_ready_push(&port->ready_queue, replacement_node);
    } else if (new_waitable_dormant) {
        atomic_store_explicit(&sock->state, EP_SOCK_REGISTERED,
                              memory_order_relaxed);
    }

    if (!new_waitable_dormant &&
        atomic_load_explicit(&sock->poll_status, memory_order_relaxed) ==
            EP_POLL_IDLE &&
        atomic_load_explicit(&port->waiter_active, memory_order_acquire)) {
        int submit_result = EP_SOCK_SUBMIT_LOCKED(sock, 1);
        if (submit_result > 0) {
            int saved_errno = ep_last_err();
            pthread_mutex_unlock(&port->fd_table_lock);
            ep_set_errno(saved_errno);
            return -1;
        }
        if (submit_result < 0) {
            int saved_errno = ep_last_err();
            sock->user_events = old_user_events;
            sock->user_flags = old_user_flags;
            sock->user_data = old_user_data;
            sock->user_ctx = old_user_ctx;
            sock->pending_events = old_pending_events;
            sock->observed_events = old_observed_events;
            ep_sock_set_oneshot_fired_locked(sock, old_oneshot_fired);
            ep_sock_set_needs_rearm_locked(sock, old_needs_rearm);
            sock->pipe_terminal_delivered = old_pipe_terminal_delivered;
            sock->et_holdoff = old_et_holdoff;
            atomic_store_explicit(&sock->waitable_notification_owned,
                                  old_waitable_notification_owned,
                                  memory_order_release);
            sock->base_socket = old_base_socket;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
            sock->endpoint_id = old_endpoint_id;
            sock->endpoint_id_state = old_endpoint_id_state;
#endif
            sock->generation = old_generation;
            atomic_store_explicit(&sock->ready_queued, old_ready_queued,
                                  memory_order_relaxed);
            atomic_store_explicit(&sock->state, old_state,
                                  memory_order_relaxed);
            pthread_mutex_unlock(&port->fd_table_lock);
            ep_set_errno(saved_errno);
            return -1;
        }
    }
    atomic_fetch_add_explicit(&port->generation, 1, memory_order_relaxed);
    pthread_mutex_unlock(&port->fd_table_lock);
    return 0;
}

int ep_port_unregister(ep_port_t *port, SOCKET fd)
{
    int saved_errno = ep_last_err();

    pthread_mutex_lock(&port->fd_table_lock);
    ep_sock_t *sock = ep_fd_table_lookup(port, fd);
    if (sock == NULL) {
        pthread_mutex_unlock(&port->fd_table_lock);
        ep_set_errno(ENOENT);
        return -1;
    }
    if (ep_sock_validate_control_locked(port, sock) != 0) {
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }

    if ((sock->user_flags & EPOLLEXCLUSIVE) != 0) {
        ep_exclusive_release_owner(sock);
    }

    if (atomic_load_explicit(&sock->poll_status, memory_order_relaxed) ==
        EP_POLL_PENDING) {
        /* DEL is a logical detach operation.  Even if kernel cancellation
         * fails, remove the public registration now and retain sock storage
         * until the outstanding completion arrives.  This also makes a
         * DEL-before-closesocket() lifetime contract safe for embedders. */
        (void)ep_sock_cancel_locked(sock);
    }

    ep_fd_table_remove(port, sock);
    atomic_store_explicit(&sock->delete_pending, 1, memory_order_relaxed);
    atomic_store_explicit(&sock->state, EP_SOCK_DELETED, memory_order_relaxed);
    ep_sock_set_needs_rearm_locked(sock, 0);
    sock->generation = ++port->next_sock_generation;
    atomic_fetch_add_explicit(&port->generation, 1, memory_order_relaxed);

    if (atomic_load_explicit(&sock->poll_status, memory_order_relaxed) ==
        EP_POLL_IDLE) {
        ep_sock_free_locked(port, sock);
    }
    pthread_mutex_unlock(&port->fd_table_lock);
    ep_set_errno(saved_errno);
    return 0;
}

int ep_port_rearm(ep_port_t *port, SOCKET fd)
{
    uint32_t old_pending_events;
    uint32_t old_ready_queued;
    uint32_t old_state;
    uint32_t old_observed_events;
    SOCKET old_base_socket;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    uint64_t old_endpoint_id;
    uint8_t old_endpoint_id_state;
#endif
    uint64_t old_generation;
    uint8_t old_needs_rearm;
    uint8_t old_oneshot_fired;
    uint8_t old_pipe_terminal_delivered;
    uint8_t old_et_holdoff;
    uint32_t old_waitable_notification_owned;

    pthread_mutex_lock(&port->fd_table_lock);
    ep_sock_t *sock = ep_fd_table_lookup(port, fd);
    if (sock == NULL) {
        pthread_mutex_unlock(&port->fd_table_lock);
        ep_set_errno(ENOENT);
        return -1;
    }
    if (ep_sock_validate_control_locked(port, sock) != 0) {
        pthread_mutex_unlock(&port->fd_table_lock);
        return -1;
    }
    if (!sock->oneshot_fired) {
        pthread_mutex_unlock(&port->fd_table_lock);
        return 0;
    }

    old_pending_events = sock->pending_events;
    old_observed_events = sock->observed_events;
    old_oneshot_fired = sock->oneshot_fired;
    old_needs_rearm = sock->needs_rearm;
    old_pipe_terminal_delivered = sock->pipe_terminal_delivered;
    old_et_holdoff = sock->et_holdoff;
    old_waitable_notification_owned = atomic_load_explicit(
        &sock->waitable_notification_owned, memory_order_acquire);
    old_base_socket = sock->base_socket;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    old_endpoint_id = sock->endpoint_id;
    old_endpoint_id_state = sock->endpoint_id_state;
#endif
    old_generation = sock->generation;
    old_ready_queued = atomic_load_explicit(&sock->ready_queued,
                                             memory_order_relaxed);
    old_state = atomic_load_explicit(&sock->state, memory_order_relaxed);

    if (old_ready_queued && ep_waitable_may_consume(sock) &&
        (old_pending_events & ~(EPOLLERR | EPOLLHUP)) != 0) {
        /* Rearming before the queued ONESHOT node is drained invalidates that
         * node's generation.  Return its consumed notification to the live
         * registration so the rearmed generation can replay it exactly once. */
        atomic_store_explicit(&sock->waitable_notification_owned, 1,
                              memory_order_release);
    }
    ep_sock_set_oneshot_fired_locked(sock, 0);
    sock->pending_events = 0;
    sock->observed_events = 0;
    sock->pipe_terminal_delivered = 0;
    sock->et_holdoff = 0;
    ep_sock_set_needs_rearm_locked(sock, 1);
    atomic_store_explicit(&sock->ready_queued, 0, memory_order_relaxed);
    sock->generation = ++port->next_sock_generation;
    if (port->next_sock_generation == 0) {
        port->next_sock_generation = 1;
        sock->generation = 1;
    }
    int result = 0;
    if (atomic_load_explicit(&port->waiter_active, memory_order_acquire)) {
        result = EP_SOCK_SUBMIT_LOCKED(sock, 1);
    }
    if (result > 0) {
        int saved_errno = ep_last_err();
        result = -1;
        ep_set_errno(saved_errno);
    } else if (result < 0) {
        int saved_errno = ep_last_err();
        sock->pending_events = old_pending_events;
        sock->observed_events = old_observed_events;
        ep_sock_set_oneshot_fired_locked(sock, old_oneshot_fired);
        ep_sock_set_needs_rearm_locked(sock, old_needs_rearm);
        sock->pipe_terminal_delivered = old_pipe_terminal_delivered;
        sock->et_holdoff = old_et_holdoff;
        atomic_store_explicit(&sock->waitable_notification_owned,
                              old_waitable_notification_owned,
                              memory_order_release);
        sock->base_socket = old_base_socket;
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
        sock->endpoint_id = old_endpoint_id;
        sock->endpoint_id_state = old_endpoint_id_state;
#endif
        sock->generation = old_generation;
        atomic_store_explicit(&sock->ready_queued, old_ready_queued,
                              memory_order_relaxed);
        atomic_store_explicit(&sock->state, old_state,
                              memory_order_relaxed);
        ep_set_errno(saved_errno);
    } else {
        atomic_fetch_add_explicit(&port->generation, 1,
                                  memory_order_relaxed);
    }
    pthread_mutex_unlock(&port->fd_table_lock);
    return result;
}

/* ------------------------------------------------------------------------- */
/* Ready queue drain and wait loop.                                         */
/* ------------------------------------------------------------------------- */

static int ep_drain_to_buffer(ep_port_t *port,
                              epoll_event_ex *out,
                              int maxevents)
{
    int delivered = 0;

    while (delivered < maxevents) {
        ep_ready_node_t *node = ep_ready_drain(&port->ready_queue, 1);
        if (node == NULL) {
            break;
        }

        int valid = 0;
        pthread_mutex_lock(&port->fd_table_lock);
        ep_sock_t *sock = ep_fd_table_lookup(port, node->fd);
        if (sock != NULL && sock->generation == node->sock_generation &&
            !atomic_load_explicit(&sock->delete_pending,
                                  memory_order_relaxed)) {
#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
            ep_identity_check_t identity_check =
                ep_sock_validate_identity_locked(sock, 0);

            if (identity_check == EP_IDENTITY_STALE ||
                identity_check == EP_IDENTITY_CLOSED) {
                /* The completion belongs to the old kernel socket object.
                 * Retire it before a queued snapshot can surface stale data
                 * for a replacement that reused the same numeric handle. */
                ep_sock_retire_stale_locked(port, sock);
            } else if (identity_check == EP_IDENTITY_ERROR) {
#ifdef WEPOLL_EX_STRICT_SOCKET_IDENTITY
                int error = ep_last_err();

                sock->pending_events = 0;
                atomic_store_explicit(&sock->ready_queued, 0,
                                      memory_order_relaxed);
                atomic_store_explicit(&sock->state, EP_SOCK_REGISTERED,
                                      memory_order_relaxed);
                ep_sock_set_oneshot_fired_locked(sock, 0);
                ep_sock_set_needs_rearm_locked(sock, 1);
                ep_port_record_async_error_locked(port, error);
#else
                /* Best-effort builds preserve legacy delivery when a provider
                 * identity query fails transiently. */
                valid = 1;
#endif
            } else
#endif
            {
                valid = 1;
            }
            if (valid) {
                int pipe_et = sock->kind == EP_REG_PIPE &&
                    (sock->user_flags & EPOLLET) != 0;
                ep_pipe_snapshot_t pipe_snapshot = {0};
                int final_pipe_et;

                if (pipe_et) {
                    pipe_snapshot = ep_pipe_snapshot(sock);
                }
                final_pipe_et = pipe_et && pipe_snapshot.valid &&
                    !pipe_snapshot.local_closed &&
                    pipe_snapshot.events == node->events &&
                    ep_pipe_et_snapshot_is_final(
                        sock, node->events, &pipe_snapshot);

                sock->pending_events = 0;
                atomic_store_explicit(&sock->ready_queued, 0,
                                      memory_order_relaxed);
                atomic_store_explicit(&sock->state, EP_SOCK_REGISTERED,
                                      memory_order_relaxed);
                if (pipe_et) {
                    sock->pipe_terminal_delivered =
                        ep_pipe_et_events_have_final_shape(
                            sock, node->events) != 0;
                }
                if (sock->kind == EP_REG_WAITABLE &&
                    (sock->user_flags & EPOLLET) != 0 &&
                    sock->waitable_semantics == EP_WAITABLE_CONSUMPTIVE) {
                    /* The wait that produced this notification consumed one
                     * signal/count.  Reopen the latch without another wait,
                     * which could silently consume the next notification. */
                    sock->observed_events = 0;
                } else if (pipe_et) {
                    if (pipe_snapshot.valid && !pipe_snapshot.local_closed &&
                        pipe_snapshot.events == 0) {
                        sock->observed_events = 0;
                    }
                }
                if (!sock->oneshot_fired &&
                    !(sock->kind == EP_REG_WAITABLE &&
                      (sock->user_flags & EPOLLET) != 0 &&
                      sock->waitable_semantics == EP_WAITABLE_TERMINAL) &&
                    !final_pipe_et) {
                    /* Level and edge registrations both re-arm after a
                     * delivered snapshot.  Process/thread objects and final
                     * pipe terminal snapshots cannot transition again, so
                     * their delivered ET registrations stay idle until a
                     * later MOD explicitly starts a fresh observation. */
                    sock->et_holdoff = 0;
                    ep_sock_set_needs_rearm_locked(sock, 1);
                }
            }
        } else {
            port->stale_events_dropped++;
        }
        pthread_mutex_unlock(&port->fd_table_lock);

        if (valid) {
            out[delivered].events = node->events;
            out[delivered].data = node->data;
            out[delivered].flags = node->flags;
            out[delivered].timestamp = node->timestamp;
            out[delivered].user_ctx = node->user_ctx;
            delivered++;
        }
        ep_ready_node_free(port, node);
    }
    return delivered;
}

typedef struct ep_wait_state {
    const ep_wait_timeout_t *timeout;
    uint64_t start_ms;
    uint64_t start_100ns;
    ULONG_PTR timeout_generation;
    int precise_clock;
    int precise_enabled;
    int precise_attempted;
} ep_wait_state_t;

static void ep_wait_state_initialize(ep_port_t *port,
                                     const ep_wait_timeout_t *timeout,
                                     ep_wait_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->timeout = timeout;
    state->start_ms = GetTickCount64();
    if (timeout->precise && !timeout->infinite &&
        timeout->intervals_100ns != 0 &&
        port->query_unbiased_interrupt_time_precise != NULL) {
        ULONGLONG now = 0;

        port->query_unbiased_interrupt_time_precise(&now);
        state->start_100ns = (uint64_t)now;
        state->precise_clock = 1;
    }
}

static uint64_t ep_wait_remaining_milliseconds(
    ep_port_t *port, const ep_wait_state_t *state)
{
    if (state->precise_enabled) {
        ULONGLONG now = 0;
        uint64_t elapsed;
        uint64_t remaining;

        port->query_unbiased_interrupt_time_precise(&now);
        elapsed = (uint64_t)now - state->start_100ns;
        if (elapsed >= state->timeout->intervals_100ns) {
            return 0;
        }
        remaining = state->timeout->intervals_100ns - elapsed;
        return remaining / EP_100NS_PER_MILLISECOND +
               (remaining % EP_100NS_PER_MILLISECOND != 0);
    }

    {
        uint64_t elapsed = GetTickCount64() - state->start_ms;

        if (elapsed >= state->timeout->milliseconds) {
            return 0;
        }
        return state->timeout->milliseconds - elapsed;
    }
}

static uint64_t ep_wait_remaining_100ns(ep_port_t *port,
                                        const ep_wait_state_t *state)
{
    ULONGLONG now = 0;
    uint64_t elapsed;

    port->query_unbiased_interrupt_time_precise(&now);
    elapsed = (uint64_t)now - state->start_100ns;
    if (elapsed >= state->timeout->intervals_100ns) {
        return 0;
    }
    return state->timeout->intervals_100ns - elapsed;
}

static int ep_wait_expired(ep_port_t *port, const ep_wait_state_t *state)
{
    return !state->timeout->infinite &&
           ep_wait_remaining_milliseconds(port, state) == 0;
}

static DWORD ep_wait_backstop_milliseconds(ep_port_t *port,
                                           const ep_wait_state_t *state)
{
    uint64_t remaining;

    if (state->timeout->infinite) {
        return INFINITE;
    }
    remaining = ep_wait_remaining_milliseconds(port, state);
    if (remaining > (uint64_t)EP_MAX_FINITE_IOCP_WAIT_MS) {
        return EP_MAX_FINITE_IOCP_WAIT_MS;
    }
    return (DWORD)remaining;
}

/* The ready queue has one consumer, so waiters still serialize their drain
 * operation.  Do not let that serialization turn a bounded wait into an
 * unbounded mutex wait: a zero-timeout drain returns immediately when another
 * consumer owns the lock, and a positive timeout includes lock acquisition. */
static int ep_wait_lock_acquire(ep_port_t *port,
                                const ep_wait_state_t *wait_state)
{
    const ep_wait_timeout_t *timeout = wait_state->timeout;

    if (timeout->infinite) {
        int lock_result = pthread_mutex_lock(&port->wait_lock);
        if (lock_result != 0) {
            ep_set_errno(lock_result);
            return -1;
        }
        return 1;
    }

    int first_attempt = 1;
    for (;;) {
        if (!first_attempt && timeout->milliseconds != 0 &&
            GetTickCount64() - wait_state->start_ms >=
                timeout->milliseconds) {
            return 0;
        }
        first_attempt = 0;

        int lock_result = pthread_mutex_trylock(&port->wait_lock);
        if (lock_result == 0) {
            return 1;
        }
        if (lock_result != EBUSY) {
            ep_set_errno(lock_result);
            return -1;
        }
        if (atomic_load_explicit(&port->closing, memory_order_acquire)) {
            int post_error = atomic_load_explicit(
                &port->iocp_post_error, memory_order_acquire);

            ep_set_errno(post_error != 0 ? post_error : EBADF);
            return -1;
        }
        if (timeout->milliseconds == 0 ||
            GetTickCount64() - wait_state->start_ms >=
                timeout->milliseconds) {
            return 0;
        }
        Sleep(1);
    }
}

int ep_port_wait_timeout(ep_port_t *port, epoll_event_ex *out, int maxevents,
                         const ep_wait_timeout_t *timeout,
                         const wepoll_sigset_t *sigmask)
{
    ep_wait_state_t wait_state;
    uint64_t zero_timeout_deadline = 0;
    unsigned int zero_timeout_dequeues = 0;
    int result = -1;
    int lock_result;

    if (out == NULL || timeout == NULL) {
        ep_set_errno(EFAULT);
        return -1;
    }
    if (maxevents <= 0) {
        ep_set_errno(EINVAL);
        return -1;
    }
    /* Windows has no POSIX process signal mask.  Accept a non-null pointer
     * so portable epoll_pwait* call sites compile and run; the mask is not
     * applied.  This matches the public wepoll_sigset_t opacity on Win32. */
    (void)sigmask;

    ep_wait_state_initialize(port, timeout, &wait_state);
    if (!timeout->infinite && timeout->milliseconds == 0) {
        /* AFD cancellation and wake packets are internal to the API.  Drain
         * all immediately queued batches for a short bounded time slice
         * before reporting an empty nonblocking wait, otherwise a large
         * cancellation burst can hide a readiness packet.  Always process a
         * minimum number of nonblocking dequeues before enforcing the time
         * bound, so timer granularity cannot recreate a small packet limit.
         * The time bound still prevents another thread from turning timeout=0
         * into an unbounded internal-completion loop. */
        zero_timeout_deadline = GetTickCount64() +
                                EP_ZERO_TIMEOUT_DRAIN_BUDGET_MS;
    }
    lock_result = ep_wait_lock_acquire(port, &wait_state);
    if (lock_result <= 0) {
        return lock_result == 0 ? 0 : -1;
    }
    atomic_store_explicit(&port->waiter_active, 1, memory_order_release);

    /* An error deferred behind a previously returned ready batch wins at the
     * start of the next wait call.  This prevents a perpetually ready socket
     * from starving the error forever while preserving ready-before-error
     * ordering within the call that first observed both conditions. */
    {
        int post_error = ep_port_take_iocp_post_error(port);

        if (post_error != 0) {
            ep_set_errno(post_error);
            result = -1;
            goto done;
        }
    }
    if (atomic_load_explicit(&port->closing, memory_order_acquire)) {
        int post_error = ep_port_take_iocp_post_error(port);

        ep_set_errno(post_error != 0 ? post_error : EBADF);
        result = -1;
        goto done;
    }
    pthread_mutex_lock(&port->fd_table_lock);
    {
        ep_port_release_deferred_rearms_locked(port);
        int deferred_error = ep_port_take_async_error_locked(port);
        pthread_mutex_unlock(&port->fd_table_lock);
        if (deferred_error != 0) {
            ep_set_errno(deferred_error);
            result = -1;
            goto done;
        }
    }

    for (;;) {
        int async_error;
        int post_error = ep_port_take_iocp_post_error(port);

        if (post_error != 0) {
            ep_set_errno(post_error);
            result = -1;
            break;
        }
        if (atomic_load_explicit(&port->closing, memory_order_acquire)) {
            post_error = ep_port_take_iocp_post_error(port);
            ep_set_errno(post_error != 0 ? post_error : EBADF);
            result = -1;
            break;
        }

        result = ep_drain_to_buffer(port, out, maxevents);
        if (result > 0) {
            break;
        }

        pthread_mutex_lock(&port->fd_table_lock);
        async_error = ep_port_take_async_error_locked(port);
        int arm_result = async_error == 0
            ? ep_port_arm_pending_locked(port) : 0;
        pthread_mutex_unlock(&port->fd_table_lock);
        if (async_error != 0) {
            ep_set_errno(async_error);
            result = -1;
            break;
        }
        if (arm_result != 0) {
            int arm_error = ep_last_err();

            result = ep_drain_to_buffer(port, out, maxevents);
            if (result <= 0) {
                int wait_post_error = ep_port_take_iocp_post_error(port);

                if (wait_post_error != 0) arm_error = wait_post_error;
                pthread_mutex_lock(&port->fd_table_lock);
                {
                    int reported_error =
                        ep_port_take_async_error_locked(port);
                    if (reported_error != 0) arm_error = reported_error;
                }
                pthread_mutex_unlock(&port->fd_table_lock);
                ep_set_errno(arm_error);
                result = -1;
            }
            break;
        }

        result = ep_drain_to_buffer(port, out, maxevents);
        if (result > 0) {
            break;
        }

        DWORD wait_ms;
        int deferred_rearm;
        int deferred_rearm_wait = 0;

        pthread_mutex_lock(&port->fd_table_lock);
        deferred_rearm = ep_port_has_deferred_rearm_locked(port);
        pthread_mutex_unlock(&port->fd_table_lock);

        if (timeout->precise && !timeout->infinite &&
            timeout->intervals_100ns != 0 &&
            !wait_state.precise_attempted) {
            wait_state.precise_attempted = 1;
            if (wait_state.precise_clock &&
                ep_port_precise_timeout_initialize(port)) {
                wait_state.precise_enabled = 1;
            }
        }
        if (!timeout->infinite && timeout->milliseconds != 0 &&
            ep_wait_expired(port, &wait_state)) {
            result = 0;
            break;
        }
        if (wait_state.precise_enabled &&
            wait_state.timeout_generation == 0) {
            uint64_t remaining_100ns =
                ep_wait_remaining_100ns(port, &wait_state);

            if (remaining_100ns == 0) {
                result = 0;
                break;
            }
            wait_state.timeout_generation =
                ep_port_precise_timeout_arm(port, remaining_100ns);
            if (wait_state.timeout_generation == 0) {
                wait_state.precise_enabled = 0;
                if (!timeout->infinite && timeout->milliseconds != 0 &&
                    ep_wait_expired(port, &wait_state)) {
                    result = 0;
                    break;
                }
            }
        }

        wait_ms = ep_wait_backstop_milliseconds(port, &wait_state);
        if (deferred_rearm &&
            (timeout->infinite || timeout->milliseconds != 0) &&
            (wait_ms == INFINITE || wait_ms > EP_DEFERRED_REARM_RETRY_MS)) {
            wait_ms = EP_DEFERRED_REARM_RETRY_MS;
            deferred_rearm_wait = 1;
        }

        ULONG removed = 0;
        if (ep_fault_hit(EP_FAULT_IOCP_DEQUEUE) != 0) {
            result = -1;
            break;
        }
        BOOL ok = port->get_queued_completion_status_ex(
            port->iocp, port->iocp_entries, port->iocp_batch_size,
            &removed, wait_ms, FALSE);
        if (!ok) {
            DWORD error = GetLastError();
            int wait_post_error = ep_port_take_iocp_post_error(port);

            if (wait_post_error != 0) {
                ep_set_errno(wait_post_error);
                result = -1;
                break;
            }
            if (error == WAIT_TIMEOUT) {
                if (deferred_rearm_wait) {
                    pthread_mutex_lock(&port->fd_table_lock);
                    ep_port_release_deferred_rearms_locked(port);
                    pthread_mutex_unlock(&port->fd_table_lock);
                    continue;
                }
                if (!ep_wait_expired(port, &wait_state)) {
                    /* A coarse IOCP boundary or a test dequeue hook may fire
                     * before the requested deadline.  Retry against the
                     * monotonic duration rather than under-waiting. */
                    Sleep(wait_state.precise_enabled ? 0 : 1);
                    continue;
                }
                result = 0;
            } else {
                ep_set_errno(ep_winerr_to_errno(error));
                result = -1;
            }
            break;
        }
        if (removed == 0) {
            if (!ep_wait_expired(port, &wait_state)) {
                continue;
            }
            result = 0;
            break;
        }

        int timeout_packet = 0;
        for (ULONG i = 0; i < removed; i++) {
            timeout_packet |= ep_port_dispatch_iocp_entry(
                port, &port->iocp_entries[i],
                wait_state.timeout_generation);
        }
        if (!timeout->infinite && timeout->milliseconds == 0) {
            zero_timeout_dequeues++;
        }

        result = ep_drain_to_buffer(port, out, maxevents);
        if (result > 0) {
            break;
        }
        post_error = ep_port_take_iocp_post_error(port);
        if (post_error != 0) {
            ep_set_errno(post_error);
            result = -1;
            break;
        }
        pthread_mutex_lock(&port->fd_table_lock);
        async_error = ep_port_take_async_error_locked(port);
        pthread_mutex_unlock(&port->fd_table_lock);
        if (async_error != 0) {
            ep_set_errno(async_error);
            result = -1;
            break;
        }
        if (atomic_load_explicit(&port->closing, memory_order_acquire)) {
            post_error = ep_port_take_iocp_post_error(port);
            ep_set_errno(post_error != 0 ? post_error : EBADF);
            result = -1;
            break;
        }
        if (timeout_packet && ep_wait_expired(port, &wait_state)) {
            result = 0;
            break;
        }
        if (!timeout->infinite && timeout->milliseconds == 0 &&
            zero_timeout_dequeues >= EP_ZERO_TIMEOUT_MIN_DEQUEUES &&
            GetTickCount64() >= zero_timeout_deadline) {
            pthread_mutex_lock(&port->fd_table_lock);
            port->zero_timeout_budget_hits++;
            pthread_mutex_unlock(&port->fd_table_lock);
            result = 0;
            break;
        }
        if (!timeout->infinite && timeout->milliseconds != 0 &&
            ep_wait_expired(port, &wait_state)) {
            result = 0;
            break;
        }
    }

done:
    if (wait_state.timeout_generation != 0) {
        ep_port_precise_timeout_disarm(port);
    }
    atomic_store_explicit(&port->waiter_active, 0, memory_order_release);
    pthread_mutex_unlock(&port->wait_lock);
    return result;
}

int ep_port_wait(ep_port_t *port, epoll_event_ex *out, int maxevents,
                 int timeout_ms, const wepoll_sigset_t *sigmask)
{
    ep_wait_timeout_t timeout;

    ep_wait_timeout_from_milliseconds(timeout_ms, &timeout);
    return ep_port_wait_timeout(port, out, maxevents, &timeout, sigmask);
}
