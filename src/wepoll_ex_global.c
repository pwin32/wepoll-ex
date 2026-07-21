/*
 * wepoll_ex_global.c — global one-shot initialization.
 *
 * Resolves NTDLL entry points (NtDeviceIoControlFile) and registers a
 * process-exit cleanup hook.  Safe to call from multiple threads.
 */
#include "wepoll_ex_internal.h"

#include <stdlib.h>

#ifdef _WIN32
#  include <windows.h>
#  include <winternl.h>
#endif

ep_ntdll_t g_ntdll = {0};

static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

#ifdef _WIN32
static NTSTATUS (NTAPI *p_RtlNtStatusToDosError)(NTSTATUS) = NULL;

DWORD RtlNtStatusToDosError(NTSTATUS s)
{
    if (p_RtlNtStatusToDosError == NULL) return (DWORD)s;
    return p_RtlNtStatusToDosError(s);
}
#else
/* POSIX stub — never actually called. */
DWORD RtlNtStatusToDosError(NTSTATUS s) { return (DWORD)s; }
#endif

static void ep_global_init_once(void)
{
#ifdef _WIN32
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == NULL) return;

    g_ntdll.NtDeviceIoControlFile =
        (PNtDeviceIoControlFile)GetProcAddress(ntdll, "NtDeviceIoControlFile");
    p_RtlNtStatusToDosError =
        (NTSTATUS (NTAPI *)(NTSTATUS))GetProcAddress(ntdll, "RtlNtStatusToDosError");

    g_ntdll.initialized = (g_ntdll.NtDeviceIoControlFile != NULL) ? 1 : 0;
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
    /* Nothing to tear down — NTDLL is process-lifetime. */
    g_ntdll.initialized = 0;
}
