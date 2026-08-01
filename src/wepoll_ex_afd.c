/*
 * wepoll_ex_afd.c — AFD (Ancillary Function Driver) helpers.
 *
 * AFD is the kernel driver behind Windows Sockets.  By sending an
 * IOCTL_AFD_POLL to it we can ask the kernel to notify us (via an
 * IOCP completion) when a socket becomes readable/writable/etc.
 *
 * This is the same mechanism WSAPoll uses internally, but going through
 * the driver directly gives us:
 *   - Asynchronous, one-completion-at-a-time readiness notification
 *   - EPOLLRDHUP-style detection via AFD_POLL_DISCONNECT
 *   - Sub-microsecond notification latency (no user-mode polling)
 *
 * The driver is undocumented but has been stable since Windows 8.
 * wepoll by Bert Belder pioneered this technique on Windows.
 */
#include "wepoll_ex_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <windows.h>
#  include <winternl.h>
#endif

#ifdef _WIN32
/* AFD poll requests can interfere when their single handle entry contains
 * the same numeric HANDLE value, including across independently opened AFD
 * control handles.  Keep one active owner for every target value in this
 * process.  Callers enter from port code while holding fd_table_lock, so the
 * only permitted nesting order is port fd_table_lock -> this lock. */
#define EP_AFD_POLL_KEY_BUCKETS 256u

static SRWLOCK g_afd_poll_key_lock = SRWLOCK_INIT;
static ep_sock_t *g_afd_poll_key_buckets[EP_AFD_POLL_KEY_BUCKETS];

static size_t ep_afd_poll_key_bucket(HANDLE target)
{
    uintptr_t value = (uintptr_t)target;

    /* Kernel handles are aligned.  Fold those otherwise-unused low bits back
     * into the mask so adjacent allocated handle values spread across the
     * power-of-two table. */
    value ^= value >> 4;
    value ^= value >> 12;
    return (size_t)(value & (EP_AFD_POLL_KEY_BUCKETS - 1u));
}

static int ep_afd_poll_key_is_active_locked(HANDLE target)
{
    size_t bucket = ep_afd_poll_key_bucket(target);

    for (ep_sock_t *active = g_afd_poll_key_buckets[bucket];
         active != NULL;
         active = active->afd_poll_key_next) {
        assert(active->afd_poll_key_owned != 0);
        if (active->afd_poll_target == target)
            return 1;
    }
    return 0;
}

typedef struct ep_afd_error_state {
    int error;
    int wsa_error;
    DWORD last_error;
} ep_afd_error_state_t;

static ep_afd_error_state_t ep_afd_error_state_save(void)
{
    ep_afd_error_state_t state;

    state.error = ep_last_err();
    state.wsa_error = WSAGetLastError();
    state.last_error = GetLastError();
    return state;
}

static void ep_afd_error_state_restore(ep_afd_error_state_t state)
{
    ep_set_errno(state.error);
    WSASetLastError(state.wsa_error);
    SetLastError(state.last_error);
}

static HANDLE ep_afd_poll_key_release_locked(ep_sock_t *sock)
{
    ep_sock_t **link;
    HANDLE reservation;

    if (sock->afd_poll_key_owned == 0) {
        assert(sock->afd_poll_key_next == NULL);
        assert(sock->afd_poll_key_reservation == NULL);
        return NULL;
    }

    link = &g_afd_poll_key_buckets[
        ep_afd_poll_key_bucket(sock->afd_poll_target)];
    while (*link != NULL && *link != sock)
        link = &(*link)->afd_poll_key_next;
    assert(*link == sock);
    if (*link == sock)
        *link = sock->afd_poll_key_next;

    reservation = sock->afd_poll_key_reservation;
    sock->afd_poll_target = NULL;
    sock->afd_poll_key_reservation = NULL;
    sock->afd_poll_key_next = NULL;
    sock->afd_poll_key_owned = 0;
    return reservation;
}

