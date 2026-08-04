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

The preferred nginx-facing improvement is a separately opted-in explicit
rearm contract:

1. delivery disarms the selected readiness classes;
2. the application drains its socket operations to `WSAEWOULDBLOCK`;
3. the event module explicitly acknowledges the drain and rearms the
   registration; and
4. a fresh AFD submission begins the next observation generation.

The design must define duplex read/write ownership, readiness arriving during
handler execution, MOD and DEL races, terminal FIN/reset delivery, and
interaction with `EPOLLONESHOT`. Until those rules and their nginx regressions
exist, the checked-in nginx adapter remains level-triggered.

### Wait notification

An embedding event loop needs a supported way for another thread or control
path to interrupt a blocked wait. The first implementation slice adds a
coalesced `wepoll_ex_wake()` operation for Windows basic and extended waits.
It produces an early zero-event return only after already-ready events and
pending errors have priority. The caller owns the notification payload or
handler queue; the library does not execute callbacks inside `epoll_wait()`.

The POSIX wrapper deliberately reports this operation unsupported. Its basic
wait calls go directly to libc, and injecting the existing internal eventfd
would either expose a private token or require an additional outer epoll layer.
The capability API makes this difference discoverable.

### Socket lifetime ownership

The existing best-effort, strict, and synchronized policies remain distinct.
A production nginx profile should qualify synchronized mode only after every
core and third-party path is proven to issue DEL before `closesocket()`.
Future work may add a close-aware helper or opaque registration token, but it
must not silently take ownership of application sockets or retain a duplicate
that changes peer-FIN behavior.

### Windows error channel

wepoll-ex reports portable `errno`, while Win32 nginx frequently reads Win32
or Winsock error state. A future error bridge should expose a documented
normalized error and, where meaningful, its Winsock equivalent. It must define
success-path preservation and avoid replacing the error from a completed
socket operation with an epoll bookkeeping error.

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
descriptors. Deployment qualification should additionally record Windows and
compiler versions, socket-lifetime policy, provider behavior, and probe
counters.

### Toolchain and provider matrix

The current evidence is strongest for x86-64 MinGW-w64 on Windows 10. Future
qualification should cover Windows 8, current Windows 11 builds, MSVC,
clang-cl, x86 and ARM64 layouts, synchronous sockets, and real layered service
providers. AFD remains undocumented, so startup capability failures must be
reported cleanly rather than being treated as a portable guarantee.

## Priority 2: opt-in platform bridges

- A close-aware `shutdown()` wrapper could publish Linux-like local
  receive/full-shutdown state when every caller uses it. Arbitrary direct
  Winsock calls remain outside the library's observation.
- `SO_OOBINLINE`, inert band bits, `EPOLLMSG`, signal interruption,
  `EPOLLWAKEUP`, nested epoll descriptors, and Linux file AIO should remain
  explicit limitations unless a demonstrated workload requires a separate
  Windows facility.
- Generic pipe and waitable extensions should not be expanded merely to make
  the socket/nginx compatibility claim look broader.

## Required nginx qualification

An edge-driven nginx experiment is not complete until it covers accepting,
HTTP keep-alive, TLS, upstream proxying, read and write backpressure, graceful
FIN, abortive reset, client write-half-close, reload, graceful quit, multiple
workers, and relevant third-party close paths. Level-triggered, observed-edge,
and explicit-rearm variants must be benchmarked separately with the probe and
wake counters recorded.
