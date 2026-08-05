# Porting nginx's Native epoll Module to Windows

## Conclusion

The nginx 1.31.3 Linux `ngx_epoll_module.c` control pattern is close enough to
reuse structurally, but it is not a trivial rename-and-recompile port against
wepoll-ex.  Its socket registration and dispatch layout can be retained; its
edge ownership, notification, file-AIO, and descriptor-lifetime assumptions
cannot.

The exact nginx source in `nginx-1.31.3.tar.gz` registers a connection with
`EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP`, promotes `EPOLLERR | EPOLLHUP`
to both readable and writable handling, and invokes or posts the read and
write handlers.  It never rearms a normal edge registration because Linux's
kernel epoll ready list records later transitions after the handler drains the
socket.  AFD exposes one level snapshot per request instead of that kernel
edge queue.

The checked-in `ngx_wepoll_module.c` now demonstrates a compatible nginx event
lifecycle using explicit readiness-class rearm. It is a semantic adapter, not
a proof that `ngx_epoll_module.c` can be ported by renaming a few functions.

Ordinary wepoll-ex `EPOLLET` remains an observed-level compatibility filter.
It suppresses a continuously true bit and resamples to prove that the level
became inactive.  That mode is useful and already regression-tested, but an
always-writable nginx connection can require extra immediate AFD observations
and deferred retries.  Treating `EPOLLONESHOT` as a substitute is incorrect:
rearming a duplex registration also rearms the continuously writable class,
which can create an `EPOLLOUT` loop.

## Explicit edge-rearm facility

The first nginx-facing library improvement is the Windows-only
`WEPOLL_EX_CREATE_EXPLICIT_REARM` flag for `epoll_create_ex()`.  On a port
created with that flag, socket `EPOLLET` registrations use readiness-class
ownership:

- a delivered read class (`EPOLLIN`, `EPOLLRDNORM`, `EPOLLPRI`, or
  `EPOLLRDHUP`) is disabled until `WEPOLL_EX_REARM_READ` is acknowledged;
- a delivered write class (`EPOLLOUT` or `EPOLLWRNORM`) is disabled until
  `WEPOLL_EX_REARM_WRITE` is acknowledged;
- `EPOLLERR` or `EPOLLHUP` disables read, write, and terminal polling while
  the application decides whether to close or deliberately rearm; and
- undelivered classes remain in the AFD request, so a disarmed writable class
  does not prevent a later read or terminal notification.

The application calls
`epoll_rearm_classes(epfd, fd, WEPOLL_EX_REARM_*)` only after the corresponding
socket operations have run to `WSAEWOULDBLOCK`.  Rearming an incompletely
drained class deliberately produces one immediate level delivery and disarms
it again.  Rearming a drained class submits a pending AFD observation, closing
the race between the final `WSAEWOULDBLOCK` and the next wait.  If another
class already has a pending AFD request, the implementation retains it when it
covers the expanded mask or cancels/refreshes it using the same race handling
as MOD.

`epoll_rearm()` is an all-class shorthand on such a registration.  A successful
MOD clears all class disarms and starts a fresh observation.  DEL retires an
idle or pending registration normally. `EPOLLONESHOT` can be combined with
socket ET on an explicit-rearm port: acknowledging only some classes from a
fired delivery keeps the one-shot disabled, and acknowledging the final class
starts its next generation. `EPOLLEXCLUSIVE` remains incompatible; pipe and
generic waitable ET are also outside this contract. POSIX does not emulate the
extension and reports `EOPNOTSUPP`.

A terminal delivery can leave no native poll in flight.  Explicit-rearm users
must therefore complete `EPOLL_CTL_DEL` before `closesocket()`.  This matches
the synchronized lifetime profile intended for an nginx experiment, but every
core and third-party close path still needs qualification.
`wepoll_ex_close_socket()` can centralize DEL-then-close for a socket owned by
one epfd; it does not remove registrations from other ports or discover nginx
objects that bypass the chosen close wrapper.

## Checked-in nginx edge adapter

`events { wepoll_edge on; }` creates an explicit-rearm port and installs one
fixed duplex ET registration for each nginx connection. Stable state indexed
by the nginx connection slot records descriptor identity, instance, active
interests, disarmed classes, and pending acknowledgements. The ordinary event
payload still carries nginx's connection pointer and instance bit; extended
`user_ctx` carries the stable state so stale records are rejected before the
connection pointer is dereferenced.

The adapter exposes `add_conn`/`del_conn` and deliberately omits
`NGX_USE_EPOLL_EVENT`, causing accepted and outbound sockets to use the
connection-wide lifecycle. On delivery it transfers READ and WRITE ownership
to nginx. A direct handler can rearm after it returns and clears `ready`; a
posted handler is rechecked at the next `process_events` entry. The accept
handler participates because it clears its own `ready` bit after draining.
`wepoll_edge_post_events on` forces the delayed posted path for qualification.
`del_conn` performs DEL-before-close, and worker exit logs delivery/rearm
counters.

Level-triggered operation remains the default. The edge mode does not add
native file AIO, nginx thread notification, local-shutdown interposition, or a
native epoll descriptor. Those remain separate from the socket handler contract.