static void ep_afd_poll_key_insert_locked(ep_sock_t *sock, HANDLE target)
{
    size_t bucket = ep_afd_poll_key_bucket(target);

    assert(sock->afd_poll_key_owned == 0);
    assert(sock->afd_poll_key_next == NULL);
    assert(sock->afd_poll_key_reservation == NULL);
    assert(!ep_afd_poll_key_is_active_locked(target));
    sock->afd_poll_target = target;
    sock->afd_poll_key_reservation = NULL;
    sock->afd_poll_key_next = g_afd_poll_key_buckets[bucket];
    sock->afd_poll_key_owned = 1;
    g_afd_poll_key_buckets[bucket] = sock;
}

static int ep_afd_poll_key_claim(ep_sock_t *sock, HANDLE base_target,
                                 int *duplicated_out)
{
    HANDLE *collisions = NULL;
    size_t collision_count = 0;
    size_t collision_capacity = 0;
    int error = 0;
    DWORD win_error = ERROR_SUCCESS;

    assert(sock->afd_poll_key_owned == 0);
    assert(sock->afd_poll_key_next == NULL);
    assert(sock->afd_poll_key_reservation == NULL);

    AcquireSRWLockExclusive(&g_afd_poll_key_lock);
    if (!ep_afd_poll_key_is_active_locked(base_target)) {
        ep_afd_poll_key_insert_locked(sock, base_target);
        ReleaseSRWLockExclusive(&g_afd_poll_key_lock);
        *duplicated_out = 0;
        return 0;
    }
    ReleaseSRWLockExclusive(&g_afd_poll_key_lock);

    {
        HANDLE process = GetCurrentProcess();

        for (;;) {
            HANDLE duplicate = NULL;

            if (!DuplicateHandle(process,
                                 base_target,
                                 process,
                                 &duplicate,
                                 0,
                                 FALSE,
                                 DUPLICATE_SAME_ACCESS)) {
                win_error = GetLastError();
                error = ep_winerr_to_errno(win_error);
                break;
            }

            AcquireSRWLockExclusive(&g_afd_poll_key_lock);
            if (!ep_afd_poll_key_is_active_locked(duplicate)) {
                ep_afd_poll_key_insert_locked(sock, duplicate);
                ReleaseSRWLockExclusive(&g_afd_poll_key_lock);
                *duplicated_out = 1;
                break;
            }
            ReleaseSRWLockExclusive(&g_afd_poll_key_lock);

            /* An active key normally has a live base or reservation handle,
             * making a numeric collision impossible.  Reservation can fail,
             * however, so retain every colliding duplicate until a distinct
             * value is allocated instead of repeatedly recycling one slot. */
            if (collision_count == collision_capacity) {
                size_t new_capacity = collision_capacity == 0
                                          ? 8
                                          : collision_capacity * 2;
                HANDLE *new_collisions;

                if (new_capacity < collision_capacity ||
                    new_capacity > SIZE_MAX / sizeof(*collisions)) {
                    (void)CloseHandle(duplicate);
                    error = ENOMEM;
                    break;
                }
                if (ep_fault_hit(EP_FAULT_AFD_KEY_COLLISION_GROW) != 0) {
                    error = ep_last_err();
                    (void)CloseHandle(duplicate);
                    break;
                }
                new_collisions = (HANDLE *)realloc(
                    collisions, new_capacity * sizeof(*collisions));
                if (new_collisions == NULL) {
                    (void)CloseHandle(duplicate);
                    error = ENOMEM;
                    break;
                }
                collisions = new_collisions;
                collision_capacity = new_capacity;
            }
            collisions[collision_count++] = duplicate;
        }
    }

    for (size_t i = 0; i < collision_count; i++)
        (void)CloseHandle(collisions[i]);
    free(collisions);

    if (error != 0) {
        ep_set_errno(error);
        if (win_error != ERROR_SUCCESS) {
            WSASetLastError((int)win_error);
            SetLastError(win_error);
        }
        return -1;
    }
    return 0;
}

