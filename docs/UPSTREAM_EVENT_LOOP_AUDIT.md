# Upstream Event-Loop Source Audit

## Scope

This audit was performed on August 4, 2026 and revisited on August 5, 2026
against the exact reference archives stored beside the repository:

- `nginx-1.31.3.tar.gz` (`nginx` 1.31.3);
- `libuv-v1.52.1.tar.gz` (libuv 1.52.1);
- `mio-master.zip` (the snapshot identifies itself as Mio 1.2.2); and
- `asio-develop.zip` (the snapshot identifies Boost.Asio 1.38.2).

The archives are read-only reference inputs and must not be staged. The audit
compares event-loop contracts, not just epoll symbol names. It does not claim
that any of these projects supports or endorses wepoll-ex.

## Summary

The four loops divide into two groups. nginx, Mio's Unix selector, and Asio's
epoll reactor depend on edge-triggered handler discipline. libuv's Linux loop
uses level-triggered epoll, while libuv and Mio already have purpose-built
Windows IOCP/AFD implementations that are more integrated than an epoll shim.

| Source | Native pattern | Useful wepoll-ex mapping | Remaining boundary |
| --- | --- | --- | --- |
| nginx 1.31.3 | Duplex socket ET, eventfd notify/AIO, pointer plus instance bit | Explicit readiness-class rearm; tagged wake for control notify; close helper | Handler-completion rearm, file AIO, multi-process exclusive behavior |
| libuv 1.52.1 | Linux LT epoll with queued changes; native Windows IOCP/AFD | Tagged wake for an epoll-shaped experiment; existing LT semantics | Replacing libuv's Windows backend would lose native completion integration |
| Mio 1.2.2 | Unix ET plus eventfd; Windows AFD disarm/rearm plus tagged IOCP wake | Explicit class rearm closely matches `WouldBlock` ownership; tagged wake matches its Waker; virtual aliases match selector cloning | Named-pipe IOCP is separate; the alias is not a native descriptor |
| Boost.Asio 1.38.2 | Socket ET, dynamic write MOD, persistent eventfd interrupter | Observed ET works; explicit rearm can avoid writable resampling; tagged wake replaces the interrupter side channel | Handler changes, timer/file completion integration |

## nginx 1.31.3

`src/event/modules/ngx_epoll_module.c` adds a connection with
`EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP`, stores the connection pointer and
instance bit in `epoll_event.data.ptr`, promotes `EPOLLERR | EPOLLHUP` into
both read and write dispatch, and relies on handlers draining their operations.
Normal Linux ET registrations are not rearmed after each handler.

The control notification and optional Linux file-AIO paths both use eventfd,
but they solve different problems. `wepoll_ex_wake_event()` can replace the
control eventfd with a tagged synthetic event. It does not implement Linux
file AIO or deliver `io_getevents()` results. The native module also skips
`EPOLL_CTL_DEL` on close paths that rely on Linux automatic removal; a Windows
port using synchronized lifetime or explicit rearm must DEL before
`closesocket()`.

The reusable structure is the connection registration and stale-instance
dispatch. The non-portable parts are ET ownership, eventfd/file-AIO setup,
native epoll descriptor lifetime, and cross-process `EPOLLEXCLUSIVE` behavior.
The detailed port contract is in `NGINX_NATIVE_EPOLL_PORT.md`.

## libuv 1.52.1

libuv's Linux backend is level-triggered. It queues watcher changes, applies
ADD or MOD when entering the poll, tolerates ADD/`EEXIST` by retrying MOD, and
performs DEL before descriptor retirement. Linux async wake uses eventfd.
Recent Linux builds can also use io_uring for batched epoll control and file
operations; that is a Linux completion facility rather than an epoll semantic
that should be emulated by this library.

`src/win/poll.c` already contains a mature Windows design: provider-specific
peer sockets, alternating AFD poll requests, IOCP completion ownership, and a
select-thread fallback. Its async path posts a tagged IOCP completion directly.
Replacing this backend with wepoll-ex would add an epoll translation layer and
discard libuv-specific completion and lifecycle knowledge. The relevant ideas
for wepoll-ex are tagged control wakes and, if measurements justify it, a
supplemental AFD request that reduces cancel/refresh work when interest expands.

`epoll_ctl_batch()` is not equivalent to libuv's queued watcher update or its
io_uring submission path. No upsert or transactional-batch guarantee is added
by this audit.

## Mio 1.2.2

