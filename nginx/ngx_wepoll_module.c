/*
 * ngx_wepoll_module.c — experimental nginx 1.31.3 event-module adapter.
 *
 * This adapter deliberately follows nginx's level-triggered poll module
 * contract.  The Windows wepoll-ex backend supports observed-readiness
 * EPOLLET, but this adapter has no separately tested nginx drain/rearm state
 * machine, so copying the Linux epoll module's edge-triggered mask would be
 * incorrect.  One virtual epoll registration is maintained per connection;
 * read/write actions merge their masks with ADD/MOD and use nginx's instance
 * bit to reject stale queued events.
 *
 * The tracked nginx/config hook builds this file and the static wepoll-ex
 * sources into a Win32 nginx tree.  The integration remains experimental.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

#include <errno.h>
#include <limits.h>

#include "ngx_wepoll_module.h"
#include "wepoll_ex.h"

typedef struct {
    ngx_uint_t  events;
} ngx_wepoll_conf_t;

static ngx_int_t ngx_wepoll_add_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags);
static ngx_int_t ngx_wepoll_del_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags);
static ngx_int_t ngx_wepoll_process_events(ngx_cycle_t *cycle,
    ngx_msec_t timer, ngx_uint_t flags);
static ngx_int_t ngx_wepoll_init(ngx_cycle_t *cycle, ngx_msec_t timer);
static void ngx_wepoll_done(ngx_cycle_t *cycle);
static void *ngx_wepoll_create_conf(ngx_cycle_t *cycle);
static char *ngx_wepoll_init_conf(ngx_cycle_t *cycle, void *conf);

static ngx_conf_num_bounds_t ngx_wepoll_events_bounds = {
    ngx_conf_check_num_bounds,
    1,
    INT_MAX
};

static ngx_command_t ngx_wepoll_commands[] = {

    { ngx_string("wepoll_events"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      0,
      offsetof(ngx_wepoll_conf_t, events),
      &ngx_wepoll_events_bounds },

      ngx_null_command
};

static ngx_str_t ngx_wepoll_name = ngx_string("wepoll");

static int              ngx_wepoll_epfd = -1;
static epoll_event_ex  *ngx_wepoll_events;
static ngx_uint_t       ngx_wepoll_nevents;

static ngx_event_module_t ngx_wepoll_module_ctx = {
    &ngx_wepoll_name,
    ngx_wepoll_create_conf,
    ngx_wepoll_init_conf,

    {
        ngx_wepoll_add_event,
        ngx_wepoll_del_event,
        ngx_wepoll_add_event,
        ngx_wepoll_del_event,
        NULL,
        NULL,
        NULL,
        ngx_wepoll_process_events,
        ngx_wepoll_init,
        ngx_wepoll_done
    }
};

ngx_module_t ngx_wepoll_module = {
    NGX_MODULE_V1,
    &ngx_wepoll_module_ctx,
    ngx_wepoll_commands,
    NGX_EVENT_MODULE,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NGX_MODULE_V1_PADDING
};

static void
ngx_wepoll_log_errno(ngx_log_t *log, const char *operation, ngx_socket_t fd)
{
    /* wepoll-ex reports portable errno values while nginx/Win32's ngx_errno
     * reads GetLastError().  Log the portable value explicitly instead of
     * presenting a stale Win32 error code. */
    ngx_log_error(NGX_LOG_ALERT, log, 0,
                  "wepoll_ex: %s fd:%d failed (errno:%d)",
                  operation, fd, errno);
}

static ngx_uint_t
ngx_wepoll_mask_for_event(ngx_int_t event)
{
    if (event == NGX_READ_EVENT) {
        return EPOLLIN | EPOLLRDHUP;
    }
    return EPOLLOUT;
}

static void *
ngx_wepoll_event_data(ngx_connection_t *c, ngx_event_t *ev)
{
    return (void *) ((uintptr_t) c | (uintptr_t) ev->instance);
}