static void ep_afd_poll_duplicate_finish(ep_sock_t *sock, int reserve_slot)
{
    ep_afd_error_state_t error_state = ep_afd_error_state_save();
    HANDLE target = sock->afd_poll_target;
    HANDLE reservation = NULL;

    assert(sock->afd_poll_key_owned != 0);
    assert(sock->afd_poll_key_reservation == NULL);

    /* NtDeviceIoControlFile has captured the duplicated target for the
     * pending IRP.  Drop the duplicate and, for a pending request, immediately
     * try to occupy the same process handle-table slot.  Never retry a failed
     * close by numeric value: HANDLE reuse means a later lookup could name and
     * eventually close an unrelated object.  The logical active-key index is
     * sufficient if the close or optional reservation fails. */
    if (CloseHandle(target) != 0 && reserve_slot &&
        ep_fault_hit(EP_FAULT_AFD_KEY_RESERVATION) == 0) {
        reservation = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (reservation != target && reservation != NULL) {
            (void)CloseHandle(reservation);
            reservation = NULL;
        }
    }

    if (reservation != NULL) {
        AcquireSRWLockExclusive(&g_afd_poll_key_lock);
        assert(sock->afd_poll_key_owned != 0);
        assert(sock->afd_poll_target == target);
        assert(sock->afd_poll_key_reservation == NULL);
        sock->afd_poll_key_reservation = reservation;
        ReleaseSRWLockExclusive(&g_afd_poll_key_lock);
    }

    /* CloseHandle/CreateEventW are bookkeeping only after AFD has accepted
     * the request.  Do not leak their incidental error state into a successful
     * epoll operation or overwrite the submit fault/status chosen by caller. */
    ep_afd_error_state_restore(error_state);
}

static void ep_afd_poll_duplicate_abandon(ep_sock_t *sock)
{
    ep_afd_poll_duplicate_finish(sock, 0);
    ep_afd_poll_key_release(sock);
}
#endif

void ep_afd_poll_key_release(ep_sock_t *sock)
{
#ifdef _WIN32
    HANDLE reservation;

    if (sock == NULL)
        return;

    AcquireSRWLockExclusive(&g_afd_poll_key_lock);
    reservation = ep_afd_poll_key_release_locked(sock);
    ReleaseSRWLockExclusive(&g_afd_poll_key_lock);

    /* Detach under the registry lock, then close outside it.  A live handle
     * prevents numeric reuse on its own, so no kernel call needs to serialize
     * unrelated AFD submissions. */
    if (reservation != NULL) {
        ep_afd_error_state_t error_state = ep_afd_error_state_save();

        (void)CloseHandle(reservation);
        ep_afd_error_state_restore(error_state);
    }
#else
    (void)sock;
#endif
}

/* --------------------------------------------------------------------- */
/* Open the AFD device.  wepoll opens a single handle per port and uses */
/* it as the file handle in NtDeviceIoControlFile calls against each    */
/* socket fd.                                                            */
/* --------------------------------------------------------------------- */

int ep_afd_open(HANDLE iocp, HANDLE *out)
{
#ifdef _WIN32
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;
    HANDLE h = NULL;

    if (iocp == NULL || out == NULL || g_ntdll.NtCreateFile == NULL) {
        ep_set_errno(EINVAL);
        return -1;
    }
    if (ep_fault_hit(EP_FAULT_AFD_OPEN) != 0)
        return -1;

    static const WCHAR afd_name[] = AFD_DEVICE_NAME;

    name.Buffer        = (PWSTR)afd_name;
    name.Length        = sizeof(afd_name) - sizeof(WCHAR);
    name.MaximumLength = sizeof(afd_name);

    InitializeObjectAttributes(&oa, &name, 0, NULL, NULL);

    /* Opening without extended attributes returns a control handle that has
     * no socket endpoint and is suitable for IOCTL_AFD_POLL. */
    status = g_ntdll.NtCreateFile(&h,
                                  SYNCHRONIZE,
                                  &oa,
                                  &iosb,
                                  NULL,
                                  0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  FILE_OPEN,
                                  0,
                                  NULL,
                                  0);
    if (status != STATUS_SUCCESS) {
        ep_set_errno(ep_status_to_errno(status));
        return -1;
    }

    if (CreateIoCompletionPort(h, iocp, 0, 0) == NULL) {
        ep_set_errno(ep_winerr_to_errno(GetLastError()));
        CloseHandle(h);
        return -1;
    }

    /* Immediate AFD polls are consumed synchronously by the port code.  Do
     * not also enqueue an IOCP packet for STATUS_SUCCESS; pending requests
     * still complete through IOCP normally.  This lets a stale queued poll be
     * refreshed in place instead of moving every ready socket to the tail of
     * a large completion backlog. */
    if (!SetFileCompletionNotificationModes(
            h, FILE_SKIP_SET_EVENT_ON_HANDLE |
                   FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)) {
        ep_set_errno(ep_winerr_to_errno(GetLastError()));
        CloseHandle(h);
        return -1;
    }

    *out = h;
    return 0;
#else
    (void)iocp;
    (void)out;
    ep_set_errno(ENOSYS);
    return -1;
#endif
}

