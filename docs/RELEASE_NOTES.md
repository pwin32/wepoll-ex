# Release Notes

## 0.1.0 (unreleased)

This is an experimental preview of the extended epoll-shaped API, the Windows
IOCP/AFD backend, the POSIX development wrapper, and the optional nginx 1.31.3
adapter. The API and ABI may change before a stable release.

The July 23, 2026 validation run used Windows 10.0.19044 with MSYS2 MinGW GCC
15.2, plus POSIX/WSL builds with GCC 14.2 and Clang 19.1. Strict MinGW CTest
passed 40 combined, 39 static-only, and 12 shared-only entries; the
synchronized-lifetime combined build also passed 40. POSIX GCC and Clang each
passed 3 CTest entries, while ASan/UBSan passed 42 API and 5 pool/MPSC checks.
Installed-package consumers and dependency checks that reject a dynamic
`libwinpthread-1.dll` import passed. The nginx adapter passed a strict full
link, `nginx -t`, 100 loopback requests across a worker reload, and graceful
quit.

The Windows regression suite now also covers IPv6 listener/stream readiness,
send-buffer backpressure, and zero-timeout waits with more than one IOCP batch
of internal completion packets. Zero-timeout waits use a bounded internal
completion drain so an initial cancellation burst cannot hide a queued
readiness event or create an unbounded nonblocking loop.

Stable Windows registrations now defer their first AFD request until a waiter
arms the port. An active waiter and an unconnected transitional stream still
arm immediately. This lets independent epoll instances watch the same socket
without leaving idle AFD work behind. Pending MOD operations retain an
in-flight request when its mask already covers the new interest and deliver
the latest data/context; expansions cancel once and rearm. Transitional TCP
requests use a broad pre-connect mask so MOD-before-connect preserves the AFD
completion evidence used for endpoint-token adoption without creating an idle
rearm loop. A stable ADD can now defer an AFD submission error until the first
wait; active-waiter ADDs still report submission failures synchronously, with a
regression covering the retained registration and subsequent recovery.

POSIX extended waits now hold a stable duplicate of the epoll descriptor.
`wepoll_close()` wakes all blocked extended waiters, which fail with `EBADF`,
and waits for their metadata references before teardown. Context decoration is
suppressed (`user_ctx == NULL`) when duplicate opaque data values make a match
ambiguous or an extension control change overlaps the wait. Metadata now keeps
separate registrations when Linux retains distinct open file descriptions
under one reused numeric fd. MOD/DEL use an `fstat` fingerprint and return
`EOPNOTSUPP` instead of mutating an arbitrary registration when multiple
entries are indistinguishable. On POSIX, `epoll_fd_count()` explicitly counts
extension-owned registrations; native `epoll_ctl()` additions are outside
that view until an extension MOD adopts them. `epoll_pwait2_ex()` applies a
non-null signal mask atomically through native `epoll_pwait` after validating
and rounding the timeout.

Windows finite waits now retry an early `WAIT_TIMEOUT` against their absolute
deadline. When internal packets keep arriving, a zero-timeout wait processes
at least 16 successful, nonempty IOCP dequeue batches before enforcing a 10 ms
budget; any readiness found during the drain is still returned. Winsock
provider resolution uses `SIO_BASE_HANDLE` first and then guarded SELECT, POLL,
and generic BSP fallbacks, rejecting malformed responses and cycles while
continuing past one cyclic candidate if a later fallback advances the chain.
New regressions exercise UDP IPv4/IPv6 readiness, optional ICMP error delivery,
DEL/ADD reuse, provider fallback, concurrent controls, a 513-packet internal
burst, and an injected early timeout.

Small waits avoid allocation: Windows basic waits use a 64-event stack buffer,
and POSIX extended waits use 32 events before falling back to the heap. The
POSIX context lookup now uses a reverse data index, while Windows skips its
socket-list scan when no socket needs re-arming and no fired oneshot needs a
native-close probe. Under the synchronized-socket-lifetime contract, Windows
also reuses the base provider handle captured at ADD instead of resolving it
for every rearm; hardened builds continue to re-resolve it. The new POSIX-only
`bench_wait_scaling` target measures empty and one-ready extended waits as
registration count grows.

A quick nginx A/B used six alternating four-second HTTP/1.1 `h2load` pairs,
32 connections, two client threads, one nginx worker, and the `empty_gif`
handler. This tree measured a 79.9k requests/s median versus 78.5k for commit
`ebc247d`; individual paired deltas ranged from -4.4% to +4.1%. That spread is
local noise, so this run does not demonstrate a throughput improvement.

This validation is not a support matrix. MSVC and other Windows toolchains are
not yet validated, and AFD is undocumented. `_WIN32_WINNT=0x0602` is the
Windows 8-or-later compile/runtime assumption; Windows 8 itself was not tested.
`EPOLLET`, `EPOLLEXCLUSIVE`, and non-null signal masks are not supported by the
Windows backend. Performance measurements are local loopback observations, not
portable throughput guarantees.

See `README.md`, `docs/DESIGN.md`, and `docs/NGINX_INTEGRATION.md` for current
contracts and integration constraints.
