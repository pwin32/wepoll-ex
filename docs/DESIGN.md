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

`WEPOLL_EX_BUILD_NGINX` adds an opt-in object compile check against a configured
nginx source/build tree. The tracked `nginx/config` hook separately registers
the adapter as an nginx EVENT addon and compiles the static wepoll-ex sources
into that disposable nginx build.

## Windows data flow and lifetime

1. `epoll_create*` creates an `ep_port_t`, IOCP, AFD poll state, pools, and a
   virtual integer `epfd` table entry.
2. `EPOLL_CTL_ADD` validates a Winsock socket, resolves its base provider
   handle, records an optional WFP ALE endpoint token for stable native-handle
   reuse detection, stores the requested data and context, and assigns a
   generation. Stable registrations defer the first asynchronous `AFD_POLL`
   until a waiter arms the port; an active waiter and an unconnected
   transitional stream submit immediately. Provider resolution first uses
   `SIO_BASE_HANDLE`, then walks distinct `SIO_BSP_HANDLE_SELECT`,
   `SIO_BSP_HANDLE_POLL`, and generic `SIO_BSP_HANDLE` results with cycle and
   depth guards. A pending MOD keeps a covering request and replaces its
   delivery snapshot; only a mask expansion cancels and rearms it. Therefore,
   an AFD submission error for an idle stable ADD is reported by the first
   wait that tries to arm it; an ADD made while a waiter is active still
   reports the error synchronously.
3. IOCP completions are translated to `EPOLL*` bits. Ready nodes snapshot the
   data, context, socket number, and generation; they never retain a raw socket
   pointer.
4. `epoll_wait*` serializes consumers because the ready queue is
   single-consumer, drains ready snapshots, waits for more IOCP packets, and
   skips stale generations. Lock acquisition is included in finite timeouts,
   and an early `WAIT_TIMEOUT` is retried against the absolute deadline. A
   zero-timeout call returns immediately if another waiter owns the drain.
   After it acquires the lock, while internal packets keep arriving it
   processes at least 16 successful, nonempty IOCP dequeue batches before
   enforcing a 10 ms drain budget.
   Level-triggered registrations are armed again on a later wait; oneshot
   registrations require MOD or `epoll_rearm()`.
5. DEL and close remove public lookup immediately, cancel pending AFD work,
   and retain `ep_sock_t` storage until its cancellation completion is
   consumed. Hardened submissions re-resolve the provider base before each
   request. Under `WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME`, the base
   captured at ADD is reused for rearms. When the provider exposes a stable
   WFP ALE endpoint token, a native close followed by immediate numeric
   `SOCKET` reuse retires the old registration before ADD/MOD/rearm can attach
   stale data to the replacement.
6. `wepoll_close()` marks the port closing, wakes waiters, waits for public API
   references to drain, and consumes outstanding completions before destroying
   handles and pools. Cancellation and IOCP draining are bounded; a permanent
   failure closes the logical epfd, reports an error, and quarantines the port
   storage rather than risking a late-completion use-after-free.

The ready queue is single-consumer MPSC. Producers append without a mutex; the
consumer uses a sentinel before reclaiming nodes. Both AFD-buffer pools use a
mutex-protected LIFO and grow with tracked fallback allocations.

The Windows basic-wait adapter uses a 64-event stack buffer and allocates only
for larger batches. Per-port `needs_rearm` and fired-oneshot counters let the
common wait path skip a full socket-list walk when no registration needs
provider work; the list is still scanned when either counter is nonzero.

## POSIX path

The host owns the basic epoll descriptor and readiness behavior.
`wepoll_ex_posix.c` maintains metadata for context, extension flags, counts,
and close/reuse detection. A hidden eventfd registration and duplicated epoll
descriptor give extended waits stable lifetime. `wepoll_close()` makes the
eventfd readable, closes the public descriptor, and waits for all metadata
references; blocked extended waits return `EBADF` and never expose the wake.
A hidden identity registration also distinguishes the tracked epoll generation
from a reused integer, so stale metadata is retired without closing a
replacement descriptor. Plain native `close()` cannot provide this wakeup or
metadata retirement contract; callers using extended waits should use
`wepoll_close()` and synchronize native close/reuse against it.

Native `epoll_event` values carry no fd or generation tag. The wrapper indexes
metadata by the opaque `data.u64` value and decorates only a unique match. If
multiple registrations share the value, or any extension metadata mutation
overlaps the wait, `user_ctx` is returned as `NULL` for the whole returned
batch rather than guessed from stale metadata. Multiple live registrations
with one reused numeric fd are retained separately; MOD/DEL use an `fstat`
fingerprint and return `EOPNOTSUPP` when that fingerprint is ambiguous.
`epoll_fd_count()` counts only registrations owned by the extension; a native
`epoll_ctl()` registration enters that view after an extension MOD. Extended
waits use a 32-event stack buffer and allocate only for larger batches.
`epoll_ctl_batch` is sequential and not transactional. `epoll_pwait2_ex`
rounds its timespec up to milliseconds and passes a non-null signal mask to
native `epoll_pwait`, preserving atomic signal-mask application.

## Supported semantics and boundaries

- Windows registrations accept Winsock sockets only, not files or arbitrary
  HANDLEs.