/* --------------------------------------------------------------------- */
/* Resolve the underlying provider socket used by AFD.                  */
/* --------------------------------------------------------------------- */

#ifdef _WIN32
#  ifndef SIO_BSP_HANDLE
#    define SIO_BSP_HANDLE 0x4800001B
#  endif
#  ifndef SIO_BSP_HANDLE_SELECT
#    define SIO_BSP_HANDLE_SELECT 0x4800001C
#  endif
#  ifndef SIO_BSP_HANDLE_POLL
#    define SIO_BSP_HANDLE_POLL 0x4800001D
#  endif
#  ifndef SIO_BASE_HANDLE
#    define SIO_BASE_HANDLE 0x48000022
#  endif
#  if !defined(WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME) && \
      !defined(SIO_QUERY_WFP_ALE_ENDPOINT_HANDLE)
#    define SIO_QUERY_WFP_ALE_ENDPOINT_HANDLE 0x580000CD
#  endif

static SOCKET ep_socket_ioctl_handle(SOCKET socket, DWORD ioctl,
                                     int *error_out, void *context)
{
    SOCKET result = INVALID_SOCKET;
    DWORD bytes = 0;

    (void)context;

    if (WSAIoctl(socket,
                 ioctl,
                 NULL,
                 0,
                 &result,
                 (DWORD)sizeof(result),
                 &bytes,
                 NULL,
                 NULL) == SOCKET_ERROR) {
        if (error_out != NULL)
            *error_out = WSAGetLastError();
        return INVALID_SOCKET;
    }

    /* The BSP-handle ioctls return exactly one SOCKET.  Treat a malformed
     * provider response as a failed lookup instead of passing an arbitrary
     * value to AFD. */
    if (bytes != sizeof(result) || result == INVALID_SOCKET) {
        if (error_out != NULL)
            *error_out = WSAEINVAL;
        return INVALID_SOCKET;
    }

    if (error_out != NULL)
        *error_out = 0;

    return result;
}

#  ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
static int ep_socket_ioctl_endpoint_id(
    SOCKET socket, DWORD ioctl, uint64_t *result_out, DWORD result_size,
    DWORD *bytes_out, int *error_out, void *context)
{
    int result;

    (void)context;

    result = WSAIoctl(socket,
                      ioctl,
                      NULL,
                      0,
                      result_out,
                      result_size,
                      bytes_out,
                      NULL,
                      NULL);
    if (error_out != NULL) {
        *error_out = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    }
    return result;
}
#  endif
#endif

