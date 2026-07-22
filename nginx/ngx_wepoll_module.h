/*
 * ngx_wepoll_module.h — nginx event module driver header.
 *
 * Add this directory with nginx's --add-module option.  The tracked
 * `config` hook compiles the adapter and the static wepoll-ex sources.  For
 * a Win32 build, configure nginx with
 *
 *     ./configure --crossbuild=win32 --add-module=path/to/wepoll-ex/nginx
 *
 * and select it explicitly with `events { use wepoll; }`.  The module uses
 * nginx's level-triggered action contract; EPOLLET is intentionally not used.
 *
 * Event data stores nginx's connection pointer plus its instance bit, matching
 * the stale-event guard used by nginx's native event modules.
 */
#ifndef NGX_WEPOLL_MODULE_H_
#define NGX_WEPOLL_MODULE_H_

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

extern ngx_module_t  ngx_wepoll_module;

#endif
