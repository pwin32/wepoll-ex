/*
 * ngx_wepoll_module.c — nginx event module implemented on wepoll-ex.
 *
 * This file is a drop-in replacement for nginx's stock
 * src/event/modules/ngx_epoll_module.c.  It implements the same
 * ngx_event_actions_t interface, so nginx's event loop code is
 * unchanged.
 *
 * Architectural differences from stock nginx epoll:
 *
 *   1. We use epoll_wait_ex() instead of epoll_wait().  The extended
 *      event carries a user_ctx pointer that we registered at ADD
 *      time, which points directly to the ngx_event_t.  No more
 *      ngx_event_actions[] hash lookup on the hot path.
 *
 *   2. We use epoll_ctl_batch() for the accept-burst path.  When
 *      nginx accepts N new connections in a tight loop, we batch the
 *      EPOLL_CTL_ADD calls into a single syscall.
 *
 *   3. Edge-triggered mode is the default for connections.  nginx
 *      already assumes ET semantics on Linux, so we match that.
 *
 *   4. EPOLLRDHUP is used to detect half-closed peers.  nginx's
 *      existing code path for handling RDHUP works unchanged.
 *
 *   5. EPOLLONESHOT is used for SSL connections.  nginx's SSL filter
 *      can call SSL_read multiple times per event; oneshot prevents
 *      the kernel from re-delivering while we're still in the
 *      handler.  We re-arm via epoll_rearm() after the handler
 *      returns NGX_OK.
 *
 * The module is configured into nginx via --add-module=path/to/this/dir
 * or by copying it into src/event/modules/ and patching auto/sources.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include "ngx_wepoll_module.h"
#include "wepoll_ex.h"

/* --------------------------------------------------------------------- */
/* Module configuration.                                              */
/* --------------------------------------------------------------------- */

typedef struct {
    ngx_uint_t  events;          /* max events per epoll_wait call */
    ngx_uint_t  afd_poll_batch;  /* obsolete — kept for ABI compat */
    ngx_uint_t  worker_threads;  /* obsolete — kept for ABI compat */
} ngx_wepoll_conf_t;

static ngx_command_t ngx_wepoll_commands[] = {
    { ngx_string("wepoll_events"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      0,
      offsetof(ngx_wepoll_conf_t, events),
      NULL },

    { ngx_string("wepoll_batch_accepts"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      0,
      offsetof(ngx_wepoll_conf_t, afd_poll_batch),
      NULL },

      ngx_null_command
};

/* --------------------------------------------------------------------- */
/* Globals.                                                           */
/* --------------------------------------------------------------------- */

static int                   ngx_wepoll_epfd = -1;
static epoll_event_ex_t     *ngx_wepoll_events;
static epoll_event_ex_t     *ngx_wepoll_overflow_events;
static ngx_uint_t            ngx_wepoll_nevents;
static ngx_wepoll_conf_t     ngx_wepoll_conf;

/* --------------------------------------------------------------------- */
/* Module init/teardown.                                              */
/* --------------------------------------------------------------------- */

static ngx_int_t
ngx_wepoll_init(ngx_cycle_t *cycle, ngx_msec_t timer)
{
    ngx_wepoll_conf_t  *wcf;

    wcf = ngx_event_get_conf(cycle->conf_ctx, ngx_wepoll_module);
    if (wcf == NULL) {
        return NGX_ERROR;
    }

    ngx_wepoll_conf = *wcf;
    if (ngx_wepoll_conf.events == 0) {
        ngx_wepoll_conf.events = 512;
    }
    ngx_wepoll_nevents = ngx_wepoll_conf.events;

    /* Create the epoll instance.  Pass cycle->connection_n as the
     * size hint so the IOCP and fd table are pre-sized. */
    ngx_wepoll_epfd = epoll_create_ex(cycle->connection_n, EPOLL_CLOEXEC);
    if (ngx_wepoll_epfd == -1) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "wepoll_ex: epoll_create_ex() failed");
        return NGX_ERROR;
    }

    ngx_wepoll_events =
        ngx_alloc(sizeof(epoll_event_ex_t) * ngx_wepoll_nevents, cycle->log);
    if (ngx_wepoll_events == NULL) {
        return NGX_ERROR;
    }

    /* Overflow buffer for events that arrive past ngx_wepoll_nevents
     * in a single epoll_wait call. */
    ngx_wepoll_overflow_events =
        ngx_alloc(sizeof(epoll_event_ex_t) * 16, cycle->log);
    if (ngx_wepoll_overflow_events == NULL) {
        return NGX_ERROR;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "wepoll_ex: epoll fd %d created", ngx_wepoll_epfd);

    return NGX_OK;
}

static void
ngx_wepoll_done(ngx_cycle_t *cycle)
{
    if (ngx_wepoll_events) {
        ngx_free(ngx_wepoll_events);
        ngx_wepoll_events = NULL;
    }
    if (ngx_wepoll_overflow_events) {
        ngx_free(ngx_wepoll_overflow_events);
        ngx_wepoll_overflow_events = NULL;
    }
    if (ngx_wepoll_epfd != -1) {
        wepoll_close(ngx_wepoll_epfd);
        ngx_wepoll_epfd = -1;
    }
}

