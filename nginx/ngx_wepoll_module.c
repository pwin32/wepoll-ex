/*
 * ngx_wepoll_module.c — experimental nginx 1.31.3 event-module adapter.
 *
 * The default mode retains the qualified level-triggered adapter.  The
 * optional `wepoll_edge on` mode uses a WEPOLL_EX_CREATE_EXPLICIT_REARM port
 * and keeps one duplex EPOLLET registration per nginx connection.  Delivery
 * transfers ownership of each returned readiness class to nginx; the class is
 * rearmed only after its event is no longer posted and nginx has cleared the
 * event's `ready` bit.  This puts rearm after direct and posted handlers,
 * including the accept handler which does not call ngx_handle_read_event().
 *
 * Edge registrations are explicitly deleted before closesocket().  Event data
 * retains nginx's connection pointer and instance bit, while user_ctx points
 * at stable per-connection adapter state so stale queued records can be
 * rejected before dereferencing a recycled connection.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

#include <errno.h>
#include <limits.h>

#include "ngx_wepoll_module.h"
#include "wepoll_ex.h"

#define NGX_WEPOLL_EDGE_EVENTS                                           \
    (EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET)

typedef struct {
    ngx_uint_t  events;
    ngx_flag_t  edge;
    ngx_flag_t  edge_post_events;
    ngx_flag_t  close_audit;
} ngx_wepoll_conf_t;

typedef struct {
    ngx_connection_t  *connection;
    ngx_socket_t        fd;
    ngx_uint_t          instance;
    uint32_t            interests;
    uint32_t            disarmed;
    uint32_t            pending;
    unsigned            registered:1;
    unsigned            queued:1;
} ngx_wepoll_connection_state_t;

static ngx_int_t ngx_wepoll_add_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags);
static ngx_int_t ngx_wepoll_del_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags);
static ngx_int_t ngx_wepoll_add_connection(ngx_connection_t *c);
static ngx_int_t ngx_wepoll_del_connection(ngx_connection_t *c,
    ngx_uint_t flags);
static ngx_int_t ngx_wepoll_process_events(ngx_cycle_t *cycle,
    ngx_msec_t timer, ngx_uint_t flags);
static ngx_int_t ngx_wepoll_init(ngx_cycle_t *cycle, ngx_msec_t timer);
static void ngx_wepoll_done(ngx_cycle_t *cycle);
static void ngx_wepoll_exit_process(ngx_cycle_t *cycle);
static void *ngx_wepoll_create_conf(ngx_cycle_t *cycle);
static char *ngx_wepoll_init_conf(ngx_cycle_t *cycle, void *conf);

static ngx_int_t ngx_wepoll_level_add_event(ngx_event_t *ev,
    ngx_int_t event, ngx_uint_t flags);
static ngx_int_t ngx_wepoll_level_del_event(ngx_event_t *ev,
    ngx_int_t event, ngx_uint_t flags);
static ngx_int_t ngx_wepoll_edge_add_event(ngx_event_t *ev,
    ngx_int_t event);
static ngx_int_t ngx_wepoll_edge_del_event(ngx_event_t *ev,
    ngx_int_t event, ngx_uint_t flags);

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

    { ngx_string("wepoll_edge"),
      NGX_EVENT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_wepoll_conf_t, edge),
      NULL },

    { ngx_string("wepoll_edge_post_events"),
      NGX_EVENT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_wepoll_conf_t, edge_post_events),
      NULL },

    { ngx_string("wepoll_close_audit"),
      NGX_EVENT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_wepoll_conf_t, close_audit),
      NULL },

      ngx_null_command
};

static ngx_str_t ngx_wepoll_name = ngx_string("wepoll");

static int                             ngx_wepoll_epfd = -1;
static epoll_event_ex                 *ngx_wepoll_events;
static ngx_uint_t                      ngx_wepoll_nevents;
static ngx_cycle_t                    *ngx_wepoll_cycle;
static ngx_wepoll_connection_state_t  *ngx_wepoll_states;
static ngx_wepoll_connection_state_t **ngx_wepoll_pending_states;
static ngx_uint_t                      ngx_wepoll_state_count;
static ngx_uint_t                      ngx_wepoll_pending_count;
static ngx_uint_t                      ngx_wepoll_edge_enabled;
static ngx_uint_t                      ngx_wepoll_edge_post_events;
static ngx_uint_t                      ngx_wepoll_close_audit;
static ngx_uint_t                      ngx_wepoll_port_edge;
static ngx_uint_t                      ngx_wepoll_missing_edge_flag_logged;
static uint64_t                        ngx_wepoll_edge_read_deliveries;
static uint64_t                        ngx_wepoll_edge_write_deliveries;
static uint64_t                        ngx_wepoll_edge_terminal_deliveries;
static uint64_t                        ngx_wepoll_edge_read_rearms;
static uint64_t                        ngx_wepoll_edge_write_rearms;

static ngx_event_module_t ngx_wepoll_module_ctx = {
    &ngx_wepoll_name,
    ngx_wepoll_create_conf,
    ngx_wepoll_init_conf,

    {
        ngx_wepoll_add_event,
        ngx_wepoll_del_event,
        ngx_wepoll_add_event,
        ngx_wepoll_del_event,
        ngx_wepoll_add_connection,
        ngx_wepoll_del_connection,
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
    ngx_wepoll_exit_process,
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

static uint32_t
ngx_wepoll_class_for_event(ngx_int_t event)
{
    if (event == NGX_READ_EVENT) {
        return WEPOLL_EX_REARM_READ;
    }
    return WEPOLL_EX_REARM_WRITE;
}

static void *
ngx_wepoll_event_data(ngx_connection_t *c)
{
    return (void *) ((uintptr_t) c | (uintptr_t) c->read->instance);
}

static ngx_wepoll_connection_state_t *
ngx_wepoll_state_for_connection(ngx_connection_t *c)
{
    uintptr_t  address, base, offset;
    ngx_uint_t index;

    if (c == NULL || ngx_wepoll_cycle == NULL ||
        ngx_wepoll_cycle->connections == NULL || ngx_wepoll_states == NULL)
    {
        return NULL;
    }

    address = (uintptr_t) c;
    base = (uintptr_t) ngx_wepoll_cycle->connections;
    if (address < base) {
        return NULL;
    }

    offset = address - base;
    if (offset % sizeof(*c) != 0) {
        return NULL;
    }

    index = (ngx_uint_t) (offset / sizeof(*c));
    if (index >= ngx_wepoll_state_count) {
        return NULL;
    }

    return &ngx_wepoll_states[index];
}

static void
ngx_wepoll_reset_state(ngx_wepoll_connection_state_t *state,
    ngx_connection_t *c)
{
    unsigned queued;

    queued = state->queued;
    ngx_memzero(state, sizeof(*state));
    state->queued = queued;
    state->connection = c;
    state->fd = c->fd;
    state->instance = c->read->instance;
}

static ngx_wepoll_connection_state_t *
ngx_wepoll_prepare_state(ngx_connection_t *c)
{
    ngx_wepoll_connection_state_t *state;

    state = ngx_wepoll_state_for_connection(c);
    if (state == NULL) {
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "wepoll_ex: connection is outside the current cycle");
        return NULL;
    }

    if (state->registered) {
        if (state->connection != c || state->fd != c->fd ||
            state->instance != c->read->instance)
        {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "wepoll_ex: registered connection identity changed "
                          "without DEL fd:%d", c->fd);
            return NULL;
        }
        return state;
    }

    ngx_wepoll_reset_state(state, c);
    return state;
}

static ngx_uint_t
ngx_wepoll_state_is_current(ngx_wepoll_connection_state_t *state)
{
    ngx_connection_t *c;

    if (state == NULL || !state->registered) {
        return 0;
    }

    c = state->connection;
    return c != NULL && c->read != NULL && c->write != NULL &&
           c->fd != (ngx_socket_t) -1 && c->fd == state->fd &&
           c->read->instance == state->instance &&
           ngx_wepoll_state_for_connection(c) == state;
}

static void
ngx_wepoll_unbind_state(ngx_wepoll_connection_state_t *state)
{
    unsigned queued;

    if (state == NULL) {
        return;
    }

    queued = state->queued;
    ngx_memzero(state, sizeof(*state));
    state->queued = queued;
    state->fd = (ngx_socket_t) -1;
}

static ngx_int_t
ngx_wepoll_register_edge_connection(ngx_wepoll_connection_state_t *state,
    ngx_connection_t *c)
{
    struct epoll_event ee;

    ee.events = NGX_WEPOLL_EDGE_EVENTS;
    ee.data.ptr = ngx_wepoll_event_data(c);

    if (epoll_ctl_ctx(ngx_wepoll_epfd, EPOLL_CTL_ADD, c->fd, &ee, state)
        == -1)
    {
        ngx_wepoll_log_errno(c->log, "epoll_ctl_ctx(ADD edge)", c->fd);
        return NGX_ERROR;
    }

    state->registered = 1;
    state->disarmed = 0;
    state->pending = 0;
    return NGX_OK;
}

static void
ngx_wepoll_queue_pending_state(ngx_wepoll_connection_state_t *state)
{
    if (state->pending == 0 || state->queued) {
        return;
    }

    if (ngx_wepoll_pending_count >= ngx_wepoll_state_count) {
        ngx_log_error(NGX_LOG_ALERT, state->connection->log, 0,
                      "wepoll_ex: pending edge-state queue overflow");
        return;
    }

    ngx_wepoll_pending_states[ngx_wepoll_pending_count++] = state;
    state->queued = 1;
}

static ngx_int_t
ngx_wepoll_rearm_classes(ngx_wepoll_connection_state_t *state,
    uint32_t classes)
{
    ngx_connection_t *c;

    if (classes == 0) {
        return NGX_OK;
    }

    c = state->connection;
    if (epoll_rearm_classes(ngx_wepoll_epfd, state->fd, classes) == -1) {
        ngx_wepoll_log_errno(c->log, "epoll_rearm_classes", state->fd);
        return NGX_ERROR;
    }

    state->disarmed &= ~classes;
    state->pending &= ~classes;

    if (classes & WEPOLL_EX_REARM_READ) {
        ngx_wepoll_edge_read_rearms++;
        if (state->interests & WEPOLL_EX_REARM_READ) {
            c->read->active = 1;
        }
    }

    if (classes & WEPOLL_EX_REARM_WRITE) {
        ngx_wepoll_edge_write_rearms++;
        if (state->interests & WEPOLL_EX_REARM_WRITE) {
            c->write->active = 1;
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_wepoll_rearm_ready_state(ngx_wepoll_connection_state_t *state)
{
    uint32_t          classes;
    ngx_connection_t *c;
    ngx_event_t      *rev, *wev;

    if (!ngx_wepoll_state_is_current(state)) {
        state->pending = 0;
        return NGX_OK;
    }

    c = state->connection;
    rev = c->read;
    wev = c->write;
    classes = 0;

    if (state->pending & WEPOLL_EX_REARM_READ) {
        if (!(state->interests & WEPOLL_EX_REARM_READ) || rev->closed) {
            state->pending &= ~WEPOLL_EX_REARM_READ;
        } else if (!rev->posted && !rev->ready) {
            classes |= WEPOLL_EX_REARM_READ;
        }
    }

    if (state->pending & WEPOLL_EX_REARM_WRITE) {
        if (!(state->interests & WEPOLL_EX_REARM_WRITE) || wev->closed) {
            state->pending &= ~WEPOLL_EX_REARM_WRITE;
        } else if (!wev->posted && !wev->ready) {
            classes |= WEPOLL_EX_REARM_WRITE;
        }
    }

    return ngx_wepoll_rearm_classes(state, classes);
}

static ngx_int_t
ngx_wepoll_sweep_pending_states(void)
{
    ngx_int_t                      result;
    ngx_uint_t                     i, retained;
    ngx_wepoll_connection_state_t *state;

    result = NGX_OK;
    retained = 0;

    for (i = 0; i < ngx_wepoll_pending_count; i++) {
        state = ngx_wepoll_pending_states[i];
        if (ngx_wepoll_rearm_ready_state(state) != NGX_OK) {
            result = NGX_ERROR;
        }

        if (state->pending != 0 && ngx_wepoll_state_is_current(state)) {
            ngx_wepoll_pending_states[retained++] = state;
        } else {
            state->queued = 0;
        }
    }

    ngx_wepoll_pending_count = retained;
    return result;
}

static ngx_int_t
ngx_wepoll_edge_add_event(ngx_event_t *ev, ngx_int_t event)
{
    uint32_t                       class_mask, previous_interests;
    ngx_connection_t             *c;
    ngx_wepoll_connection_state_t *state;

    c = ev->data;
    if (c == NULL || c->read == NULL || c->write == NULL ||
        c->fd == (ngx_socket_t) -1)
    {
        return NGX_ERROR;
    }

    state = ngx_wepoll_prepare_state(c);
    if (state == NULL) {
        return NGX_ERROR;
    }

    if (!state->registered &&
        ngx_wepoll_register_edge_connection(state, c) != NGX_OK)
    {
        return NGX_ERROR;
    }

    class_mask = ngx_wepoll_class_for_event(event);
    previous_interests = state->interests;
    state->interests |= class_mask;

    if ((state->disarmed & class_mask) != 0 &&
        ngx_wepoll_rearm_classes(state, class_mask) != NGX_OK)
    {
        state->interests = previous_interests;
        return NGX_ERROR;
    }

    state->pending &= ~class_mask;
    ev->active = 1;
    return NGX_OK;
}

static ngx_int_t
ngx_wepoll_edge_del_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags)
{
    uint32_t                       class_mask;
    ngx_connection_t             *c;
    ngx_wepoll_connection_state_t *state;

    c = ev->data;
    if (c == NULL) {
        ev->active = 0;
        return NGX_OK;
    }

    if (flags & NGX_CLOSE_EVENT) {
        return ngx_wepoll_del_connection(c, flags);
    }

    state = ngx_wepoll_state_for_connection(c);
    class_mask = ngx_wepoll_class_for_event(event);
    if (state != NULL && state->registered && state->connection == c) {
        state->interests &= ~class_mask;
        state->pending &= ~class_mask;
    }

    ev->active = 0;
    return NGX_OK;
}

static ngx_int_t
ngx_wepoll_level_add_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags)
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

    /* Level mode ignores nginx's clear/oneshot flags rather than passing
     * nginx-internal values through the public epoll API. */
    (void) flags;
    ee.events = mask;
    ee.data.ptr = ngx_wepoll_event_data(c);

    if (epoll_ctl(ngx_wepoll_epfd, op, c->fd, &ee) == -1) {
        ngx_wepoll_log_errno(ev->log, "epoll_ctl(ADD/MOD)", c->fd);
        return NGX_ERROR;
    }

    ev->active = 1;
    return NGX_OK;
}

