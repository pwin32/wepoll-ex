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
idle or pending registration normally.  `EPOLLONESHOT` and `EPOLLEXCLUSIVE`
cannot be combined with socket ET on an explicit-rearm port in this initial
contract; pipe and generic waitable ET are also outside its scope.  POSIX does
not emulate the extension and reports `EOPNOTSUPP`.

A terminal delivery can leave no native poll in flight.  Explicit-rearm users
must therefore complete `EPOLL_CTL_DEL` before `closesocket()`.  This matches
the synchronized lifetime profile intended for an nginx experiment, but every
core and third-party close path still needs qualification.

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

## nginx changes still required

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
   surviving classes; and
6. the control notify path must use a tagged wake or another Windows primitive;
   optional Linux file AIO and native epoll-fd close assumptions still need
   separate Windows implementations.

Those changes can be localized around nginx's event lifecycle, but they are
semantic changes rather than symbol aliases.  The checked-in
`ngx_wepoll_module.c` therefore remains level-triggered until a separate
edge-driven nginx experiment implements this handler-completion contract.

## Performance expectation and qualification

Explicit rearm removes repeated observation of a continuously writable class
while preserving native AFD coverage for the other directions.  It should
reduce the main avoidable overhead in an nginx-style duplex ET registration,
but no throughput claim follows from the mechanism alone.  Extra cancellation
is possible when a handler rearms one direction while another direction has a
narrow pending request, and established TCP delivery may still use the
current-level `WSAPoll` qualification needed for FIN/reset races.

An nginx port is not qualified until it covers accept draining, HTTP
keep-alive, TLS, upstream proxying, read/write backpressure, graceful FIN,
abortive reset, write half-close, posted events, reload, graceful quit,
multiple workers, and third-party close paths.  Level-triggered,
observed-edge, and explicit-rearm builds must be measured separately with the
wepoll-ex probe, wake, and lifecycle diagnostics recorded.

The broader nginx/libuv/Mio/Asio comparison and the reasons for not treating
native completion facilities as epoll flags are recorded in
`UPSTREAM_EVENT_LOOP_AUDIT.md`.
