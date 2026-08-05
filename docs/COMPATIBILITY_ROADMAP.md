# Compatibility Improvement Roadmap

## Purpose

The qualified core API now covers the practical epoll behavior exercised by
the current socket, pipe, waitable, lifecycle, and concurrency tests. The
remaining differences are primarily Windows semantic boundaries rather than
unclassified defects. This roadmap prioritizes the gaps that matter when a
Linux edge-driven server, particularly nginx, is adapted to the Windows
IOCP/AFD backend.

The objective is not to claim that every Linux epoll facility can be recreated
on Windows. Each improvement must expose its actual contract, preserve the
documented fallback, and add deterministic regressions before an integration
depends on it.

## Priority 0: integration correctness

### Deterministic edge rearming

Windows `EPOLLET` is currently an observed-level filter over AFD snapshots.
An event bit is suppressed while the latest sampled level remains asserted,
and becomes eligible again after a newly submitted AFD request proves the
level inactive by remaining pending. This is useful but is not a Linux kernel
edge queue.

The first nginx-facing library slice now provides a separately opted-in
explicit rearm contract:

1. delivery disarms the selected readiness classes;
2. the application drains its socket operations to `WSAEWOULDBLOCK`;
3. the event module explicitly acknowledges the drain and rearms the
   registration; and
4. a fresh AFD submission begins the next observation generation.

`WEPOLL_EX_CREATE_EXPLICIT_REARM` selects it per Windows port, and
`epoll_rearm_classes()` acknowledges READ, WRITE, or TERMINAL ownership.
Delivered read/write classes stay disabled while undelivered directions remain
in the AFD request. Terminal delivery disables every class; MOD clears all
disarms; pending-mask expansion uses the existing cancellation-losing refresh
path; and DEL retires idle or pending state. The contract is socket-only and
rejects EXCLUSIVE. It now supports ONESHOT: partial class acknowledgement keeps
the one-shot fired, while the final delivered-class acknowledgement starts its
next generation. The base mode and stronger combination are reported by
`WEPOLL_EX_CAP_EXPLICIT_EDGE_REARM` and
`WEPOLL_EX_CAP_EXPLICIT_REARM_ONESHOT`.

Library regressions cover duplex independence, incomplete drains, pending AFD
mask expansion, terminal idling, MOD reset, and DEL. The remaining work is the
nginx handler integration itself: rearm must occur after the real accept,
read, or write handler drains, including after `NGX_POST_EVENTS` dispatch.
Until those nginx regressions exist, the checked-in adapter remains
level-triggered. See `docs/NGINX_NATIVE_EPOLL_PORT.md`.

### Wait notification

An embedding event loop needs a supported way for another thread or control
path to interrupt a blocked wait. Windows now provides both a coalesced
`wepoll_ex_wake()` zero-event return and `wepoll_ex_wake_event()`, which
snapshots one application-supplied event. A tagged request upgrades a pending
plain wake, while later requests do not replace an already pending tagged
payload. Basic waits return the supplied event directly; extended waits add
`WEPOLL_FLAG_WAKE_EVENT`. Already-ready registrations and pending errors retain
priority. The library does not execute callbacks inside `epoll_wait()`.

The POSIX wrapper deliberately reports both operations unsupported. Its basic
wait calls go directly to libc, and injecting the existing internal eventfd
would either expose a private token or require an additional outer epoll layer.
`WEPOLL_EX_CAP_TAGGED_WAKE_EVENT` makes the stronger Windows contract
discoverable. nginx control notification, Mio's Waker, libuv's async marker,
and Asio's interrupter can map to the tagged form without creating a synthetic
socket or eventfd side channel.

### Exact-source audit follow-ups

The nginx 1.31.3, libuv 1.52.1, Mio 1.2.2, and Boost.Asio 1.38.2 reference
archives were compared in `docs/UPSTREAM_EVENT_LOOP_AUDIT.md`. Direct porting
gaps implemented from that audit now include opt-in `<sys/epoll.h>` packaging,
virtual epfd aliases with final-close semantics, one-port close and shutdown
helpers, explicit rearm with ONESHOT, and a documented native error-info
channel. The remaining cross-loop candidates are intentionally conditional:

- a supplemental/double-buffered AFD request, inspired by libuv, may reduce
  cancel/refresh work during interest expansion, but should follow measured
  cancellation and probe pressure;
- native file AIO, io_uring, timerfd, named-pipe completions, and general IOCP
  dispatch remain separate integration facilities, not epoll readiness gaps.

### Socket lifetime ownership