static ngx_int_t
ngx_wepoll_add_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    ngx_event_t      *other;
    ngx_connection_t *c;
    struct epoll_event ee;
    ngx_uint_t         mask;
    int                op;

    c = ev->data;
    if (c == NULL || c->fd == (ngx_socket_t) -1) {
        return NGX_ERROR;
    }

    other = (event == NGX_READ_EVENT) ? c->write : c->read;
    mask = ngx_wepoll_mask_for_event(event);
    if (other != NULL && other->active) {
        mask |= ngx_wepoll_mask_for_event(
            event == NGX_READ_EVENT ? NGX_WRITE_EVENT : NGX_READ_EVENT);
        op = EPOLL_CTL_MOD;
    } else {
        op = EPOLL_CTL_ADD;
    }

    /* The current Windows backend is level-triggered.  Ignore
     * NGX_CLEAR_EVENT/NGX_ONESHOT_EVENT rather than passing incompatible
     * nginx-internal flag values to the public epoll API. */
    (void) flags;
    ee.events = mask;
    ee.data.ptr = ngx_wepoll_event_data(c, ev);

    if (epoll_ctl(ngx_wepoll_epfd, op, c->fd, &ee) == -1) {
        ngx_wepoll_log_errno(ev->log, "epoll_ctl(ADD/MOD)", c->fd);
        return NGX_ERROR;
    }

    ev->active = 1;
    return NGX_OK;
}

static ngx_int_t
ngx_wepoll_del_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    ngx_event_t      *other;
    ngx_connection_t *c;
    struct epoll_event ee;
    ngx_uint_t         mask;
    int                op;

    c = ev->data;
    if (c == NULL) {
        ev->active = 0;
        return NGX_OK;
    }

    /* DEL is safe and useful even before closesocket(); it also bounds the
     * deferred AFD cancellation state during nginx connection teardown. */
    other = (event == NGX_READ_EVENT) ? c->write : c->read;
    if (flags & NGX_CLOSE_EVENT) {
        ngx_int_t close_result = NGX_OK;

        if (ngx_wepoll_epfd != -1 && c->fd != (ngx_socket_t) -1) {
            if (epoll_ctl(ngx_wepoll_epfd, EPOLL_CTL_DEL, c->fd, NULL) == -1 &&
                errno != EBADF && errno != ENOENT && errno != ENOTSOCK) {
                ngx_wepoll_log_errno(ev->log, "epoll_ctl(DEL close)", c->fd);
                close_result = NGX_ERROR;
            }
        }
        ev->active = 0;
        if (other != NULL) {
            other->active = 0;
        }
        return close_result;
    }

    if (other != NULL && other->active) {
        op = EPOLL_CTL_MOD;
        mask = ngx_wepoll_mask_for_event(
            event == NGX_READ_EVENT ? NGX_WRITE_EVENT : NGX_READ_EVENT);
        ee.events = mask;
        ee.data.ptr = ngx_wepoll_event_data(c, other);
    } else {
        op = EPOLL_CTL_DEL;
        ee.events = 0;
        ee.data.ptr = NULL;
    }

    (void) flags;
    if (epoll_ctl(ngx_wepoll_epfd, op, c->fd,
                  op == EPOLL_CTL_DEL ? NULL : &ee) == -1) {
        if (errno == EBADF || errno == ENOENT || errno == ENOTSOCK) {
            /* The socket or its registration disappeared independently.
             * Keep nginx's active flags consistent with the port state. */
            ev->active = 0;
            if (other != NULL) {
                other->active = 0;
            }
            return NGX_OK;
        }
        ngx_wepoll_log_errno(ev->log, "epoll_ctl(MOD/DEL)", c->fd);
        return NGX_ERROR;
    }

    ev->active = 0;
    return NGX_OK;
}

