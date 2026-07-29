/*
 * wepoll_ex_global.c — global one-shot initialization.
 *
 * Resolves the NTDLL entry points used by the AFD backend and initializes
 * Winsock.  Safe to call from multiple threads.
 */
#include "wepoll_ex_internal.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <windows.h>
#  include <winternl.h>
#endif

ep_ntdll_t g_ntdll = {0};

static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

#ifdef _WIN32
static ULONG (NTAPI *p_RtlNtStatusToDosError)(NTSTATUS) = NULL;

static void ep_store_proc(void *target, size_t target_size, FARPROC proc)
{
    size_t copy_size = target_size < sizeof(proc) ? target_size : sizeof(proc);
    memset(target, 0, target_size);
    memcpy(target, &proc, copy_size);
}

DWORD ep_ntstatus_to_winerr(NTSTATUS status)
{
    if (p_RtlNtStatusToDosError == NULL) return (DWORD)status;
    return p_RtlNtStatusToDosError(status);
}
#else
/* POSIX stub — never actually called. */
DWORD ep_ntstatus_to_winerr(NTSTATUS status) { return (DWORD)status; }
#endif

static void ep_global_init_once(void)
{
#ifdef _WIN32
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == NULL) return;

    ep_store_proc(&g_ntdll.NtDeviceIoControlFile,
                  sizeof(g_ntdll.NtDeviceIoControlFile),
                  GetProcAddress(ntdll, "NtDeviceIoControlFile"));
    ep_store_proc(&g_ntdll.NtCreateFile,
                  sizeof(g_ntdll.NtCreateFile),
                  GetProcAddress(ntdll, "NtCreateFile"));
    ep_store_proc(&g_ntdll.NtCancelIoFileEx,
                  sizeof(g_ntdll.NtCancelIoFileEx),
                  GetProcAddress(ntdll, "NtCancelIoFileEx"));
    ep_store_proc(&g_ntdll.NtQueryObject,
                  sizeof(g_ntdll.NtQueryObject),
                  GetProcAddress(ntdll, "NtQueryObject"));
    ep_store_proc(&g_ntdll.NtQueryEvent,
                  sizeof(g_ntdll.NtQueryEvent),
                  GetProcAddress(ntdll, "NtQueryEvent"));
    ep_store_proc(&g_ntdll.NtQueryInformationFile,
                  sizeof(g_ntdll.NtQueryInformationFile),
                  GetProcAddress(ntdll, "NtQueryInformationFile"));
    ep_store_proc(&p_RtlNtStatusToDosError,
                  sizeof(p_RtlNtStatusToDosError),
                  GetProcAddress(ntdll, "RtlNtStatusToDosError"));

    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0)
        g_ntdll.wsa_initialized = 1;

    g_ntdll.initialized =
        g_ntdll.NtDeviceIoControlFile != NULL &&
        g_ntdll.NtCreateFile != NULL &&
        g_ntdll.NtCancelIoFileEx != NULL &&
        g_ntdll.NtQueryObject != NULL &&
        p_RtlNtStatusToDosError != NULL &&
        g_ntdll.wsa_initialized;
#else
    g_ntdll.initialized = 1;  /* Native epoll — nothing to resolve. */
#endif
}

int ep_global_init(void)
{
    pthread_once(&g_init_once, ep_global_init_once);
#ifdef _WIN32
    if (!g_ntdll.initialized) {
        ep_set_errno(ENOSYS);
        return -1;
    }
#endif
    return 0;
}

void ep_global_fini(void)
{
    if (g_ntdll.wsa_initialized) {
#ifdef _WIN32
        WSACleanup();
#endif
        g_ntdll.wsa_initialized = 0;
    }
    g_ntdll.initialized = 0;
}
