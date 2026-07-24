/* Deterministic coverage for bounded epfd API-reference shutdown. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

/* Internal static-library hooks; intentionally absent from the public
 * installed header. */
uint64_t ep_api_close_timeout_count(void);
void ep_test_set_api_close_timeout_ms(unsigned int timeout_ms);
void *ep_test_api_ref_hold(int epfd);
void ep_test_api_ref_release(void *reference);
uint64_t ep_test_api_deferred_destroy_count(void);

int main(void)
{
    const unsigned int timeout_ms = 50U;
    WSADATA wsa_data;
    void *reference;
    uint64_t timeout_count;
    uint64_t destroy_count;
    uint64_t started;
    uint64_t elapsed;
    int close_error;
    int close_result;
    int epfd;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fputs("close-refs: WSAStartup failed\n", stderr);
        return 2;
    }

    epfd = epoll_create1(0);
    if (epfd < 0) {
        fprintf(stderr, "close-refs: epoll_create1 failed, errno=%d\n",
                errno);
        WSACleanup();
        return 1;
    }
    reference = ep_test_api_ref_hold(epfd);
    if (reference == NULL) {
        fprintf(stderr, "close-refs: could not hold reference, errno=%d\n",
                errno);
        (void)wepoll_close(epfd);
        WSACleanup();
        return 1;
    }

    timeout_count = ep_api_close_timeout_count();
    destroy_count = ep_test_api_deferred_destroy_count();
    ep_test_set_api_close_timeout_ms(timeout_ms);
    errno = 0;
    started = GetTickCount64();
    close_result = wepoll_close(epfd);
    elapsed = GetTickCount64() - started;
    close_error = errno;

    /* WaitForSingleObject and GetTickCount64 both have coarse timer
     * resolution on Windows, so a requested N ms wait can observe slightly
     * less than N elapsed milliseconds.  Require a bounded, non-zero wait
     * rather than a perfect lower bound. */
    if (close_result != -1 || close_error != ETIMEDOUT ||
        elapsed == 0 || elapsed + 20U < timeout_ms ||
        elapsed >= UINT64_C(1000) ||
        ep_api_close_timeout_count() != timeout_count + UINT64_C(1)) {
        fprintf(stderr,
                "close-refs: close=%d errno=%d elapsed=%llu count=%llu\n",
                close_result, close_error, (unsigned long long)elapsed,
                (unsigned long long)ep_api_close_timeout_count());
        if (close_result != 0) {
            ep_test_api_ref_release(reference);
        }
        WSACleanup();
        return 1;
    }

    errno = 0;
    if (epoll_fd_count(epfd) != -1 || errno != EBADF) {
        fprintf(stderr,
                "close-refs: timed-out descriptor remained visible, errno=%d\n",
                errno);
        ep_test_api_ref_release(reference);
        WSACleanup();
        return 1;
    }

    /* Releasing the final reference performs deferred port destruction.  It
     * must not replace the errno selected by the operation being completed. */
    errno = EDOM;
    ep_test_api_ref_release(reference);
    if (errno != EDOM ||
        ep_test_api_deferred_destroy_count() !=
            destroy_count + UINT64_C(1)) {
        fprintf(stderr,
                "close-refs: deferred cleanup failed, errno=%d count=%llu\n",
                errno,
                (unsigned long long)ep_test_api_deferred_destroy_count());
        WSACleanup();
        return 1;
    }

    puts("close-refs: OK");
    WSACleanup();
    return 0;
}
