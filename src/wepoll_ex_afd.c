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
#  ifndef SIO_BSP_HANDLE_POLL
#    define SIO_BSP_HANDLE_POLL 0x4800001D
#  endif
#  ifndef SIO_BASE_HANDLE
#    define SIO_BASE_HANDLE 0x48000022
#  endif
#  ifndef SIO_QUERY_WFP_ALE_ENDPOINT_HANDLE
#    define SIO_QUERY_WFP_ALE_ENDPOINT_HANDLE 0x580000CD
#  endif

static SOCKET ep_socket_ioctl_handle(SOCKET socket, DWORD ioctl)
{
    SOCKET result = INVALID_SOCKET;
    DWORD bytes = 0;

    if (WSAIoctl(socket,
                 ioctl,
                 NULL,
                 0,
                 &result,
                 (DWORD)sizeof(result),
                 &bytes,
                 NULL,
                 NULL) == SOCKET_ERROR)
        return INVALID_SOCKET;

    return result;
}
#endif

SOCKET ep_socket_get_base(SOCKET socket)
{
#ifdef _WIN32
    SOCKET current = socket;

    if (current == INVALID_SOCKET) {
        ep_set_errno(ENOTSOCK);
        return INVALID_SOCKET;
    }

    /* A finite bound protects against a broken provider returning a cycle. */
    for (unsigned int depth = 0; depth < 32; depth++) {
        SOCKET base = ep_socket_ioctl_handle(current, SIO_BASE_HANDLE);
        if (base != INVALID_SOCKET)
            return base;

        int error = WSAGetLastError();
        if (error == WSAENOTSOCK) {
            ep_set_errno(ENOTSOCK);
            return INVALID_SOCKET;
        }

        base = ep_socket_ioctl_handle(current, SIO_BSP_HANDLE_POLL);
        if (base == INVALID_SOCKET || base == current) {
            ep_set_errno(ep_winerr_to_errno((DWORD)error));
            return INVALID_SOCKET;
        }
        current = base;
    }

    ep_set_errno(ELOOP);
    return INVALID_SOCKET;
#else
    return socket;
#endif
}

int ep_socket_get_endpoint_id(SOCKET socket, uint64_t *endpoint_id)
{
#ifdef _WIN32
    uint64_t result = 0;
    DWORD bytes = 0;

    if (socket == INVALID_SOCKET || endpoint_id == NULL) {
        ep_set_errno(socket == INVALID_SOCKET ? ENOTSOCK : EFAULT);
        return -1;
    }

    if (WSAIoctl(socket,
                 SIO_QUERY_WFP_ALE_ENDPOINT_HANDLE,
                 NULL,
                 0,
                 &result,
                 (DWORD)sizeof(result),
                 &bytes,
                 NULL,
                 NULL) == SOCKET_ERROR) {
        int error = WSAGetLastError();

        /* Some non-TCP/IP providers do not expose a WFP ALE endpoint.  Keep
         * them usable with the legacy numeric-handle behavior rather than
         * rejecting an otherwise pollable Winsock socket. */
        if (error == WSAEOPNOTSUPP || error == WSAENOPROTOOPT ||
            error == WSAEINVAL) {
            return 0;
        }
        ep_set_errno(ep_winerr_to_errno((DWORD)error));
        return -1;
    }
    if (bytes != sizeof(result)) {
        ep_set_errno(EIO);
        return -1;
    }

    *endpoint_id = result;
    return 1;
#else
    (void)socket;
    (void)endpoint_id;
    return 0;
#endif
}

/* --------------------------------------------------------------------- */
/* Submit an AFD poll request.                                          */
/*                                                                     */
/* The poll is asynchronous: it completes via the IOCP associated with */
/* the port.  The completion packet carries the embedded IO_STATUS_BLOCK. */
/* --------------------------------------------------------------------- */

int ep_afd_poll_submit(ep_sock_t *sock, uint32_t afd_events)
{
#ifdef _WIN32
    NTSTATUS status;

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

    sock->base_socket = ep_socket_get_base(sock->fd);
    if (sock->base_socket == INVALID_SOCKET)
        return -1;

    AFD_POLL_INFO *info = sock->afd_info;
    memset(info, 0, sizeof(*info));
    info->Timeout.QuadPart  = INT64_MAX;
    info->NumberOfHandles   = 1;
    info->Exclusive         = FALSE;
    info->Handles[0].Handle = (HANDLE)sock->base_socket;
    info->Handles[0].Events = afd_events;

    memset(&sock->io_status_block, 0, sizeof(sock->io_status_block));
    sock->io_status_block.Status = STATUS_PENDING;

    /* Publish PENDING before entering the kernel: an immediately satisfied
     * poll can enqueue its completion on another thread before this call
     * returns. */
    atomic_store(&sock->poll_status, EP_POLL_PENDING);

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
        ep_set_errno(ep_status_to_errno(status));
        return -1;
    }
    return 0;
#else
    (void)sock; (void)afd_events;
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

uint32_t ep_afd_to_epoll_events(ULONG afd_events)
{
    uint32_t out = 0;

    if (afd_events & (AFD_POLL_RECEIVE | AFD_POLL_ACCEPT))
        out |= EPOLLIN | EPOLLRDNORM;
    if (afd_events & AFD_POLL_RECEIVE_EXPEDITED)
        out |= EPOLLPRI | EPOLLRDBAND;
    if (afd_events & AFD_POLL_SEND)
        out |= EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND;
    if (afd_events & AFD_POLL_DISCONNECT)
        out |= EPOLLIN | EPOLLRDNORM | EPOLLRDHUP;
    if (afd_events & AFD_POLL_ABORT)
        out |= EPOLLHUP;
    if (afd_events & AFD_POLL_CONNECT_FAIL)
        out |= EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLRDNORM |
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

    if (epoll_events & (EPOLLIN | EPOLLRDNORM))
        afd |= AFD_POLL_RECEIVE | AFD_POLL_ACCEPT;
    if (epoll_events & (EPOLLIN | EPOLLRDNORM | EPOLLRDHUP))
        afd |= AFD_POLL_DISCONNECT;
    if (epoll_events & (EPOLLPRI | EPOLLRDBAND))
        afd |= AFD_POLL_RECEIVE_EXPEDITED;
    if (epoll_events & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND))
        afd |= AFD_POLL_SEND;
    return afd;
}