static ngx_int_t
ngx_wepoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer,
    ngx_uint_t flags)
{
    int                timeout;
    int                events;
    ngx_int_t          i;
    ngx_err_t          err;
    ngx_event_t       *rev, *wev;
    ngx_connection_t  *c;
    ngx_queue_t        *queue;
    uintptr_t           instance;
    uint32_t            revents;

    if (timer == NGX_TIMER_INFINITE) {
        timeout = -1;
    } else if (timer > (ngx_msec_t) INT_MAX) {
        timeout = INT_MAX;
    } else {
        timeout = (int) timer;
    }

    events = epoll_wait_ex(ngx_wepoll_epfd, ngx_wepoll_events,
                           (int) ngx_wepoll_nevents, timeout);
    err = (events == -1) ? errno : 0;

    if ((flags & NGX_UPDATE_TIME) || ngx_event_timer_alarm) {
        ngx_time_update();
    }

    if (err != 0) {
        if (err == EINTR) {
            if (ngx_event_timer_alarm) {
                ngx_event_timer_alarm = 0;
            }
            return NGX_OK;
        }
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "wepoll_ex: epoll_wait_ex() failed (errno:%d)", err);
        return NGX_ERROR;
    }

    if (events == 0) {
        return NGX_OK;
    }

    for (i = 0; i < events; i++) {
        revents = ngx_wepoll_events[i].events;
        c = (ngx_connection_t *) ngx_wepoll_events[i].data.ptr;
        instance = (uintptr_t) c & (uintptr_t) 1;
        c = (ngx_connection_t *) ((uintptr_t) c & ~(uintptr_t) 1);

        if (c == NULL || c->fd == (ngx_socket_t) -1 || c->read == NULL ||
            c->read->instance != instance) {
            continue;
        }

        if (revents & (EPOLLERR | EPOLLHUP)) {
            revents |= EPOLLIN | EPOLLOUT;
        }

        rev = c->read;
        if ((revents & EPOLLIN) && rev->active) {
            rev->ready = 1;
            rev->available = -1;
            if (revents & EPOLLRDHUP) {
                rev->pending_eof = 1;
            }

            if (flags & NGX_POST_EVENTS) {
                queue = rev->accept ? &ngx_posted_accept_events
                                    : &ngx_posted_events;
                ngx_post_event(rev, queue);
            } else if (rev->handler != NULL) {
                rev->handler(rev);
            }
        }

        wev = c->write;
        if ((revents & EPOLLOUT) && wev != NULL && wev->active) {
            if (c->fd == (ngx_socket_t) -1 || wev->instance != instance) {
                continue;
            }
            wev->ready = 1;
#if (NGX_THREADS)
            wev->complete = 1;
#endif
            if (flags & NGX_POST_EVENTS) {
                ngx_post_event(wev, &ngx_posted_events);
            } else if (wev->handler != NULL) {
                wev->handler(wev);
            }
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_wepoll_init(ngx_cycle_t *cycle, ngx_msec_t timer)
{
    ngx_wepoll_conf_t *wcf;
    int                size_hint;

    (void) timer;
    wcf = ngx_event_get_conf(cycle->conf_ctx, ngx_wepoll_module);
    if (wcf == NULL) {
        return NGX_ERROR;
    }

    if (wcf->events == 0 ||
        wcf->events > (ngx_uint_t) INT_MAX ||
        wcf->events > (ngx_uint_t) ((size_t) -1 / sizeof(*ngx_wepoll_events))) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "wepoll_events must fit an epoll_wait() event array");
        return NGX_ERROR;
    }

    if (ngx_wepoll_epfd == -1) {
        size_hint = cycle->connection_n > (ngx_uint_t) INT_MAX
                    ? INT_MAX : (int) cycle->connection_n;
        ngx_wepoll_epfd = epoll_create_ex(size_hint, EPOLL_CLOEXEC);
        if (ngx_wepoll_epfd == -1) {
            ngx_wepoll_log_errno(cycle->log, "epoll_create_ex", -1);
            return NGX_ERROR;
        }
    }

    if (ngx_wepoll_nevents < wcf->events) {
        epoll_event_ex *events;

        events = ngx_alloc(sizeof(*events) * wcf->events, cycle->log);
        if (events == NULL) {
            return NGX_ERROR;
        }
        if (ngx_wepoll_events != NULL) {
            ngx_free(ngx_wepoll_events);
        }
        ngx_wepoll_events = events;
    }
    ngx_wepoll_nevents = wcf->events;

    ngx_io = ngx_os_io;
    ngx_event_actions = ngx_wepoll_module_ctx.actions;
    ngx_event_flags = NGX_USE_LEVEL_EVENT;

    return NGX_OK;
}

static void
ngx_wepoll_done(ngx_cycle_t *cycle)
{
    if (ngx_wepoll_epfd != -1) {
        if (wepoll_close(ngx_wepoll_epfd) == -1) {
            ngx_wepoll_log_errno(cycle->log, "wepoll_close", -1);
        }
        ngx_wepoll_epfd = -1;
    }
    if (ngx_wepoll_events != NULL) {
        ngx_free(ngx_wepoll_events);
        ngx_wepoll_events = NULL;
    }
    ngx_wepoll_nevents = 0;
}

static void *
ngx_wepoll_create_conf(ngx_cycle_t *cycle)
{
    ngx_wepoll_conf_t *wcf;

    wcf = ngx_pcalloc(cycle->pool, sizeof(*wcf));
    if (wcf == NULL) {
        return NULL;
    }
    wcf->events = NGX_CONF_UNSET_UINT;
    return wcf;
}

static char *
ngx_wepoll_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_wepoll_conf_t *wcf = conf;

    (void) cycle;
    ngx_conf_init_uint_value(wcf->events, 512);
    return NGX_CONF_OK;
}
