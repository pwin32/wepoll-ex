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
#include <string.h>

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

static _Thread_local wepoll_ex_error_info g_last_error_info;

static int ep_errno_to_winsock(int error)
{
#ifdef _WIN32
    if (error == EAGAIN) return WSAEWOULDBLOCK;
    if (error == EACCES) return WSAEACCES;
    if (error == EAFNOSUPPORT) return WSAEAFNOSUPPORT;
    if (error == EALREADY) return WSAEALREADY;
    if (error == EBADF) return WSAEBADF;
    if (error == ECONNABORTED) return WSAECONNABORTED;
    if (error == ECONNREFUSED) return WSAECONNREFUSED;
    if (error == ECONNRESET) return WSAECONNRESET;
    if (error == EDESTADDRREQ) return WSAEDESTADDRREQ;
    if (error == EFAULT) return WSAEFAULT;
    if (error == EHOSTUNREACH) return WSAEHOSTUNREACH;
    if (error == EINPROGRESS) return WSAEINPROGRESS;
    if (error == EINVAL) return WSAEINVAL;
    if (error == EISCONN) return WSAEISCONN;
    if (error == EMFILE) return WSAEMFILE;
    if (error == EMSGSIZE) return WSAEMSGSIZE;
    if (error == ENETDOWN) return WSAENETDOWN;
    if (error == ENETRESET) return WSAENETRESET;
    if (error == ENETUNREACH) return WSAENETUNREACH;
    if (error == ENOBUFS || error == ENOMEM) return WSAENOBUFS;
    if (error == ENOPROTOOPT) return WSAENOPROTOOPT;
    if (error == ENOTCONN) return WSAENOTCONN;
    if (error == ENOTSOCK) return WSAENOTSOCK;
    if (error == EOPNOTSUPP) return WSAEOPNOTSUPP;
    if (error == EPROTONOSUPPORT) return WSAEPROTONOSUPPORT;
    if (error == EPROTOTYPE) return WSAEPROTOTYPE;
    if (error == ETIMEDOUT) return WSAETIMEDOUT;
#else
    (void)error;
#endif
    return 0;
}

static void ep_error_info_from_errno(wepoll_ex_error_info *error_info,
                                     int error)
{
    int winsock_error;

    memset(error_info, 0, sizeof(*error_info));
    error_info->version = WEPOLL_EX_ERROR_INFO_VERSION;
    error_info->struct_size = (uint32_t)sizeof(*error_info);
    error_info->portable_error = error;
    winsock_error = ep_errno_to_winsock(error);
    if (winsock_error != 0) {
        error_info->winsock_error = winsock_error;
        error_info->flags |= WEPOLL_EX_ERROR_WINSOCK_EQUIVALENT;
    }
}

/* On Windows, errno and the supplementary error channel are both per-thread. */
void ep_set_errno(int error)
{
    errno = error;
    ep_error_info_from_errno(&g_last_error_info, error);
}

int ep_last_err(void)
{
    return errno;
}

void ep_set_native_error(int portable_error, uint32_t native_domain,
                         uint32_t native_code)
{
    ep_set_errno(portable_error);
    g_last_error_info.native_domain = native_domain;
    g_last_error_info.native_code = native_code;
    g_last_error_info.flags |= WEPOLL_EX_ERROR_NATIVE_EXACT;
}

void ep_set_win32_error(DWORD error)
{
    int portable_error = ep_winerr_to_errno(error);

    ep_set_native_error(portable_error, WEPOLL_EX_NATIVE_ERROR_WIN32,
                        (uint32_t)error);
}

void ep_set_winsock_error(DWORD error)
{
    int portable_error = ep_winerr_to_errno(error);

    ep_set_native_error(portable_error, WEPOLL_EX_NATIVE_ERROR_WINSOCK,
                        (uint32_t)error);
    g_last_error_info.winsock_error = (int32_t)error;
    g_last_error_info.flags |= WEPOLL_EX_ERROR_WINSOCK_EQUIVALENT;
}

void ep_set_ntstatus_error(NTSTATUS status)
{
    int portable_error = ep_status_to_errno(status);

    ep_set_native_error(portable_error, WEPOLL_EX_NATIVE_ERROR_NTSTATUS,
                        (uint32_t)status);
}

void ep_get_last_error_info(wepoll_ex_error_info *error_info)
{
    if (g_last_error_info.version != WEPOLL_EX_ERROR_INFO_VERSION) {
        ep_error_info_from_errno(error_info, errno);
    } else {
        *error_info = g_last_error_info;
    }
}

void ep_restore_last_error_info(const wepoll_ex_error_info *error_info)
{
    g_last_error_info = *error_info;
    errno = error_info->portable_error;
}

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