static ngx_int_t
ngx_wepoll_level_del_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags)
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
                errno != EBADF && errno != ENOENT && errno != ENOTSOCK)
            {
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
        ee.data.ptr = ngx_wepoll_event_data(c);
    } else {
        op = EPOLL_CTL_DEL;
        ee.events = 0;
        ee.data.ptr = NULL;
    }

    (void) flags;
    if (epoll_ctl(ngx_wepoll_epfd, op, c->fd,
                  op == EPOLL_CTL_DEL ? NULL : &ee) == -1)
    {
        if (errno == EBADF || errno == ENOENT || errno == ENOTSOCK) {
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
ngx_wepoll_add_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    if (ngx_wepoll_edge_enabled) {
        (void) flags;
        return ngx_wepoll_edge_add_event(ev, event);
    }
    return ngx_wepoll_level_add_event(ev, event, flags);
}

static ngx_int_t
ngx_wepoll_del_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    if (ngx_wepoll_edge_enabled) {
        return ngx_wepoll_edge_del_event(ev, event, flags);
    }
    return ngx_wepoll_level_del_event(ev, event, flags);
}

static ngx_int_t
ngx_wepoll_add_connection(ngx_connection_t *c)
{
    uint32_t                       classes;
    ngx_wepoll_connection_state_t *state;

    if (!ngx_wepoll_edge_enabled) {
        return NGX_ERROR;
    }

    state = ngx_wepoll_prepare_state(c);
    if (state == NULL) {
        return NGX_ERROR;
    }

    if (!state->registered &&
        ngx_wepoll_register_edge_connection(state, c) != NGX_OK)
    {
        return NGX_ERROR;
    }

    state->interests |= WEPOLL_EX_REARM_READ | WEPOLL_EX_REARM_WRITE;
    classes = state->disarmed &
              (WEPOLL_EX_REARM_READ | WEPOLL_EX_REARM_WRITE);
    if (ngx_wepoll_rearm_classes(state, classes) != NGX_OK) {
        return NGX_ERROR;
    }

    c->read->active = 1;
    c->write->active = 1;
    return NGX_OK;
}

