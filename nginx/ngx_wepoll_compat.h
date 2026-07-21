/*
 * ngx_wepoll_compat.h — minimal typedefs so the wepoll-ex nginx
 * module compiles even outside the nginx source tree.
 *
 * In real builds this file is NOT used — the nginx build system
 * provides <ngx_config.h>, <ngx_core.h>, <ngx_event.h> and the
 * real ngx_event_module_t type.  We keep this shim only for IDE
 * indexing and for unit-test harnesses that compile the module
 * against stubs.
 */
#ifndef NGX_WEPOLL_COMPAT_H_
#define NGX_WEPOLL_COMPAT_H_

#include <stdint.h>
#include <stddef.h>

/* ----- minimal stand-ins for nginx types ----- */

typedef intptr_t            ngx_int_t;
typedef uintptr_t           ngx_uint_t;
typedef uintptr_t           ngx_msec_t;
typedef int                 ngx_socket_t;
typedef struct ngx_cycle_s  ngx_cycle_t;
typedef struct ngx_event_s  ngx_event_t;
typedef struct ngx_connection_s ngx_connection_t;
typedef struct ngx_log_s    ngx_log_t;
typedef struct ngx_module_s ngx_module_t;

struct ngx_cycle_s { void *conf_ctx; ngx_log_t *log; ngx_uint_t connection_n; };
struct ngx_log_s   { int dummy; };

#define NGX_OK          0
#define NGX_ERROR      -1
#define NGX_AGAIN      -2
#define NGX_DECLINED   -5

#define NGX_LOG_EMERG    0
#define NGX_LOG_ALERT    1
#define NGX_LOG_ERR      3
#define NGX_LOG_DEBUG_EVENT 8

#define NGX_READ_EVENT   1
#define NGX_WRITE_EVENT  2

#define NGX_CLEAR_EVENT   0x400
#define NGX_ONESHOT_EVENT 0x800
#define NGX_CLOSE_EVENT   0x1000

#define NGX_TIMER_INFINITE  ((ngx_msec_t) -1)

#define NGX_EVENT_MODULE    0x544E5645  /* "EVNT" */
#define NGX_EVENT_CONF      0x02000000
#define NGX_CONF_TAKE1      0x00010000

#define NGX_MODULE_V1       0, 0, NULL, 0, 0, 0, 0, NULL, 0, NULL, 0, 0
#define NGX_MODULE_V1_PADDING  0, 0, 0, 0, 0, 0, 0, 0

#define ngx_string(s)     { sizeof(s)-1, (u_char *) s }
#define ngx_null_command  { 0, 0, NULL, 0, 0, NULL }
#define ngx_errno         errno
#define ngx_memzero(p,n)  memset(p,0,n)
#define ngx_alloc(n,log)  malloc(n)
#define ngx_free(p)       free(p)
#define ngx_palloc(p,n)   malloc(n)

#define ngx_log_error(level, log, err, ...)  do {} while(0)
#define ngx_log_debug1(level, log, err, ...) do {} while(0)
#define ngx_event_get_conf(cc, mod)         NULL
#define ngx_conf_set_num_slot               NULL

#endif