- POSIX `fstat` fingerprints are a conservative metadata aid, not a formal
  open-file-description identifier. Multiple matching registrations reject
  MOD/DEL/rearm with `EOPNOTSUPP`; a single stale fingerprint collision and a
  native close/reuse racing a control call still require caller
  synchronization. Use extension DEL before native close when accurate
  `epoll_fd_count()` results and later control operations are required; plain
  close cannot retire the user-space metadata immediately.
- Native socket reuse protection is best-effort: providers without
  `SIO_QUERY_WFP_ALE_ENDPOINT_HANDLE`, a close racing the identity query, and
  a post-connect control operation before the transitional AFD completion
  still require caller synchronization. Normal pre-connect registrations
  adopt the new endpoint token only after an AFD completion proves continuity;
  the covered MOD-before-connect path preserves that evidence. Stable
  connected/listening TCP and UDP sockets are covered directly.
- Embedders that guarantee `EPOLL_CTL_DEL` before every native socket close may
  define `WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME` to omit endpoint-token
  probes. Windows DEL removes the public registration even when cancellation
  of an in-flight AFD request fails; storage remains pinned for a later
  completion or safe close-time quarantine. Native close/reuse identity tests
  are skipped under this contract because the caller has assumed that
  synchronization responsibility. The nginx addon uses this contract-specific
  optimization.
- Windows builds set `_WIN32_WINNT=0x0602`; Windows 8 or later is the current
  compile/runtime assumption, not a validated compatibility floor for every
  AFD revision.
- `EPOLLONESHOT`, context delivery, RDHUP mapping, zero-timeout waits, native
  socket close cleanup and stable numeric reuse, and concurrent epoll close
  have regression coverage.
- `EPOLLET` and `EPOLLEXCLUSIVE` are rejected with `EOPNOTSUPP`. Silently
  treating AFD's one-shot level notification as an edge would duplicate unread
  readiness.
- The nginx adapter leaves `ngx_event_actions.notify` unset. nginx 1.31.3
  rejects `--with-threads` on Win32 and its thread-pool sources are POSIX-only,
  so thread-pool integration is outside this prototype's supported boundary.
- Windows signal masks are opaque API placeholders; non-null masks are
  rejected with `EOPNOTSUPP`. POSIX extended waits pass masks atomically to
  native `epoll_pwait` after validating and rounding the timespec.
- `epoll_ctl_batch` best-effort rolls back successful ADDs after a later
  failure. Earlier MOD and DEL operations remain applied.
- Timestamps use `QueryPerformanceCounter` on Windows and have an unspecified
  monotonic origin.
- After it acquires the single-consumer drain lock, a zero-timeout Windows
  wait probes IOCP. While only internal or cancellation packets keep arriving,
  it processes at least 16 successful, nonempty dequeue batches before
  stopping once a 10 ms wall-clock budget expires. Readiness found during that
  drain is returned even at the budget boundary. The combined minimum and
  deadline avoid both shallow batch limits and an unbounded nonblocking loop.
- Cancelled registrations remain internally allocated until a wait or close
  drains their IOCP completion, although they are absent from public lookup.
- Concurrent finite waits include time spent waiting for the single-consumer
  drain lock. A zero-timeout call may return no events while another waiter is
  draining them.
- If Windows shutdown cannot safely drain an outstanding AFD request, the
  unreachable port is intentionally leaked and `wepoll_close()` returns an
  error; retrying the removed epfd is invalid.
- The checked-in benchmarks exercise the POSIX wrapper only. `bench_latency`
  measures the basic native wait path; `bench_wait_scaling` measures empty and
  one-ready `epoll_wait_ex` calls as registration count grows. Disposable nginx
  loopback comparisons are recorded separately when the Windows adapter is
  evaluated.

## Verification baseline

The July 23, 2026 strict GCC and Clang POSIX builds each pass all 3 CTest
entries. The API executable passes 42 behavior checks, and ASan/UBSan passes
the API and 5 pool/MPSC checks. Coverage includes duplicate-data and reused-fd
metadata, ambiguous identity rejection, signal masks, multi-waiter close wake,
and the installed-package consumer.

Strict MinGW GCC 15.2 passes 40 combined, 39 static-only, and 12 shared-only
CTest entries. The synchronized-lifetime combined build also passes all 40;
the UDP ICMP-error mode and four native close/reuse modes are expected skips
where their prerequisites do not apply. Coverage includes TCP/UDP/IPv6,
provider-chain fallback, same-socket multi-epfd waits, lazy ADD failure,
pending MOD narrowing/expansion, transitional connect continuity, lifecycle
faults, a 513-packet IOCP burst, early `WAIT_TIMEOUT`, backpressure, packaging,
and static-winpthread dependency checks.

The nginx 1.31.3 adapter passes a strict full Win32 link, dependency inspection,
`nginx -t`, 100 loopback requests across a worker reload, and graceful quit
using `use wepoll`. Six alternating four-second `h2load` pairs against commit
`ebc247d` measured medians of 79.9k requests/s for this tree and 78.5k for the
checkpoint; paired deltas ranged from -4.4% to +4.1%, so the local result is
performance-neutral rather than evidence of a throughput improvement. These
results still do not constitute a supported Windows/compiler matrix.