#ifdef _WIN32
SOCKET ep_socket_get_base_with_ioctl(SOCKET socket,
                                     ep_socket_ioctl_fn ioctl_fn,
                                     void *context)
{
    SOCKET current = socket;
    SOCKET visited[32];
    unsigned int visited_count = 1;

    if (current == INVALID_SOCKET || ioctl_fn == NULL) {
        ep_set_errno(current == INVALID_SOCKET ? ENOTSOCK : EFAULT);
        return INVALID_SOCKET;
    }
    if (ep_fault_hit(EP_FAULT_PROVIDER_BASE) != 0)
        return INVALID_SOCKET;
    visited[0] = current;

    /* A finite bound protects against a broken provider returning a cycle.
     * The visited set catches short cycles immediately and also documents
     * that each fallback result must be a distinct provider-layer handle. */
    for (unsigned int depth = 0; depth < 32; depth++) {
        int base_error = WSAEINVAL;
        SOCKET base = ioctl_fn(current, SIO_BASE_HANDLE, &base_error,
                               context);
        if (base != INVALID_SOCKET)
            return base;

        if (base_error == WSAENOTSOCK) {
            ep_set_errno(ENOTSOCK);
            return INVALID_SOCKET;
        }

        /* SIO_BASE_HANDLE is documented as bypassing layered service
         * providers, but a few LSPs intercept or reject it.  Try the
         * provider-chain handles in order of specificity and accept only a
         * distinct result.  Once a layer is unwrapped, loop back to
         * SIO_BASE_HANDLE so multi-layer chains are handled as well. */
        static const DWORD fallback_ioctls[] = {
            SIO_BSP_HANDLE_SELECT,
            SIO_BSP_HANDLE_POLL,
            SIO_BSP_HANDLE
        };
        int advanced = 0;
        int saw_cycle = 0;

        for (size_t i = 0;
             i < sizeof(fallback_ioctls) / sizeof(fallback_ioctls[0]); i++) {
            SOCKET candidate = ioctl_fn(current, fallback_ioctls[i], NULL,
                                        context);
            if (candidate == INVALID_SOCKET || candidate == current)
                continue;

            for (unsigned int j = 0; j < visited_count; j++) {
                if (visited[j] == candidate) {
                    /* A provider can return a cyclic SELECT handle while a
                     * later POLL or generic handle still unwraps the layer.
                     * Remember the cycle, but exhaust the remaining
                     * fallbacks before rejecting the chain. */
                    saw_cycle = 1;
                    candidate = INVALID_SOCKET;
                    break;
                }
            }
            if (candidate == INVALID_SOCKET)
                continue;
            if (visited_count >= sizeof(visited) / sizeof(visited[0])) {
                ep_set_errno(ELOOP);
                return INVALID_SOCKET;
            }

            visited[visited_count++] = candidate;
            current = candidate;
            advanced = 1;
            break;
        }

        if (advanced)
            continue;

        if (saw_cycle) {
            ep_set_errno(ELOOP);
            return INVALID_SOCKET;
        }

        /* Preserve the SIO_BASE_HANDLE error.  The fallback probes are
         * best-effort and would otherwise overwrite the useful provider
         * error in the caller's errno. */
        ep_set_errno(ep_winerr_to_errno((DWORD)base_error));
        return INVALID_SOCKET;
    }

    ep_set_errno(ELOOP);
    return INVALID_SOCKET;
}
#endif

SOCKET ep_socket_get_base(SOCKET socket)
{
#ifdef _WIN32
    return ep_socket_get_base_with_ioctl(socket, ep_socket_ioctl_handle,
                                         NULL);
#else
    return socket;
#endif
}

#ifdef _WIN32
uint8_t ep_socket_protocol_from_info(const WSAPROTOCOL_INFOW *protocol_info,
                                     int protocol_info_length)
{
    if (protocol_info == NULL ||
        protocol_info_length < (int)sizeof(*protocol_info)) {
        return EP_SOCKET_PROTOCOL_UNKNOWN;
    }
    if ((protocol_info->iAddressFamily == AF_INET ||
         protocol_info->iAddressFamily == AF_INET6) &&
        protocol_info->iSocketType == SOCK_DGRAM &&
        protocol_info->iProtocol == IPPROTO_UDP &&
        protocol_info->iProtocolMaxOffset == 0) {
        return EP_SOCKET_PROTOCOL_UDP;
    }
    if ((protocol_info->iAddressFamily == AF_INET ||
         protocol_info->iAddressFamily == AF_INET6) &&
        protocol_info->iSocketType == SOCK_STREAM &&
        protocol_info->iProtocol == IPPROTO_TCP &&
        protocol_info->iProtocolMaxOffset == 0) {
        return EP_SOCKET_PROTOCOL_TCP;
    }
    return EP_SOCKET_PROTOCOL_UNKNOWN;
}
#endif

uint8_t ep_socket_get_protocol(SOCKET socket)
{
#ifdef _WIN32
    WSAPROTOCOL_INFOW protocol_info;
    int protocol_info_length = (int)sizeof(protocol_info);
    int saved_errno = ep_last_err();
    int saved_wsa_error = WSAGetLastError();
    uint8_t protocol = EP_SOCKET_PROTOCOL_UNKNOWN;

    memset(&protocol_info, 0, sizeof(protocol_info));
    if (socket != INVALID_SOCKET &&
        getsockopt(socket, SOL_SOCKET, SO_PROTOCOL_INFOW,
                   (char *)&protocol_info, &protocol_info_length) == 0) {
        protocol = ep_socket_protocol_from_info(&protocol_info,
                                                protocol_info_length);
    }
    WSASetLastError(saved_wsa_error);
    ep_set_errno(saved_errno);
    return protocol;
#else
    (void)socket;
    return EP_SOCKET_PROTOCOL_UNKNOWN;
#endif
}

