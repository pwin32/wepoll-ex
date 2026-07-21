/*
 * ngx_wepoll_module.h — nginx event module driver header.
 *
 * Drop this file into nginx's src/event/modules/ directory and the
 * corresponding .c file alongside it.  Configure nginx with
 *
 *     ./auto/configure --with-cc-opt=-DWEPOLL_EX_NGINX \
 *                      --add-module=path/to/wepoll-ex/nginx
 *
 * to enable the wepoll-ex backend in place of the default select/poll
 * modules on Windows.  The module implements the ngx_event_module_t
 * interface so the rest of nginx sees it as just another event
 * backend.
 *
 * The module transparently uses epoll_ctl_ctx() to register the
 * ngx_event_t pointer as user_ctx on each fd, eliminating the
 * traditional ngx_event_t** lookup array that nginx has to maintain
 * on Linux.  When epoll_wait_ex returns, the user_ctx field already
 * points at the ngx_event_t — no hash, no array.
 */
#ifndef NGX_WEPOLL_MODULE_H_
#define NGX_WEPOLL_MODULE_H_

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

ngx_module_t         ngx_wepoll_module;
ngx_event_module_t   ngx_wepoll_module_ctx;

#endif
