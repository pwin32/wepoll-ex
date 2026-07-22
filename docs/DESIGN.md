# Design Notes

## Status

This document describes an experimental prototype, not a compatibility or
performance specification. The Windows path depends on undocumented AFD
interfaces and is socket-only. The POSIX path wraps native `epoll`; passing its
tests does not validate the IOCP/AFD engine.

## Build-time split

`CMakeLists.txt` selects one implementation set:

| Platform | Library sources | Purpose |
| --- | --- | --- |
| Windows | `wepoll_ex_global.c`, `wepoll_ex_errno.c`, `wepoll_ex_afd.c`, `wepoll_ex_pool.c`, `wepoll_ex_port.c`, `wepoll_ex_api.c` | IOCP/AFD implementation |
| POSIX | `wepoll_ex_posix.c`, `wepoll_ex_pool.c` | Native-epoll wrapper and queue/pool tests |

`WEPOLL_EX_BUILD_NGINX` currently emits a warning and creates no target. The
adapter requires the separate validation described in
[`NGINX_INTEGRATION.md`](NGINX_INTEGRATION.md).

## Windows data flow and lifetime

1. `epoll_create*` creates an `ep_port_t`, IOCP, AFD control handle, pools, and
   a virtual integer `epfd` table entry.
2. `EPOLL_CTL_ADD` validates a Winsock socket, stores the requested data and
   context, assigns a generation, and submits one asynchronous `AFD_POLL`.
3. IOCP completions are translated to `EPOLL*` bits. Ready nodes snapshot the
   data, context, socket number, and generation; they never retain a raw socket
   pointer.
4. `epoll_wait*` serializes consumers, drains ready snapshots, waits for more
   IOCP packets, and skips stale generations. Level-triggered registrations
   are armed again on a later wait; oneshot registrations require MOD or
   `epoll_rearm()`.
5. DEL and close remove public lookup immediately, cancel pending AFD work,
   and retain `ep_sock_t` storage until its cancellation completion is
   consumed. A later registration may safely reuse the same socket value.
6. `wepoll_close()` marks the port closing, wakes waiters, waits for public API
   references to drain, consumes all outstanding completions, and only then
   destroys handles and pools.

The ready queue is single-consumer MPSC. Producers append without a mutex; the
consumer uses a sentinel before reclaiming nodes. Both AFD-buffer pools use a
mutex-protected LIFO and grow with tracked fallback allocations.

## POSIX path

The host owns the basic epoll descriptor and readiness behavior.
`wepoll_ex_posix.c` maintains metadata for context, extension flags, fd counts,
and safe close/reuse detection. `epoll_ctl_batch` is sequential and not
transactional. `epoll_pwait2_ex` converts its timeout to milliseconds; a
non-null signal mask is not implemented by the extension path.

## Supported semantics and boundaries

- Windows registrations accept Winsock sockets only, not files or arbitrary
  HANDLEs.
- `EPOLLONESHOT`, context delivery, RDHUP mapping, zero-timeout waits, native
  socket close cleanup, and concurrent epoll close have regression coverage.
- `EPOLLET` and `EPOLLEXCLUSIVE` are rejected with `EOPNOTSUPP`. Silently
  treating AFD's one-shot level notification as an edge would duplicate unread
  readiness.
- Windows signal masks are opaque API placeholders; non-null masks are
  rejected with `EOPNOTSUPP`.
- `epoll_ctl_batch` best-effort rolls back successful ADDs after a later
  failure. Earlier MOD and DEL operations remain applied.
- Timestamps use `QueryPerformanceCounter` on Windows and have an unspecified
  monotonic origin.
- Cancelled registrations remain internally allocated until a wait or close
  drains their IOCP completion, although they are absent from public lookup.
- The benchmark exercises the POSIX wrapper only.

## Verification baseline

Strict warnings-as-errors builds pass on GCC/POSIX and MinGW. POSIX CTest covers
the API, MPSC/pool behavior, and installed-package consumer. Windows CTest
covers loopback readiness, context changes, oneshot/rearm, invalid arguments,
batch rollback and immediate reuse, descriptor-table collisions, native socket
close, concurrent close/wait, and the installed-package consumer. Shared,
static, and shared-only MinGW configurations are exercised; this is still not
a supported Windows version/compiler matrix.