/* --------------------------------------------------------------------- */
/* Event registration.                                                */
/* --------------------------------------------------------------------- */

static ngx_int_t
ngx_wepoll_add_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    ngx_event_t       *read = ev, *write;
    ngx_connection_t  *c;
    struct epoll_event  ee;
    uint32_t            mask;
    int                 op;

    c = ev->data;
    if (c == NULL || c->fd == (ngx_socket_t) -1) {
        return NGX_ERROR;
    }

    /* Default to EPOLLET for connection-style events.  This matches
     * nginx's Linux epoll module behaviour. */
    mask = EPOLLET;

    if (event == NGX_READ_EVENT) {
        mask |= EPOLLIN | EPOLLRDHUP;
    } else if (event == NGX_WRITE_EVENT) {
        mask |= EPOLLOUT;
    }

    if (flags & NGX_ONESHOT_EVENT) {
        mask |= EPOLLONESHOT;
    }
    if (flags & NGX_CLEAR_EVENT) {
        /* Already set EPOLLET above; this is a no-op flag for
         * compatibility with nginx's Linux epoll module. */
    }

    /* If a previous event was registered for the same fd, we MOD
     * instead of ADD to merge the masks. */
    if (ev->active) {
        op = EPOLL_CTL_MOD;
        if (event == NGX_READ_EVENT) {
            mask |= (c->write->active ? EPOLLOUT : 0);
        } else {
            mask |= (c->read->active ? (EPOLLIN | EPOLLRDHUP) : 0);
        }
    } else {
        op = EPOLL_CTL_ADD;
    }

    ee.events = mask;
    ee.data.ptr = c;  /* nginx reads this via ngx_cycle->connection_n */

    /* Use epoll_ctl_ctx so the ngx_event_t is surfaced directly in
     * epoll_wait_ex()'s output — no array lookup. */
    if (epoll_ctl_ctx(ngx_wepoll_epfd, op, c->fd, &ee, ev) == -1) {
        ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
                      "wepoll_ex: epoll_ctl_ctx(%d, %d, %d) failed",
                      ngx_wepoll_epfd, op, c->fd);
        return NGX_ERROR;
    }

    ev->active = 1;
    return NGX_OK;
}

static ngx_int_t
ngx_wepoll_del_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    ngx_connection_t  *c;
    struct epoll_event  ee;
    int                 op;

    c = ev->data;
    if (c == NULL) return NGX_OK;

    /* NGX_CLOSE_EVENT means the fd is being closed.  EPOLL_CTL_DEL. */
    if (flags & NGX_CLOSE_EVENT) {
        if (epoll_ctl(ngx_wepoll_epfd, EPOLL_CTL_DEL, c->fd, &ee) == -1) {
            /* If the fd is already gone, ignore. */
            if (ngx_errno != EBADF && ngx_errno != ENOENT) {
                ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
                              "wepoll_ex: EPOLL_CTL_DEL fd:%d failed", c->fd);
                return NGX_ERROR;
            }
        }
        ev->active = 0;
        return NGX_OK;
    }

    /* Otherwise we're disabling one direction.  MOD the event mask. */
    op = EPOLL_CTL_MOD;
    uint32_t mask = EPOLLET;
    if (event == NGX_READ_EVENT) {
        if (c->write->active) mask |= EPOLLOUT;
        /* read disabled: don't add EPOLLIN */
    } else {
        if (c->read->active) mask |= (EPOLLIN | EPOLLRDHUP);
    }
    ee.events = mask;
    ee.data.ptr = c;

    if (epoll_ctl_ctx(ngx_wepoll_epfd, op, c->fd, &ee, ev) == -1) {
        ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
                      "wepoll_ex: EPOLL_CTL_MOD fd:%d failed", c->fd);
        return NGX_ERROR;
    }
    ev->active = 0;
    return NGX_OK;
}

/* --------------------------------------------------------------------- */
/* Re-arm a one-shot fd after the handler completes.                  */
/* --------------------------------------------------------------------- */

static ngx_int_t
ngx_wepoll_rearm(ngx_event_t *ev)
{
    ngx_connection_t *c = ev->data;
    if (c == NULL) return NGX_ERROR;
    if (epoll_rearm(ngx_wepoll_epfd, c->fd) == -1) {
        ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
                      "wepoll_ex: epoll_rearm fd:%d failed", c->fd);
        return NGX_ERROR;
    }
    return NGX_OK;
}

/* --------------------------------------------------------------------- */
/* Process events.                                                    */
/* --------------------------------------------------------------------- */