static ngx_int_t
ngx_wepoll_del_connection(ngx_connection_t *c, ngx_uint_t flags)
{
    ngx_int_t                       result;
    ngx_wepoll_connection_state_t *state;

    (void) flags;
    result = NGX_OK;
    state = ngx_wepoll_state_for_connection(c);

    if (state != NULL && state->registered && state->connection == c &&
        ngx_wepoll_epfd != -1 && c->fd != (ngx_socket_t) -1)
    {
        if (epoll_ctl(ngx_wepoll_epfd, EPOLL_CTL_DEL, c->fd, NULL) == -1 &&
            errno != EBADF && errno != ENOENT && errno != ENOTSOCK)
        {
            ngx_wepoll_log_errno(c->log, "epoll_ctl(DEL connection)", c->fd);
            result = NGX_ERROR;
        }
    }

    c->read->active = 0;
    c->write->active = 0;
    ngx_wepoll_unbind_state(state);
    return result;
}

static ngx_int_t
ngx_wepoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer,
    ngx_uint_t flags)
{
    int                            timeout;
    int                            events;
    ngx_int_t                      i, result;
    ngx_err_t                      err;
    ngx_event_t                   *rev, *wev;
    ngx_connection_t              *c;
    ngx_queue_t                   *queue;
    ngx_uint_t                     post_events;
    uintptr_t                      instance;
    uint32_t                       revents;
    uint32_t                       delivered;
    ngx_uint_t                     read_ready, write_ready, terminal;
    ngx_uint_t                     read_dispatch, write_dispatch;
    ngx_wepoll_connection_state_t *state;

    result = NGX_OK;
    if (ngx_wepoll_edge_enabled &&
        ngx_wepoll_sweep_pending_states() != NGX_OK)
    {
        result = NGX_ERROR;
    }

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
            return result;
        }
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "wepoll_ex: epoll_wait_ex() failed (errno:%d)", err);
        return NGX_ERROR;
    }

    if (events == 0) {
        return result;
    }

    post_events = (flags & NGX_POST_EVENTS) ||
                  (ngx_wepoll_edge_enabled &&
                   ngx_wepoll_edge_post_events);

    for (i = 0; i < events; i++) {
        revents = ngx_wepoll_events[i].events;
        c = (ngx_connection_t *) ngx_wepoll_events[i].data.ptr;
        instance = (uintptr_t) c & (uintptr_t) 1;
        c = (ngx_connection_t *) ((uintptr_t) c & ~(uintptr_t) 1);

        state = NULL;
        if (ngx_wepoll_edge_enabled) {
            state = ngx_wepoll_events[i].user_ctx;
            if (!ngx_wepoll_state_is_current(state) ||
                state->connection != c || state->instance != instance)
            {
                continue;
            }

            if ((ngx_wepoll_events[i].flags & WEPOLL_FLAG_ET_DELIVERED) == 0 &&
                !ngx_wepoll_missing_edge_flag_logged)
            {
                ngx_wepoll_missing_edge_flag_logged = 1;
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "wepoll_ex: explicit edge event missing "
                              "WEPOLL_FLAG_ET_DELIVERED");
            }

        } else if (c == NULL || c->fd == (ngx_socket_t) -1 ||
                   c->read == NULL || c->read->instance != instance)
        {
            continue;
        }

        terminal = (revents & (EPOLLERR | EPOLLHUP)) != 0;
        read_ready = (revents & (EPOLLIN | EPOLLPRI | EPOLLRDNORM |
                                 EPOLLRDBAND | EPOLLRDHUP)) != 0;
        write_ready = (revents & (EPOLLOUT | EPOLLWRNORM |
                                  EPOLLWRBAND)) != 0;

        if (terminal) {
            read_ready = 1;
            write_ready = 1;
        }

        rev = c->read;
        wev = c->write;

        if (ngx_wepoll_edge_enabled) {
            delivered = 0;
            if (terminal) {
                delivered = WEPOLL_EX_REARM_ALL;
                ngx_wepoll_edge_terminal_deliveries++;
            } else {
                if (read_ready) {
                    delivered |= WEPOLL_EX_REARM_READ;
                }
                if (write_ready) {
                    delivered |= WEPOLL_EX_REARM_WRITE;
                }
            }
            state->disarmed |= delivered;

            read_dispatch = read_ready &&
                            (state->interests & WEPOLL_EX_REARM_READ);
            write_dispatch = write_ready &&
                             (state->interests & WEPOLL_EX_REARM_WRITE);

            if (read_dispatch) {
                state->pending |= WEPOLL_EX_REARM_READ;
                if (!rev->accept) {
                    rev->active = 0;
                }
                ngx_wepoll_edge_read_deliveries++;
            }

            if (write_dispatch) {
                state->pending |= WEPOLL_EX_REARM_WRITE;
                wev->active = 0;
                ngx_wepoll_edge_write_deliveries++;
            }

            ngx_wepoll_queue_pending_state(state);

        } else {
            read_dispatch = read_ready && rev->active;
            write_dispatch = write_ready && wev != NULL && wev->active;
        }

        if (read_dispatch) {
            rev->ready = 1;
            rev->available = -1;
            if (revents & EPOLLRDHUP) {
                rev->pending_eof = 1;
            }

            if (post_events) {
                queue = rev->accept ? &ngx_posted_accept_events
                                    : &ngx_posted_events;
                ngx_post_event(rev, queue);
            } else if (rev->handler != NULL) {
                rev->handler(rev);
            }
        }

        if (write_dispatch) {
            if (ngx_wepoll_edge_enabled) {
                if (!ngx_wepoll_state_is_current(state) ||
                    !(state->interests & WEPOLL_EX_REARM_WRITE))
                {
                    write_dispatch = 0;
                }
            } else if (c->fd == (ngx_socket_t) -1 ||
                       wev->instance != instance)
            {
                write_dispatch = 0;
            }

            if (write_dispatch) {
                wev->ready = 1;
#if (NGX_THREADS)
                wev->complete = 1;
#endif
                if (post_events) {
                    ngx_post_event(wev, &ngx_posted_events);
                } else if (wev->handler != NULL) {
                    wev->handler(wev);
                }
            }
        }

        if (ngx_wepoll_edge_enabled &&
            ngx_wepoll_state_is_current(state) &&
            ngx_wepoll_rearm_ready_state(state) != NGX_OK)
        {
            result = NGX_ERROR;
        }
    }

    return result;
}