#ifndef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
#ifdef _WIN32
int ep_socket_get_endpoint_id_with_ioctl(
    SOCKET socket, uint64_t *endpoint_id,
    ep_socket_endpoint_ioctl_fn ioctl_fn, void *context)
{
    uint64_t result = 0;
    DWORD bytes = 0;
    int ioctl_error = WSAEINVAL;

    if (socket == INVALID_SOCKET || endpoint_id == NULL || ioctl_fn == NULL) {
        ep_set_errno(socket == INVALID_SOCKET ? ENOTSOCK : EFAULT);
        return -1;
    }
    if (ep_fault_hit(EP_FAULT_ENDPOINT_UNAVAILABLE) != 0) {
        ep_set_errno(0);
        return 0;
    }
    if (ep_fault_hit(EP_FAULT_ENDPOINT_IDENTITY) != 0)
        return -1;

    if (ioctl_fn(socket,
                 SIO_QUERY_WFP_ALE_ENDPOINT_HANDLE,
                 &result,
                 (DWORD)sizeof(result),
                 &bytes,
                 &ioctl_error,
                 context) == SOCKET_ERROR) {
        /* Some non-TCP/IP providers do not expose a WFP ALE endpoint.  Keep
         * them usable with the legacy numeric-handle behavior rather than
         * rejecting an otherwise pollable Winsock socket. */
        if (ioctl_error == WSAEOPNOTSUPP ||
            ioctl_error == WSAENOPROTOOPT || ioctl_error == WSAEINVAL) {
            return 0;
        }
        ep_set_errno(ep_winerr_to_errno((DWORD)ioctl_error));
        return -1;
    }
    if (bytes != (DWORD)sizeof(result)) {
        ep_set_errno(EIO);
        return -1;
    }

    *endpoint_id = result;
    return 1;
}
#endif

int ep_socket_get_endpoint_id(SOCKET socket, uint64_t *endpoint_id)
{
#ifdef _WIN32
    return ep_socket_get_endpoint_id_with_ioctl(
        socket, endpoint_id, ep_socket_ioctl_endpoint_id, NULL);
#else
    (void)socket;
    (void)endpoint_id;
    return 0;
#endif
}
#endif

/* --------------------------------------------------------------------- */
/* Submit an AFD poll request.                                          */
/*                                                                     */
/* The poll is asynchronous: it completes via the IOCP associated with */
/* the port.  The completion packet carries the embedded IO_STATUS_BLOCK. */
/* --------------------------------------------------------------------- */

