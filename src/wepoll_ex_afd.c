/*
 * wepoll_ex_afd.c — AFD (Ancillary Function Driver) helpers.
 *
 * AFD is the kernel driver behind Windows Sockets.  By sending an
 * IOCTL_AFD_POLL to it we can ask the kernel to notify us (via an
 * IOCP completion) when a socket becomes readable/writable/etc.
 *
 * This is the same mechanism WSAPoll uses internally, but going through
 * the driver directly gives us:
 *   - One-shot edge-triggered semantics (by re-arming after each
 *     completion)
 *   - EPOLLRDHUP-style detection via AFD_POLL_RECEIVE_DISCONNECT
 *   - Sub-microsecond notification latency (no user-mode polling)
 *
 * The driver is undocumented but has been stable since Windows 8.
 * wepoll by Bert Belder pioneered this technique on Windows.
 */
#include "wepoll_ex_internal.h"

#include <stdlib.h>
#include <string.h>

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

#define AFD_OPEN_PACKET_SIZE  (sizeof(AFD_OPEN_PACKET) + sizeof(WCHAR))

typedef struct _AFD_OPEN_PACKET {
    ULONG TransportNameLength;
    LARGE_INTEGER EndpointAddress;
} AFD_OPEN_PACKET;

int ep_afd_open(HANDLE *out)
{
#ifdef _WIN32
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;
    HANDLE h;

    /* Build the AFD device path: \Device\Afd\WepollEx */
    static const WCHAR afd_name[] = L"\\Device\\Afd\\WepollEx";

    name.Buffer        = (PWSTR)afd_name;
    name.Length        = sizeof(afd_name) - sizeof(WCHAR);
    name.MaximumLength = sizeof(afd_name);

    InitializeObjectAttributes(&oa, &name, 0, NULL, NULL);

    /* We need to pass an AFD_OPEN_PACKET as the EaBuffer.  The
     * EndpointAddress field can be zero for our purposes (we never
     * actually use this handle to send/recv — we only IOCTL on it). */
    AFD_OPEN_PACKET pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.TransportNameLength = 0;
    pkt.EndpointAddress.QuadPart = 0;

    status = NtCreateFile(&h,
                          GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                          &oa,
                          &iosb,
                          NULL,
                          0,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_OPEN_IF,
                          0,
                          &pkt,
                          sizeof(pkt));
    if (status < 0) {
        ep_set_errno(ep_status_to_errno(status));
        return -1;
    }
    *out = h;
    return 0;
#else
    (void)out;
    ep_set_errno(ENOSYS);
    return -1;
#endif
}

/* --------------------------------------------------------------------- */
/* Submit an AFD poll request.                                          */
/*                                                                     */
/* The poll is asynchronous: it completes via the IOCP associated with */
/* the port.  When it completes, the OVERLAPPED embedded in ep_sock_t */
/* is signalled.                                                       */
/* --------------------------------------------------------------------- */

int ep_afd_poll_submit(ep_sock_t *sock, uint32_t afd_events)
{
#ifdef _WIN32
    IO_STATUS_BLOCK *iosb;
    NTSTATUS status;

    if (sock->afd_info == NULL) {
        sock->afd_info = (AFD_POLL_INFO *)calloc(1, sizeof(AFD_POLL_INFO));
        if (sock->afd_info == NULL) {
            ep_set_errno(ENOMEM);
            return -1;
        }
    }

    AFD_POLL_INFO *info = sock->afd_info;
    memset(info, 0, sizeof(*info));
    info->Timeout.QuadPart  = LLONG_MAX;
    info->NumberOfHandles   = 1;
    info->Exclusive         = FALSE;
    info->Handles[0].Handle = (HANDLE)sock->fd;
    info->Handles[0].Events = afd_events;

    memset(&sock->overlapped, 0, sizeof(sock->overlapped));

    /* IO_STATUS_BLOCK lives in the same memory as the OVERLAPPED's
     * Internal/InternalHigh fields.  This is documented by Microsoft
     * for NtDeviceIoControlFile consumers. */
    iosb = (IO_STATUS_BLOCK *)&sock->overlapped;
    iosb->Status = STATUS_PENDING;

    status = g_ntdll.NtDeviceIoControlFile(
        sock->port->afd,                /* FileHandle — actually AFD handle */
        NULL,                           /* Event — we use IOCP instead */
        NULL,                           /* ApcRoutine */
        (PVOID)sock,                    /* ApcContext — used by IOCP */
        iosb,                           /* IoStatusBlock */
        IOCTL_AFD_POLL,                 /* IoControlCode */
        info,                           /* InputBuffer */
        sizeof(*info),                  /* InputBufferLength */
        info,                           /* OutputBuffer (same) */
        sizeof(*info));                 /* OutputBufferLength */

    /* STATUS_PENDING means the request was queued.  Any other success
     * code means it already completed (e.g. socket already readable),
     * which we treat as a normal completion via the IOCP path. */
    if (status != STATUS_PENDING && status < 0) {
        ep_set_errno(ep_status_to_errno(status));
        return -1;
    }
    sock->poll_pending = 1;
    return 0;
#else
    (void)sock; (void)afd_events;
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

    if (afd_events & AFD_POLL_RECEIVE)            out |= EPOLLIN;
    if (afd_events & AFD_POLL_RECEIVE_EXPEDITED)  out |= EPOLLPRI;
    if (afd_events & AFD_POLL_SEND)               out |= EPOLLOUT;
    if (afd_events & AFD_POLL_DISCONNECT)         out |= EPOLLHUP;
    if (afd_events & AFD_POLL_RECEIVE_DISCONNECT) out |= EPOLLRDHUP;
    if (afd_events & AFD_POLL_ABORT)              out |= (EPOLLERR | EPOLLHUP);
    if (afd_events & AFD_POLL_CONNECT_FAIL)       out |= EPOLLERR;
    if (afd_events & AFD_POLL_CONNECT)            out |= EPOLLOUT;
    if (afd_events & AFD_POLL_LOCAL_CLOSE)        out |= EPOLLHUP;

    return out;
}

/* Compute the AFD events mask we should ask for given the user's
 * requested epoll events.  Note that EPOLLERR and EPOLLHUP are always
 * delivered by epoll on Linux even if not requested — we mirror that
 * by always arming AFD for the disconnect/abort events. */
uint32_t ep_epoll_to_afd_events(uint32_t epoll_events)
{
    uint32_t afd = 0;

    if (epoll_events & (EPOLLIN | EPOLLRDNORM | EPOLLRDBAND | EPOLLMSG))
        afd |= AFD_POLL_RECEIVE | AFD_POLL_RECEIVE_EXPEDITED;
    if (epoll_events & EPOLLPRI)
        afd |= AFD_POLL_RECEIVE_EXPEDITED;
    if (epoll_events & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND))
        afd |= AFD_POLL_SEND;
    if (epoll_events & EPOLLRDHUP)
        afd |= AFD_POLL_RECEIVE_DISCONNECT;

    /* Always arm for these — they're edge events that fire once and
     * need no user request to be delivered. */
    afd |= AFD_POLL_DISCONNECT | AFD_POLL_ABORT | AFD_POLL_CONNECT_FAIL
         | AFD_POLL_CONNECT | AFD_POLL_LOCAL_CLOSE;

    return afd;
}
