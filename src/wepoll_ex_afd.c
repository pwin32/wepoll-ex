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

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <windows.h>
#  include <winternl.h>
#endif

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

    if (!SetFileCompletionNotificationModes(h,
                                            FILE_SKIP_SET_EVENT_ON_HANDLE)) {
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
    /* Exclusive AFD polls cancel peer non-exclusive/exclusive requests for
     * the same provider handle, approximating Linux EPOLLEXCLUSIVE wake
     * uniqueness among wepoll-ex instances watching the same socket. */
    info->Exclusive         = (sock->user_flags & EPOLLEXCLUSIVE) != 0
                                  ? TRUE : FALSE;
    info->Handles[0].Handle = (HANDLE)sock->base_socket;
    info->Handles[0].Events = afd_events;

    memset(&sock->io_status_block, 0, sizeof(sock->io_status_block));
    sock->io_status_block.Status = STATUS_PENDING;

    /* Publish PENDING before entering the kernel: an immediately satisfied
     * poll can enqueue its completion on another thread before this call
     * returns. */
    old_submitted_afd_events = sock->submitted_afd_events;
    sock->submitted_afd_events = afd_events;
    atomic_store(&sock->poll_status, EP_POLL_PENDING);

    if (ep_fault_hit(EP_FAULT_AFD_SUBMIT) != 0) {
        atomic_store(&sock->poll_status, EP_POLL_IDLE);
        sock->submitted_afd_events = old_submitted_afd_events;
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

    /* STATUS_PENDING means the request was queued.  Any other success
     * code means it already completed (e.g. socket already readable),
     * which we treat as a normal completion via the IOCP path. */
    if (status != STATUS_SUCCESS && status != STATUS_PENDING) {
        atomic_store(&sock->poll_status, EP_POLL_IDLE);
        sock->submitted_afd_events = old_submitted_afd_events;
        ep_set_errno(ep_status_to_errno(status));
        return -1;
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
        out |= EPOLLPRI | EPOLLRDBAND;
    if (afd_events & AFD_POLL_SEND)
        out |= EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND;
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
    if (epoll_events & (EPOLLPRI | EPOLLRDBAND))
        afd |= AFD_POLL_RECEIVE_EXPEDITED;
    if (epoll_events & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND))
        afd |= AFD_POLL_SEND;
    return afd;
}
