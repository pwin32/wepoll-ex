/*
 * ngx_wepoll_module.h — nginx event module driver header.
 *
 * Add this directory with nginx's --add-module option.  The tracked
 * `config` hook compiles the adapter and the static wepoll-ex sources.  For
 * a Win32 build, configure nginx with
 *
 *     ./configure --crossbuild=win32 --add-module=path/to/wepoll-ex/nginx
 *
 * and select it explicitly with `events { use wepoll; }`.  Level-triggered
 * operation remains the default.  `wepoll_edge on` opts into the separately
 * qualified explicit-rearm EPOLLET contract, and
 * `wepoll_edge_post_events on` forces posted dispatch for qualification.
 * `wepoll_close_audit on` (the default) reports registration and ownership
 * queue state before port close and lifecycle quarantine state afterward.
 *
 * Event data stores nginx's connection pointer plus its instance bit.  Edge
 * mode additionally stores stable per-connection adapter state in user_ctx so
 * queued records can be rejected before a recycled connection is dereferenced.
 */
#ifndef NGX_WEPOLL_MODULE_H_
#define NGX_WEPOLL_MODULE_H_

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

extern ngx_module_t  ngx_wepoll_module;

#endif
