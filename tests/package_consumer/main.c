#include "wepoll_ex.h"

#include <string.h>

int main(void)
{
    static const char version_prefix[] = "wepoll-ex 0.1.0 ";
    const char *version = wepoll_ex_version_string();
    if (wepoll_ex_version() != UINT32_C(0x00000100) || version == 0 ||
        strncmp(version, version_prefix, sizeof(version_prefix) - 1) != 0) {
        return 1;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        return 2;
    }

    return wepoll_close(epfd) == 0 ? 0 : 3;
}