## Local shutdown bridge

The exact nginx 1.31.3 platform headers define `ngx_shutdown_socket` as the
native `shutdown()` call. A Windows epoll experiment should route that macro or
an equivalent event-module hook through
`wepoll_ex_shutdown_socket(epfd, fd, how)`. For a registered TCP socket, local
read shutdown then publishes the requested readable aliases and
`EPOLLRDHUP`; shutting down both directions also publishes `EPOLLHUP`. The
state follows LT/ET, ONESHOT, explicit-rearm, MOD, and blocked-wait rules, so
the native module does not need a full registration scan to discover its own
half-close.

This bridge is one-port and changes readiness only. Third-party code that
calls Winsock `shutdown()` directly, or another independent epfd containing
the socket, remains outside observation. nginx data-path code must also retain
the Windows result contract: `recv()` after local receive shutdown reports
`WSAESHUTDOWN`, not Linux's zero-byte EOF. A cancellation/publication error can
be returned after native shutdown has succeeded; a still-usable port can retry
the same helper call because the native direction is recorded. That record is
owned by the current registration, so a DEL/re-ADD sequence must not assume
that the new registration inherited an earlier local shutdown publication.

## Notification bridge

The native module's control `eventfd` can be replaced without emulating a Unix
descriptor. `wepoll_ex_wake_event()` snapshots a synthetic `EPOLLIN` event
whose data points at the nginx notify connection. The next Windows wait returns
that record after ordinary readiness and pending errors; repeated requests
coalesce, and extended waits identify it with `WEPOLL_FLAG_WAKE_EVENT`. nginx
must still serialize ownership of `notify_event.data` and execute the pending
handler in its normal dispatch path.

This mapping applies only to nginx's control notification. The optional Linux
file-AIO eventfd is coupled to `io_setup`, `io_submit`, and `io_getevents` and
therefore needs a separate Windows file-I/O completion design. A tagged wake
does not manufacture file-AIO completion records.

## Changes required by a direct native-module port

Using this facility is more than changing `epoll_create`, `epoll_ctl`, and
`epoll_wait` names:

1. nginx must create the port with `epoll_create_ex(...,
   WEPOLL_EX_CREATE_EXPLICIT_REARM)`;
2. an accept handler must rearm READ only after `accept()` drains to
   `WSAEWOULDBLOCK`;
3. connection read and write handlers must acknowledge their own classes only
   after their recv/send loops drain, and only if the registration is still
   active;
4. when `NGX_POST_EVENTS` is used, rearm belongs after the posted handler
   executes, not at the end of `ngx_epoll_process_events()`;
5. `EPOLLERR | EPOLLHUP` handling must close/DEL or deliberately rearm the
   surviving classes;
6. every local socket shutdown path must use the event module's
   `wepoll_ex_shutdown_socket()` bridge; and
7. the control notify path must use a tagged wake or another Windows primitive;
   optional Linux file AIO and native epoll-fd close assumptions still need
   separate Windows implementations.

The build can retain nginx's `<sys/epoll.h>` include through the opt-in
`wepoll_ex::epoll_compat` target, but that removes only a source-level include
edit. `wepoll_ex_dup()` can supply same-process shared epfd aliases if nginx
code needs them; it is not a native descriptor and cannot satisfy inheritance,
`fcntl`, or `ioctl` assumptions. `wepoll_ex_get_last_error_info()` can bridge a
failed wepoll-ex control operation into nginx logging without guessing whether
the normalized errno originated as Win32, Winsock, or NTSTATUS.

Those changes can be localized around nginx's event lifecycle, but they are
semantic changes rather than symbol aliases. The checked-in adapter now
implements items 1 through 5 for its own socket lifecycle and validates direct
and posted handlers. It does not transform the native Linux module, interpose
the shutdown macro, enable notify/file AIO, or make virtual epfds native
descriptors.

## Performance expectation and qualification

Explicit rearm removes repeated observation of a continuously writable class
while preserving native AFD coverage for the other directions.  It should
reduce the main avoidable overhead in an nginx-style duplex ET registration,
but no throughput claim follows from the mechanism alone.  Extra cancellation
is possible when a handler rearms one direction while another direction has a
narrow pending request, and established TCP delivery may still use the
current-level `WSAPoll` qualification needed for FIN/reset races.

The checked-in adapter has now covered accept draining, HTTP keep-alive, TLS,
upstream proxying, read/write backpressure, graceful FIN, abortive reset,
direct client write-half-close, posted events, reload, graceful worker exit,
and multiple workers in bounded loopback tests. Proxied client write-half-close
failed identically in level and edge modes and remains a stock Win32 nginx
baseline rather than an edge-specific pass. Arbitrary third-party close paths
still require an audit, and level versus explicit-rearm throughput must be
measured separately with probe, wake, and lifecycle diagnostics recorded.

The broader nginx/libuv/Mio/Asio comparison and the reasons for not treating
native completion facilities as epoll flags are recorded in
`UPSTREAM_EVENT_LOOP_AUDIT.md`.