The existing best-effort, strict, and synchronized policies remain distinct.
A production nginx profile should qualify synchronized mode only after every
core and third-party path is proven to issue DEL before `closesocket()`.
`wepoll_ex_close_socket()` now performs DEL against one specified epfd and then
closes the socket, accepting an already absent registration after validating
the target. It neither retains a duplicate nor changes peer-FIN behavior. A
socket registered in multiple ports still has to be removed from the other
ports before the helper is used for final close.

### Local shutdown publication

`wepoll_ex_shutdown_socket()` owns the Windows shutdown call when a port needs
Linux-like local EOF readiness. A registered TCP receive shutdown publishes
requested readable aliases plus `EPOLLRDHUP`; full local shutdown also
publishes `EPOLLHUP`. LT, observed ET, ONESHOT, explicit rearm, queued MOD
replacement, and blocked-wait wakeup share the normal ready-state rules.
Pending AFD work is cancelled/refreshed rather than requiring a full table
scan. Recorded state makes a retry after a nonfatal cancellation/publication
failure idempotent with respect to native shutdown.

The helper is intentionally one-port and does not change Winsock `recv()`
semantics. Direct `shutdown()` calls, another independently created epfd, and
code that expects zero-byte Linux EOF remain explicit porting boundaries. Its
record is registration-local: DEL/re-ADD starts a fresh registration without
reconstructing prior local shutdown state.

### Windows error channel

wepoll-ex continues to report portable `errno`, while
`wepoll_ex_get_last_error_info()` now exposes a per-thread versioned record for
the most recent failed library call. Windows retains an exact Win32, Winsock,
or NTSTATUS source where the failing path has it and separately marks a
canonical Winsock equivalent when meaningful. A normalized mapping without
the exact-source flag is not presented as the originating native failure. The
successful getter preserves the channel. Application socket-operation errors
remain owned by the application and are not overwritten merely to populate
this record.

## Priority 1: measurable performance and deployment behavior

### Current TCP readiness probes

Established TCP completions may run `getpeername()` followed by a zero-time
`WSAPoll()` so a FIN or reset racing behind an earlier AFD readiness class is
merged into the delivered snapshot. Providers that reject the probe use
zero-time `select()` fallbacks.

The first implementation slice adds counters for current-level probes and
fallbacks. Optimization follows evidence: retain the AFD-only fast path when
the completion is unambiguous, probe mixed-direction or terminal-race cases,
and evaluate batched qualification before changing correctness behavior.

### Exclusive wake scope

Windows `EPOLLEXCLUSIVE` arbitration is process-local and shared only by one
loaded wepoll-ex image. nginx should use an accept mutex or a designated accept
owner across worker processes. Cross-process named shared arbitration is an
optional future facility, not an implied property of the existing flag.

### Capability and environment reporting

`wepoll_ex_get_capabilities()` reports semantic backend properties such as
native versus observed edge delivery, process-local exclusive arbitration,
wait-wake support, signal-mask application, and native versus virtual epoll
descriptors. It also advertises the close helper, explicit ONESHOT rearm,
virtual epfd alias, shutdown helper, and error-info contracts. Deployment
qualification should additionally record Windows and compiler versions,
socket-lifetime policy, provider behavior, and probe counters.

### Toolchain and provider matrix

The current evidence is strongest for x86-64 MinGW-w64 on Windows 10. Future
qualification should cover Windows 8, current Windows 11 builds, MSVC,
clang-cl, x86 and ARM64 layouts, synchronous sockets, and real layered service
providers. AFD remains undocumented, so startup capability failures must be
reported cleanly rather than being treated as a portable guarantee.

## Priority 2: opt-in platform bridges

- The close-aware shutdown wrapper is implemented. Arbitrary direct Winsock
  calls and registrations in other independent ports remain outside the
  library's observation.
- `SO_OOBINLINE`, inert band bits, `EPOLLMSG`, signal interruption,
  `EPOLLWAKEUP`, nested epoll descriptors, and Linux file AIO should remain
  explicit limitations unless a demonstrated workload requires a separate
  Windows facility.
- Generic pipe and waitable extensions should not be expanded merely to make
  the socket/nginx compatibility claim look broader.
- A supplemental AFD request remains an opt-in candidate and should not change
  the default request topology without a concrete embedder and before/after
  measurements.

## Required nginx qualification

An edge-driven nginx experiment is not complete until it covers accepting,
HTTP keep-alive, TLS, upstream proxying, read and write backpressure, graceful
FIN, abortive reset, client write-half-close, reload, graceful quit, multiple
workers, and relevant third-party close paths. Level-triggered, observed-edge,
and explicit-rearm variants must be benchmarked separately with the probe and
wake counters recorded.
