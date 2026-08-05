#include <sys/epoll.h>

int main(void)
{
    int epfd = epoll_create1(EPOLL_CLOEXEC);

    if (epfd < 0) {
        return 1;
    }
    return wepoll_close(epfd) == 0 ? 0 : 2;
}
