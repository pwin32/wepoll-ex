# Release Notes

## 0.1.0 (unreleased)

This is an experimental preview of the extended epoll-shaped API, the Windows
IOCP/AFD backend, the POSIX development wrapper, and the optional nginx 1.31.3
adapter. The API and ABI may change before a stable release.

The July 23, 2026 validation run used Windows 10.0.19044 with MSYS2 MinGW GCC
15.2, plus POSIX/WSL builds with GCC 14.2 and Clang 19.1. It covered strict
MinGW shared and static builds, installed-package consumers, and dependency
checks that reject a dynamic `libwinpthread-1.dll` import. The Windows tests
exercise readiness, socket reuse, cancellation, concurrent wait/close, fault
injection, and bounded teardown. The nginx adapter has been compile-checked and
smoke-tested with loopback HTTP traffic across a worker reload.

The Windows regression suite now also covers IPv6 listener/stream readiness,
send-buffer backpressure, and zero-timeout waits with more than one IOCP batch
of internal completion packets. Zero-timeout waits use a bounded internal
completion drain so an initial cancellation burst cannot hide a queued
readiness event or create an unbounded nonblocking loop.

This validation is not a support matrix. MSVC and other Windows toolchains are
not yet validated, and AFD is undocumented. `_WIN32_WINNT=0x0602` is the
Windows 8-or-later compile/runtime assumption; Windows 8 itself was not tested.
`EPOLLET`, `EPOLLEXCLUSIVE`, and non-null signal masks are not supported by the
Windows backend. Performance measurements are local loopback observations, not
portable throughput guarantees.

See `README.md`, `docs/DESIGN.md`, and `docs/NGINX_INTEGRATION.md` for current
contracts and integration constraints.
