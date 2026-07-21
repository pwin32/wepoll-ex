/*
 * wepoll_ex_errno.c — errno shim and NTSTATUS / WSA-error mapping.
 *
 * On Windows, we want consumer code that uses portable patterns like
 *
 *     if (epoll_ctl(...) == -1) {
 *         if (errno == EBADF) ...
 *         if (errno == EEXIST) ...
 *     }
 *
 * to behave exactly like on Linux.  Windows does not define these errno
 * values for socket operations — instead it uses WSA* error codes.  We
 * translate.
 */
#include "wepoll_ex_internal.h"

#include <errno.h>

#ifdef _WIN32
#  include <winsock2.h>
#endif

/* On Windows, errno is per-thread via TLS so this is safe. */
void ep_set_errno(int e) { errno = e; }
int  ep_last_err(void)   { return errno; }

int ep_winerr_to_errno(DWORD wsaerr)
{
    /* Translation table mirrors Cygwin's winerr_to_errno. */
    switch (wsaerr) {
    case 0:                   return 0;
    case WSAEWOULDBLOCK:      return EAGAIN;
    case WSAEINPROGRESS:      return EAGAIN;
    case WSAEALREADY:         return EAGAIN;
    case WSAEINVAL:           return EINVAL;
    case WSAEBADF:            return EBADF;
    case WSAEACCES:           return EACCES;
    case WSAEFAULT:           return EFAULT;
    case WSAEMFILE:           return EMFILE;
    case WSAENFILE:           return ENFILE;
    case WSAENOBUFS:          return ENOBUFS;
    case WSAENOMEM:           return ENOMEM;
    case WSAENOTSOCK:         return ENOTSOCK;
    case WSAEDESTADDRREQ:     return EDESTADDRREQ;
    case WSAEMSGSIZE:         return EMSGSIZE;
    case WSAEPROTOTYPE:       return EPROTOTYPE;
    case WSAENOPROTOOPT:      return ENOPROTOOPT;
    case WSAEPROTONOSUPPORT:  return EPROTONOSUPPORT;
    case WSAESOCKTNOSUPPORT:  return ESOCKTNOSUPPORT;
    case WSAEOPNOTSUPP:       return EOPNOTSUPP;
    case WSAEPFNOSUPPORT:     return EPFNOSUPPORT;
    case WSAEAFNOSUPPORT:     return EAFNOSUPPORT;
    case WSAEADDRINUSE:       return EADDRINUSE;
    case WSAEADDRNOTAVAIL:    return EADDRNOTAVAIL;
    case WSAENETDOWN:         return ENETDOWN;
    case WSAENETUNREACH:      return ENETUNREACH;
    case WSAENETRESET:        return ENETRESET;
    case WSAECONNABORTED:     return ECONNABORTED;
    case WSAECONNRESET:       return ECONNRESET;
    case WSAENOCONN:          return ENOTCONN;
    case WSAESHUTDOWN:        return EPIPE;
    case WSAETOOMANYREFS:     return ETOOMANYREFS;
    case WSAETIMEDOUT:        return ETIMEDOUT;
    case WSAECONNREFUSED:     return ECONNREFUSED;
    case WSAELOOP:            return ELOOP;
    case WSAENAMETOOLONG:     return ENAMETOOLONG;
    case WSAEHOSTDOWN:        return EHOSTDOWN;
    case WSAEHOSTUNREACH:     return EHOSTUNREACH;
    case WSAENOTEMPTY:        return ENOTEMPTY;
    case WSAEUSERS:           return EUSERS;
    case WSAEDQUOT:           return EDQUOT;
    case WSAESTALE:           return ESTALE;
    case WSAEREMOTE:          return EREMOTE;
    default:                  return EINVAL;
    }
}

int ep_status_to_errno(NTSTATUS s)
{
    /* Convert NTSTATUS to a Windows error code, then to errno. */
    DWORD wsaerr = RtlNtStatusToDosError(s);
    return ep_winerr_to_errno(wsaerr);
}
