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

/* MinGW's errno.h intentionally exposes only the POSIX errors used by the
 * CRT.  Keep the Linux-facing mapping total without relying on optional
 * BSD/SysV names. */
#ifndef ESOCKTNOSUPPORT
#  define ESOCKTNOSUPPORT EOPNOTSUPP
#endif
#ifndef EPFNOSUPPORT
#  define EPFNOSUPPORT EAFNOSUPPORT
#endif
#ifndef ETOOMANYREFS
#  define ETOOMANYREFS EAGAIN
#endif
#ifndef EHOSTDOWN
#  define EHOSTDOWN EHOSTUNREACH
#endif
#ifndef EUSERS
#  define EUSERS EAGAIN
#endif
#ifndef EDQUOT
#  define EDQUOT ENOSPC
#endif
#ifndef ESTALE
#  define ESTALE EINVAL
#endif
#ifndef EREMOTE
#  define EREMOTE EINVAL
#endif

/* On Windows, errno is per-thread via TLS so this is safe. */
void ep_set_errno(int e) { errno = e; }
int  ep_last_err(void)   { return errno; }

int ep_winerr_to_errno(DWORD wsaerr)
{
#ifdef _WIN32
    /* RtlNtStatusToDosError returns ordinary Win32 codes, while Winsock APIs
     * return WSA* codes.  Accept both domains so callers do not need to know
     * which layer produced the error. */
    switch (wsaerr) {
    case 0:                   return 0;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_NOT_FOUND:     return ENOENT;
    case ERROR_TOO_MANY_OPEN_FILES:
                               return EMFILE;
    case ERROR_ACCESS_DENIED: return EACCES;
    case ERROR_INVALID_HANDLE:
    case ERROR_ABANDONED_WAIT_0:
                               return EBADF;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_NO_SYSTEM_RESOURCES:
                               return ENOMEM;
    case ERROR_INVALID_DATA:
    case ERROR_INVALID_PARAMETER:
                               return EINVAL;
    case ERROR_INVALID_ADDRESS:
                               return EFAULT;
    case ERROR_NOT_READY:
    case ERROR_IO_PENDING:    return EAGAIN;
    case ERROR_BUSY:
    case ERROR_SHARING_VIOLATION:
                               return EBUSY;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
                               return EEXIST;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:       return EPIPE;
    case ERROR_PIPE_NOT_CONNECTED:
                               return ENOTCONN;
    case ERROR_OPERATION_ABORTED:
    case ERROR_CANCELLED:     return ECANCELED;
    case ERROR_SEM_TIMEOUT:
    case ERROR_TIMEOUT:
    case WAIT_TIMEOUT:        return ETIMEDOUT;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
                               return ENOTSUP;
    case ERROR_INVALID_FUNCTION:
                               return ENOSYS;
    case ERROR_GEN_FAILURE:   return EIO;
    case WSAEWOULDBLOCK:      return EAGAIN;
    case WSAEINPROGRESS:      return EAGAIN;
    case WSAEALREADY:         return EAGAIN;
    case WSAEINVAL:           return EINVAL;
    case WSAEBADF:            return EBADF;
    case WSAEACCES:           return EACCES;
    case WSAEFAULT:           return EFAULT;
    case WSAEMFILE:           return EMFILE;
    case WSAENOBUFS:          return ENOBUFS;
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
    case WSAENOTCONN:         return ENOTCONN;
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
#else
    return wsaerr == 0 ? 0 : EINVAL;
#endif
}

int ep_status_to_errno(NTSTATUS s)
{
#ifdef _WIN32
    /* Convert NTSTATUS to a Windows error code, then to errno. */
    DWORD wsaerr = ep_ntstatus_to_winerr(s);
    return ep_winerr_to_errno(wsaerr);
#else
    return s == STATUS_SUCCESS ? 0 : EINVAL;
#endif
}
