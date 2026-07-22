#include "wepoll_ex.h"

int main(void)
{
    if (wepoll_ex_version() == 0 || wepoll_ex_version_string() == 0) {
        return 1;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        return 2;
    }

    return wepoll_close(epfd) == 0 ? 0 : 3;
}