static ngx_int_t
ngx_wepoll_init(ngx_cycle_t *cycle, ngx_msec_t timer)
{
    ngx_wepoll_conf_t *wcf;
    int                create_flags, size_hint;
    size_t             state_bytes, pending_bytes;

    (void) timer;
    wcf = ngx_event_get_conf(cycle->conf_ctx, ngx_wepoll_module);
    if (wcf == NULL) {
        return NGX_ERROR;
    }

    if (wcf->events == 0 ||
        wcf->events > (ngx_uint_t) INT_MAX ||
        wcf->events > (ngx_uint_t) ((size_t) -1 / sizeof(*ngx_wepoll_events)))
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "wepoll_events must fit an epoll_wait() event array");
        return NGX_ERROR;
    }

    ngx_wepoll_edge_enabled = wcf->edge;
    ngx_wepoll_edge_post_events = wcf->edge_post_events;
    ngx_wepoll_close_audit = wcf->close_audit;
    if (ngx_wepoll_edge_post_events && !ngx_wepoll_edge_enabled) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "wepoll_edge_post_events requires wepoll_edge on");
        return NGX_ERROR;
    }

    if (ngx_wepoll_epfd != -1 &&
        ngx_wepoll_port_edge != ngx_wepoll_edge_enabled)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "wepoll_edge cannot change while the event port exists");
        return NGX_ERROR;
    }

    if (ngx_wepoll_epfd == -1) {
        size_hint = cycle->connection_n > (ngx_uint_t) INT_MAX
                    ? INT_MAX : (int) cycle->connection_n;
        create_flags = EPOLL_CLOEXEC;
        if (ngx_wepoll_edge_enabled) {
            create_flags |= WEPOLL_EX_CREATE_EXPLICIT_REARM;
        }
        ngx_wepoll_epfd = epoll_create_ex(size_hint, create_flags);
        if (ngx_wepoll_epfd == -1) {
            ngx_wepoll_log_errno(cycle->log, "epoll_create_ex", -1);
            return NGX_ERROR;
        }
        ngx_wepoll_port_edge = ngx_wepoll_edge_enabled;
    }

    if (ngx_wepoll_nevents < wcf->events) {
        epoll_event_ex *events_array;

        events_array = ngx_alloc(sizeof(*events_array) * wcf->events,
                                 cycle->log);
        if (events_array == NULL) {
            return NGX_ERROR;
        }
        if (ngx_wepoll_events != NULL) {
            ngx_free(ngx_wepoll_events);
        }
        ngx_wepoll_events = events_array;
    }
    ngx_wepoll_nevents = wcf->events;

    if (ngx_wepoll_edge_enabled) {
        if (cycle->connection_n == 0 ||
            cycle->connection_n >
                (ngx_uint_t) ((size_t) -1 /
                              sizeof(*ngx_wepoll_states)) ||
            cycle->connection_n >
                (ngx_uint_t) ((size_t) -1 /
                              sizeof(*ngx_wepoll_pending_states)))
        {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                          "worker_connections exceeds edge state limits");
            return NGX_ERROR;
        }

        state_bytes = sizeof(*ngx_wepoll_states) * cycle->connection_n;
        pending_bytes = sizeof(*ngx_wepoll_pending_states) *
                        cycle->connection_n;
        ngx_wepoll_states = ngx_alloc(state_bytes, cycle->log);
        ngx_wepoll_pending_states = ngx_alloc(pending_bytes, cycle->log);
        if (ngx_wepoll_states == NULL || ngx_wepoll_pending_states == NULL) {
            if (ngx_wepoll_states != NULL) {
                ngx_free(ngx_wepoll_states);
                ngx_wepoll_states = NULL;
            }
            if (ngx_wepoll_pending_states != NULL) {
                ngx_free(ngx_wepoll_pending_states);
                ngx_wepoll_pending_states = NULL;
            }
            return NGX_ERROR;
        }
        ngx_memzero(ngx_wepoll_states, state_bytes);
        ngx_memzero(ngx_wepoll_pending_states, pending_bytes);
        ngx_wepoll_state_count = cycle->connection_n;
        ngx_wepoll_pending_count = 0;
    }

    ngx_wepoll_cycle = cycle;
    ngx_io = ngx_os_io;
    ngx_event_actions = ngx_wepoll_module_ctx.actions;

    if (ngx_wepoll_edge_enabled) {
        ngx_event_flags = NGX_USE_CLEAR_EVENT | NGX_USE_GREEDY_EVENT;
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "wepoll_ex: explicit-rearm edge mode enabled%s",
                      ngx_wepoll_edge_post_events
                      ? " with forced posted dispatch" : "");
    } else {
        ngx_event_actions.add_conn = NULL;
        ngx_event_actions.del_conn = NULL;
        ngx_event_flags = NGX_USE_LEVEL_EVENT;
    }

    return NGX_OK;
}

