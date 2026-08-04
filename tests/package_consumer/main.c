#include "wepoll_ex.h"

#include <errno.h>
#include <string.h>

int main(void)
{
    wepoll_ex_capabilities capabilities;
    static const char version_prefix[] =
        "wepoll-ex " WEPOLL_EX_VERSION_STRING " ";
    const char *version = wepoll_ex_version_string();
    if (wepoll_ex_version() != WEPOLL_EX_VERSION_NUMBER || version == 0 ||
        strncmp(version, version_prefix, sizeof(version_prefix) - 1) != 0) {
        return 1;
    }
    if (wepoll_ex_get_capabilities(&capabilities, sizeof(capabilities)) != 0 ||
        capabilities.version != WEPOLL_EX_CAPABILITIES_VERSION ||
        capabilities.struct_size != sizeof(capabilities) ||
        capabilities.flags == 0) {
        return 2;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        return 3;
    }

    struct epoll_event wake_event;
    memset(&wake_event, 0, sizeof(wake_event));
    wake_event.events = EPOLLIN;
    wake_event.data.u64 = UINT64_C(0x57414b45544f4b4e);

    errno = 0;
#ifdef _WIN32
    struct epoll_event wake_output;
    if (wepoll_ex_wake_event(epfd, &wake_event) != 0 ||
        epoll_wait(epfd, &wake_output, 1, 1000) != 1 ||
        wake_output.events != wake_event.events ||
        wake_output.data.u64 != wake_event.data.u64 ||
        epoll_rearm_classes(epfd, EPOLL_FD_INVALID,
                            WEPOLL_EX_REARM_ALL) != -1 ||
        errno != EBADF) {
        (void)wepoll_close(epfd);
        return 4;
    }
#else
    if (wepoll_ex_wake_event(epfd, &wake_event) != -1 ||
        errno != EOPNOTSUPP ||
        epoll_rearm_classes(epfd, EPOLL_FD_INVALID,
                            WEPOLL_EX_REARM_ALL) != -1 ||
        errno != EOPNOTSUPP) {
        (void)wepoll_close(epfd);
        return 4;
    }
#endif

    return wepoll_close(epfd) == 0 ? 0 : 5;
}