int ep_afd_poll_submit(ep_sock_t *sock, uint32_t afd_events, int *pending_out)
{
#ifdef _WIN32
    NTSTATUS status;
    uint32_t old_submitted_afd_events;
    int target_is_duplicate = 0;

    if (pending_out != NULL) {
        *pending_out = 0;
    }
    if (sock == NULL || sock->port == NULL || sock->port->afd == NULL ||
        g_ntdll.NtDeviceIoControlFile == NULL) {
        ep_set_errno(EINVAL);
        return -1;
    }

    if (atomic_load(&sock->poll_status) != EP_POLL_IDLE) {
        ep_set_errno(EALREADY);
        return -1;
    }

    if (sock->afd_info == NULL) {
        sock->afd_info = (AFD_POLL_INFO *)calloc(1, sizeof(AFD_POLL_INFO));
        if (sock->afd_info == NULL) {
            ep_set_errno(ENOMEM);
            return -1;
        }
    }

#ifdef WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME
    /* The registration captured the provider base handle under the caller's
     * DEL-before-closesocket lifetime contract.  Reusing it avoids another
     * SIO_BASE_HANDLE/LSP traversal on every re-arm. */
    if (sock->base_socket == INVALID_SOCKET) {
        ep_set_errno(ENOTSOCK);
        return -1;
    }
#else
    /* Hardened builds revalidate the provider chain before every request so
     * native close/reuse cannot attach an old base handle to a new socket. */
    sock->base_socket = ep_socket_get_base(sock->fd);
    if (sock->base_socket == INVALID_SOCKET)
        return -1;
#endif

    AFD_POLL_INFO *info = sock->afd_info;
    memset(info, 0, sizeof(*info));
    info->Timeout.QuadPart  = INT64_MAX;
    info->NumberOfHandles   = 1;
    /* AFD native exclusivity cancels peer polls for the provider handle,
     * including ordinary registrations.  Linux mixed EPOLLEXCLUSIVE
     * semantics instead wake every ordinary epoll instance plus at least one
     * exclusive instance.  Keep every kernel request non-exclusive and use
     * the process-wide readiness-class claim filter to arbitrate only the
     * user-visible exclusive deliveries. */
    info->Exclusive = FALSE;
    info->Handles[0].Events = afd_events;

    /* Insert the live target before entering the kernel.  The caller holds
     * fd_table_lock, so a same-socket completion cannot release ownership
     * until duplicate close/reservation bookkeeping below has finished. */
    if (ep_afd_poll_key_claim(sock,
                             (HANDLE)sock->base_socket,
                             &target_is_duplicate) != 0) {
        return -1;
    }
    info->Handles[0].Handle = sock->afd_poll_target;

    memset(&sock->io_status_block, 0, sizeof(sock->io_status_block));
    sock->io_status_block.Status = STATUS_PENDING;

    /* Publish PENDING before entering the kernel: an immediately satisfied
     * poll can enqueue its completion on another thread before this call
     * returns. */
    old_submitted_afd_events = sock->submitted_afd_events;
    sock->submitted_afd_events = afd_events;
    atomic_store(&sock->poll_status, EP_POLL_PENDING);

    if (ep_fault_hit(EP_FAULT_AFD_SUBMIT) != 0) {
        int saved_errno = ep_last_err();

        atomic_store(&sock->poll_status, EP_POLL_IDLE);
        sock->submitted_afd_events = old_submitted_afd_events;
        if (target_is_duplicate) {
            ep_afd_poll_duplicate_abandon(sock);
        } else {
            ep_afd_poll_key_release(sock);
        }
        ep_set_errno(saved_errno);
        return -1;
    }

    status = g_ntdll.NtDeviceIoControlFile(
        sock->port->afd,
        NULL,                           /* Event — we use IOCP instead */
        NULL,                           /* ApcRoutine */
        &sock->io_status_block,         /* ApcContext returned by IOCP */
        &sock->io_status_block,
        IOCTL_AFD_POLL,                 /* IoControlCode */
        info,                           /* InputBuffer */
        sizeof(*info),                  /* InputBufferLength */
        info,                           /* OutputBuffer (same) */
        sizeof(*info));                 /* OutputBufferLength */
    /* STATUS_PENDING means the request was queued.  STATUS_SUCCESS means the
     * output snapshot is complete now; FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
     * guarantees that no IOCP packet owns the embedded status block. */
    if (status != STATUS_SUCCESS && status != STATUS_PENDING) {
        int status_errno = ep_status_to_errno(status);

        atomic_store(&sock->poll_status, EP_POLL_IDLE);
        sock->submitted_afd_events = old_submitted_afd_events;
        if (target_is_duplicate) {
            ep_afd_poll_duplicate_abandon(sock);
        } else {
            ep_afd_poll_key_release(sock);
        }
        ep_set_errno(status_errno);
        return -1;
    }
    if (status == STATUS_PENDING) {
        if (target_is_duplicate)
            ep_afd_poll_duplicate_finish(sock, 1);
    } else {
        sock->io_status_block.Status = STATUS_SUCCESS;
        atomic_store(&sock->poll_status, EP_POLL_IDLE);
        if (target_is_duplicate) {
            ep_afd_poll_duplicate_abandon(sock);
        } else {
            ep_afd_poll_key_release(sock);
        }
    }
    /* A pending poll means the provider is not currently matching the
     * requested level.  Clear the edge latch so a later assert can form a
     * fresh EPOLLET edge after the user drained to EAGAIN.  Immediate
     * success keeps the latch so unread level does not redeliver. */
    if (status == STATUS_PENDING && (sock->user_flags & EPOLLET) != 0) {
        sock->observed_events = 0;
    }
    if (pending_out != NULL && status == STATUS_PENDING) {
        *pending_out = 1;
    }
    return 0;
#else
    (void)sock; (void)afd_events;
    if (pending_out != NULL) {
        *pending_out = 0;
    }
    ep_set_errno(ENOSYS);
    return -1;
#endif
}

