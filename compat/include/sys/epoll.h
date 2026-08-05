/* Optional Windows source-compatibility facade for Linux-style includes.
 * Consume it through the wepoll_ex::epoll_compat CMake target so this isolated
 * include root is searched only by projects that explicitly opt in. */
#ifndef WEPOLL_EX_COMPAT_SYS_EPOLL_H_
#define WEPOLL_EX_COMPAT_SYS_EPOLL_H_

#ifndef _WIN32
#  error "wepoll-ex's <sys/epoll.h> compatibility facade is Windows-only"
#endif

#include <wepoll_ex.h>

#endif /* WEPOLL_EX_COMPAT_SYS_EPOLL_H_ */