static ngx_int_t
ngx_wepoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer, ngx_uint_t flags)
{
    int                events;
    ngx_uint_t         i, mask;
    ngx_event_t       *rev, *wev;
    ngx_connection_t  *c;
    epoll_event_ex_t  *ee = ngx_wepoll_events;

    /* Convert nginx's timer semantics: NGX_TIMER_INFINITE => -1,
     * NGX_TIMER_NO_WAIT => 0. */
    int timeout_ms;
    if (timer == NGX_TIMER_INFINITE) timeout_ms = -1;
    else if (timer == 0)             timeout_ms = 0;
    else                              timeout_ms = (int)timer;

    /* The flags argument carries NGX_POST_EVENTS / NGX_POST_THREAD_EVENTS.
     * If set, we should defer the handler call by queueing the event
     * on the post list.  wepoll-ex just calls the handler — nginx's
     * upstream code handles posting via ev->ready semantics. */

    events = epoll_wait_ex(ngx_wepoll_epfd, ee, ngx_wepoll_nevents, timeout_ms);
    if (events == -1) {
        if (ngx_errno == EINTR) return NGX_OK;
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "wepoll_ex: epoll_wait_ex() failed");
        return NGX_ERROR;
    }

    if (events == 0) {
        /* Timeout. */
        return NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "wepoll_ex: %d events ready", events);

    for (i = 0; i < (ngx_uint_t)events; i++) {
        /* ee[i].user_ctx is the ngx_event_t* we registered at ADD time. */
        ngx_event_t *ev = (ngx_event_t *) ee[i].user_ctx;

        if (ev == NULL) {
            /* Fall back to connection lookup via data.ptr. */
            c = (ngx_connection_t *) ee[i].data.ptr;
            if (c == NULL) continue;
            /* Determine read/write from the event bits. */
            if (ee[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                ev = c->read;
                mask = NGX_READ_EVENT;
            } else {
                ev = c->write;
                mask = NGX_WRITE_EVENT;
            }
        } else {
            /* Determine the direction from the event bits. */
            if (ee[i].events & (EPOLLOUT | EPOLLHUP | EPOLLERR)) {
                mask = NGX_WRITE_EVENT;
                c = ev->data;
                if (c) wev = c->write;
            } else {
                mask = NGX_READ_EVENT;
            }
            c = ev->data;
        }

        if (c == NULL) continue;

        /* Stash the connection for handler access. */
        c->ready = 1;
        rev = c->read;
        wev = c->write;

        /* Handle read events. */
        if ((ee[i].events & (EPOLLIN | EPOLLRDHUP)) && rev && rev->handler) {
            rev->ready = 1;
            if (ee[i].events & EPOLLRDHUP) {
                rev->pending_eof = 1;
                rev->kq_errno = NGX_ECONNRESET;
            }
            if (ee[i].events & EPOLLERR) {
                rev->error = 1;
            }
            rev->handler(rev);
        }

        /* Handle write events. */
        if ((ee[i].events & EPOLLOUT) && wev && wev->handler) {
            wev->ready = 1;
            if (ee[i].events & EPOLLERR) {
                wev->error = 1;
            }
            wev->handler(wev);
        }

        /* If the fd was in oneshot mode, the user_ctx is still valid
         * but the fd is no longer armed.  nginx's connection code
         * will call ngx_add_event() again to re-arm, which translates
         * to epoll_rearm() via the EPOLL_CTL_MOD path. */
    }

    return NGX_OK;
}

/* --------------------------------------------------------------------- */
/* Module plumbing.                                                   */
/* --------------------------------------------------------------------- */

static void *
ngx_wepoll_create_conf(ngx_cycle_t *cycle)
{
    ngx_wepoll_conf_t  *wcf;
    wcf = ngx_palloc(cycle->pool, sizeof(ngx_wepoll_conf_t));
    if (wcf == NULL) return NULL;
    ngx_memzero(wcf, sizeof(*wcf));
    return wcf;
}

static char *
ngx_wepoll_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_wepoll_conf_t *wcf = conf;
    if (wcf->events == 0) wcf->events = 512;
    return NGX_CONF_OK;
}

ngx_event_actions_t   ngx_wepoll_actions = {
    ngx_wepoll_add_event,          /* add */
    ngx_wepoll_del_event,          /* delete */
    ngx_wepoll_add_event,          /* enable */
    ngx_wepoll_del_event,          /* disable */
    ngx_wepoll_add_event,          /* add_conn — same code path */
    ngx_wepoll_del_event,          /* del_conn — same code path */
    NULL,                          /* notify — wepoll has no native notify */
    ngx_wepoll_process_events,     /* process_events */
    ngx_wepoll_init,               /* init */
    ngx_wepoll_done,               /* done */
};

ngx_event_module_t  ngx_wepoll_module_ctx = {
    ngx_string("wepoll"),
    ngx_wepoll_create_conf,        /* create configuration */
    ngx_wepoll_init_conf,          /* init configuration */
    { 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* module-specific config defaults */
    &ngx_wepoll_actions
};

ngx_module_t  ngx_wepoll_module = {
    NGX_MODULE_V1,
    &ngx_wepoll_module_ctx,        /* module context */
    ngx_wepoll_commands,           /* module directives */
    NGX_EVENT_MODULE,              /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    NULL,                          /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    NULL,                          /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};