Mio's Unix epoll selector always includes `EPOLLET`, stores its token in
`epoll_event.u64`, uses explicit ADD/MOD/DEL, can clone the underlying epoll
descriptor, and uses eventfd for its Waker.

Mio's Windows selector already uses IOCP and AFD. Delivered AFD flags are
disarmed, and the I/O wrapper rearms after an operation reaches
`WouldBlock`. That ownership model is close to a wepoll-ex port created with
`WEPOLL_EX_CREATE_EXPLICIT_REARM`: a handler drains one direction and calls
`epoll_rearm_classes()` for that direction. Mio's Windows Waker posts a tagged
IOCP completion, which maps directly to `wepoll_ex_wake_event()`.

A source-oriented port of Mio's Unix selector can now use
`wepoll_ex_dup()` for its same-process selector-clone lifetime. The aliases
share one port and final close owns destruction, but they remain virtual
integers rather than native duplicable descriptors. Mio's Windows named-pipe
support still uses a separate completion path rather than socket readiness;
that boundary should not be hidden behind ordinary socket registrations.

## Boost.Asio 1.38.2

Asio's epoll reactor registers descriptors with
`EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLPRI | EPOLLET` and adds `EPOLLOUT` with
MOD when a write operation blocks. Descriptor-state pointers are stored in
the event payload. Its interrupter is registered ET, deliberately remains
readable, and is retriggered with a same-mask `EPOLL_CTL_MOD`. An optional
timerfd handles reactor deadlines.

Ordinary observed ET can run this pattern, but continuously writable sockets
may incur extra readiness sampling. Explicit readiness-class rearm offers a
more deterministic socket contract if Asio acknowledges a direction only
after its operation reaches `WouldBlock`. A tagged wake can replace the
persistent eventfd interrupter. Waitable HANDLE ET now has a regression for
Asio's exact persistent-ready plus MOD-retrigger behavior.

## Improvements selected from the audit

The following library work is implemented and regression-tested:

1. `WEPOLL_EX_CREATE_EXPLICIT_REARM` and `epoll_rearm_classes()` provide
   direction-aware ET ownership for nginx-, Mio-, and Asio-style socket loops.
2. `wepoll_ex_wake_event()` posts one coalesced, snapshotted synthetic event on
   Windows. A tagged request upgrades a pending plain wake; the first pending
   tagged payload wins; ordinary readiness and asynchronous errors retain
   priority. Extended waits mark it with `WEPOLL_FLAG_WAKE_EVENT`.
3. A persistent manual-reset waitable regression verifies Asio's ET
   MOD-retrigger pattern without resetting the ready level.
4. Fault injection verifies that a failed tagged IOCP post clears pending wake
   state and makes the port fail through the existing fatal-post path.
5. `wepoll_ex_dup()` supplies a virtual selector alias with shared readiness
   and final-close ownership, covering Mio's process-local clone requirement
   without claiming native descriptor semantics.
6. `wepoll_ex_close_socket()` performs one-port DEL-before-close ordering
   without retaining the socket; multi-port ownership remains explicit.
7. Explicit readiness-class rearm now composes with `EPOLLONESHOT`; partial
   acknowledgement keeps the one-shot disabled and the final class starts the
   next generation.
8. `wepoll_ex_get_last_error_info()` distinguishes portable errno, exact native
   Win32/Winsock/NTSTATUS sources, and canonical Winsock equivalents.
9. The opt-in `wepoll_ex::epoll_compat` package target exposes an isolated
   Windows `<sys/epoll.h>` include path for source-oriented builds.

## Remaining candidates

These are candidates, not promises:

- Prototype a supplemental/double-buffered AFD request for interest expansion,
  inspired by libuv, only after cancellation and probe counters show a useful
  bottleneck. It increases request, provider-handle, and close complexity.
- Add cross-process exclusive arbitration only for a demonstrated multi-worker
  workload. Current `EPOLLEXCLUSIVE` coordination is process- and image-local.
- Keep native file AIO, io_uring, timerfd, named-pipe completions, and general
  IOCP operation dispatch as explicit integration layers rather than pretending
  they are socket epoll readiness.

The highest-value next step is an end-to-end explicit-rearm nginx experiment,
not another unconditional epoll flag. It must qualify accept, posted events,
TLS, proxy backpressure, FIN/reset, reload, graceful quit, and every close path
before performance comparisons are meaningful.

The resulting source-porting surface and the remaining non-drop-in descriptor
boundaries are summarized in `WINDOWS_PORTING.md`.