int ep_afd_cancel(ep_sock_t *sock)
{
#ifdef _WIN32
    IO_STATUS_BLOCK cancel_status_block;
    NTSTATUS status;

    if (sock == NULL || sock->port == NULL ||
        g_ntdll.NtCancelIoFileEx == NULL) {
        ep_set_errno(EINVAL);
        return -1;
    }

    if (atomic_load(&sock->poll_status) != EP_POLL_PENDING ||
        sock->io_status_block.Status != STATUS_PENDING)
        return 0;

    if (ep_fault_hit(EP_FAULT_AFD_CANCEL) != 0)
        return -1;

    memset(&cancel_status_block, 0, sizeof(cancel_status_block));
    status = g_ntdll.NtCancelIoFileEx(sock->port->afd,
                                     &sock->io_status_block,
                                     &cancel_status_block);

    if (status == STATUS_SUCCESS) {
        atomic_store(&sock->poll_status, EP_POLL_CANCELLED);
        return 0;
    }

    /* The operation raced with cancellation and its completion packet is
     * already queued (or about to be queued). */
    if (status == STATUS_NOT_FOUND)
        return 0;

    ep_set_errno(ep_status_to_errno(status));
    return -1;
#else
    (void)sock;
    ep_set_errno(ENOSYS);
    return -1;
#endif
}

/* --------------------------------------------------------------------- */
/* Translate AFD poll result events to Linux epoll events.             */
/* --------------------------------------------------------------------- */

uint32_t ep_afd_to_epoll_events(ULONG afd_events, NTSTATUS afd_status,
                                uint8_t socket_protocol)
{
    uint32_t out = 0;

    if (afd_status < 0)
        out |= EPOLLERR;
    if (afd_events & (AFD_POLL_RECEIVE | AFD_POLL_ACCEPT))
        out |= EPOLLIN | EPOLLRDNORM;
    if (afd_events & AFD_POLL_RECEIVE_EXPEDITED)
        out |= EPOLLPRI;
    if (afd_events & AFD_POLL_SEND)
        out |= EPOLLOUT | EPOLLWRNORM;
    if (afd_events & AFD_POLL_DISCONNECT)
        out |= EPOLLIN | EPOLLRDNORM | EPOLLRDHUP;
    if (afd_events & AFD_POLL_ABORT) {
        out |= EPOLLERR;
        if (socket_protocol != EP_SOCKET_PROTOCOL_UDP)
            out |= EPOLLHUP;
    }
    if (afd_events & AFD_POLL_CONNECT_FAIL)
        out |= EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDNORM |
               EPOLLWRNORM | EPOLLRDHUP;
    if (afd_events & AFD_POLL_LOCAL_CLOSE)
        out |= EPOLLHUP;

    return out;
}

/* Compute the AFD events mask we should ask for given the user's requested
 * epoll events.  Abort, connect-failure, and local-close conditions remain
 * armed because EPOLLERR/EPOLLHUP are delivered even when not requested.
 * A graceful peer disconnect is readiness/half-close rather than an error;
 * arm it only when the caller requested a readable or RDHUP notification. */
uint32_t ep_epoll_to_afd_events(uint32_t epoll_events)
{
    uint32_t afd = AFD_POLL_ABORT | AFD_POLL_CONNECT_FAIL |
                   AFD_POLL_LOCAL_CLOSE;

    if (epoll_events & (EPOLLIN | EPOLLRDNORM)) {
        afd |= AFD_POLL_RECEIVE | AFD_POLL_ACCEPT | AFD_POLL_DISCONNECT;
    } else if (epoll_events & EPOLLRDHUP) {
        afd |= AFD_POLL_DISCONNECT;
    }
    if (epoll_events & EPOLLPRI)
        afd |= AFD_POLL_RECEIVE_EXPEDITED;
    if (epoll_events & (EPOLLOUT | EPOLLWRNORM))
        afd |= AFD_POLL_SEND;
    return afd;
}