static void
ngx_wepoll_done(ngx_cycle_t *cycle)
{
    ngx_uint_t       audit_failed, audit_level;
    wepoll_ex_global_stats global_stats;
    wepoll_ex_stats  stats;

    if (ngx_wepoll_epfd != -1 && ngx_wepoll_close_audit) {
        ngx_memzero(&stats, sizeof(stats));
        if (wepoll_ex_get_stats(ngx_wepoll_epfd, &stats, sizeof(stats)) == -1) {
            ngx_wepoll_log_errno(cycle->log, "wepoll_ex_get_stats", -1);

        } else {
            /* A port may still own one cancellation-losing AFD request after
             * its final registration has been DEL'ed.  wepoll_close() drains
             * that port-level request; it is not a socket close-path leak. */
            audit_failed = stats.active_registrations != 0 ||
                           stats.rearm_queue_depth != 0 ||
                           stats.oneshot_probe_queue_depth != 0 ||
                           stats.ready_queue_depth != 0;
            audit_level = ngx_exiting && !ngx_terminate && audit_failed
                          ? NGX_LOG_ALERT : NGX_LOG_NOTICE;

            ngx_log_error(audit_level, cycle->log, 0,
                          "wepoll_ex: close audit %s policy:%ui "
                          "active:%uL pending:%uL rearm:%uL probes:%uL "
                          "ready:%uL stale:%uL identity:%uL async:%uL "
                          "wake:%uL/%uL/%uL tcp-probe:%uL/%uL",
                          audit_failed ? "dirty" : "clean",
                          (ngx_uint_t) stats.socket_lifetime_policy,
                          stats.active_registrations,
                          stats.pending_polls,
                          stats.rearm_queue_depth,
                          stats.oneshot_probe_queue_depth,
                          stats.ready_queue_depth,
                          stats.stale_events_dropped,
                          stats.identity_failures,
                          stats.asynchronous_errors,
                          stats.wake_requests,
                          stats.wake_coalesced,
                          stats.wake_returns,
                          stats.tcp_current_level_probes,
                          stats.tcp_current_level_fallbacks);
        }
    }

    if (ngx_wepoll_edge_enabled) {
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "wepoll_ex: edge summary read deliveries:%uL "
                      "write deliveries:%uL terminal deliveries:%uL "
                      "read rearms:%uL write rearms:%uL",
                      ngx_wepoll_edge_read_deliveries,
                      ngx_wepoll_edge_write_deliveries,
                      ngx_wepoll_edge_terminal_deliveries,
                      ngx_wepoll_edge_read_rearms,
                      ngx_wepoll_edge_write_rearms);
    }

    if (ngx_wepoll_epfd != -1) {
        if (wepoll_close(ngx_wepoll_epfd) == -1) {
            ngx_wepoll_log_errno(cycle->log, "wepoll_close", -1);
        }
        ngx_wepoll_epfd = -1;
    }
    if (ngx_wepoll_close_audit) {
        ngx_memzero(&global_stats, sizeof(global_stats));
        if (wepoll_ex_get_global_stats(&global_stats, sizeof(global_stats))
            == -1)
        {
            ngx_wepoll_log_errno(cycle->log,
                                 "wepoll_ex_get_global_stats", -1);

        } else {
            audit_level = ngx_exiting && !ngx_terminate &&
                          global_stats.active_quarantines != 0
                          ? NGX_LOG_ALERT : NGX_LOG_NOTICE;
            ngx_log_error(audit_level, cycle->log, 0,
                          "wepoll_ex: lifecycle audit quarantined:%uL "
                          "reaped:%uL irrecoverable:%uL timeouts:%uL "
                          "active:%uL",
                          global_stats.quarantined_ports,
                          global_stats.reaped_ports,
                          global_stats.irrecoverable_ports,
                          global_stats.api_close_timeouts,
                          global_stats.active_quarantines);
        }
    }
    if (ngx_wepoll_events != NULL) {
        ngx_free(ngx_wepoll_events);
        ngx_wepoll_events = NULL;
    }
    if (ngx_wepoll_states != NULL) {
        ngx_free(ngx_wepoll_states);
        ngx_wepoll_states = NULL;
    }
    if (ngx_wepoll_pending_states != NULL) {
        ngx_free(ngx_wepoll_pending_states);
        ngx_wepoll_pending_states = NULL;
    }

    ngx_wepoll_nevents = 0;
    ngx_wepoll_cycle = NULL;
    ngx_wepoll_state_count = 0;
    ngx_wepoll_pending_count = 0;
    ngx_wepoll_edge_enabled = 0;
    ngx_wepoll_edge_post_events = 0;
    ngx_wepoll_close_audit = 0;
    ngx_wepoll_port_edge = 0;
    ngx_wepoll_missing_edge_flag_logged = 0;
    ngx_wepoll_edge_read_deliveries = 0;
    ngx_wepoll_edge_write_deliveries = 0;
    ngx_wepoll_edge_terminal_deliveries = 0;
    ngx_wepoll_edge_read_rearms = 0;
    ngx_wepoll_edge_write_rearms = 0;
}

static void
ngx_wepoll_exit_process(ngx_cycle_t *cycle)
{
    ngx_wepoll_done(cycle);
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
    wcf->edge = NGX_CONF_UNSET;
    wcf->edge_post_events = NGX_CONF_UNSET;
    wcf->close_audit = NGX_CONF_UNSET;
    return wcf;
}

static char *
ngx_wepoll_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_wepoll_conf_t *wcf = conf;

    (void) cycle;
    ngx_conf_init_uint_value(wcf->events, 512);
    ngx_conf_init_value(wcf->edge, 0);
    ngx_conf_init_value(wcf->edge_post_events, 0);
    ngx_conf_init_value(wcf->close_audit, 1);
    return NGX_CONF_OK;
}
