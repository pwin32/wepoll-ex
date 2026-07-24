# Design Notes

## Status

This document describes an experimental prototype, not a compatibility or
performance specification. The Windows path depends on undocumented AFD
interfaces and is socket-only. The development wrapper is Linux-specific and
uses native `epoll` and `eventfd`; passing its tests does not validate the
IOCP/AFD engine.

## Build-time split

`CMakeLists.txt` selects one implementation set:

| Platform | Library sources | Purpose |
| --- | --- | --- |
| Windows | `wepoll_ex_global.c`, `wepoll_ex_errno.c`, `wepoll_ex_fault.c`, `wepoll_ex_afd.c`, `wepoll_ex_pool.c`, `wepoll_ex_port.c`, `wepoll_ex_api.c` | IOCP/AFD implementation |
| Linux | `wepoll_ex_fault.c`, `wepoll_ex_posix.c`, `wepoll_ex_pool.c` | Native-epoll wrapper and queue/pool tests |

Other operating systems fail configuration rather than compiling an assumed
generic POSIX backend. A top-level build defaults tests on and may default its
single-config build type to Release; an `add_subdirectory` consumer keeps the
parent build type and defaults wepoll-ex tests off.

Windows builds select `WEPOLL_EX_SOCKET_LIFETIME_MODE` as `best-effort`
(default), `strict`, or `synchronized`. The compiled policy is observable via
`wepoll_ex_get_socket_lifetime_policy()` and the per-port statistics snapshot.

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
   reports the error synchronously. Best-effort mode accepts a provider that
   cannot expose an endpoint token, strict mode rejects it with
   `EOPNOTSUPP`, and synchronized mode omits token queries entirely.
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
   registrations require MOD or `epoll_rearm()`. Failures discovered during
   completion processing or deferred rearming are latched and wake the wait
   path. Ready snapshots already in the queue are returned first; the pending
   error is reported by the next wait. Only the first currently pending error
   is returned, while the statistics counter records every occurrence.
5. DEL and close remove public lookup immediately, cancel pending AFD work,
   and retain `ep_sock_t` storage until its cancellation completion is
   consumed. Hardened submissions re-resolve the provider base before each
   request. In synchronized lifetime mode, the base captured at ADD is reused
   for rearms. When the provider exposes a stable
   WFP ALE endpoint token, a native close followed by immediate numeric
   `SOCKET` reuse retires the old registration before ADD/MOD/rearm can attach
   stale data to the replacement.
6. `wepoll_close()` marks the port closing, wakes waiters, and removes the
   logical epfd. Public API references and the first AFD-completion drain each
   have a five-second bound. If references remain, close returns
   `ETIMEDOUT`; the last operation to release its reference performs deferred
   destruction. If only completion draining times out while IOCP remains
   usable, a detached reaper owns the quarantined port for up to 60 seconds.
   A successful late drain frees it. An unusable IOCP, reaper failure, or
   second drain failure closes reachable handles and intentionally leaves the
   remaining storage unreachable rather than risking use-after-free. A failed
   close has still consumed the epfd and must not be retried.

The ready queue is single-consumer MPSC. Producers append without a mutex; the
consumer uses a sentinel before reclaiming nodes. Both AFD-buffer pools use a
mutex-protected LIFO and grow with tracked fallback allocations.

The Windows basic-wait adapter uses a 64-event stack buffer and allocates only
for larger batches. Intrusive rearm and fired-oneshot worklists make the arm
pass O(pending work), including the native-close probes required for fired
oneshots; it does not scan every registration. List membership and visit
counters are checked by internal invariants and a synthetic 50,000-node
worklist regression that does not consume Winsock handles.

## Linux path

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
uses native `epoll_pwait2` when the libc symbol is present and the kernel
supports it. A build-time absence or runtime `ENOSYS` falls back to
`epoll_pwait` for a supplied mask, or `epoll_wait` otherwise, rounding a
nonzero submillisecond timeout up. Pthread cancellation cleanup releases the
optional heap event buffer and metadata reference before unwinding.

## Supported semantics and boundaries

- Windows registrations accept Winsock sockets and waitable HANDLEs
  (`WaitForSingleObject`-compatible objects such as events).  Non-waitable
  files and pipes remain unsupported.  Waitable registrations use
  `RegisterWaitForSingleObject` + IOCP wakeups rather than AFD, reject
  `EPOLLEXCLUSIVE`, and map a signaled object to the requested
  `EPOLLIN`/`EPOLLOUT` interest bits.
- Linux `fstat` fingerprints are a conservative metadata aid, not a formal
  open-file-description identifier. Multiple matching registrations reject
  MOD/DEL/rearm with `EOPNOTSUPP`; a single stale fingerprint collision and a
  native close/reuse racing a control call still require caller
  synchronization. Use extension DEL before native close when accurate
  `epoll_fd_count()` results and later control operations are required; plain
  close cannot retire the user-space metadata immediately.
- Best-effort native socket reuse protection depends on
  `SIO_QUERY_WFP_ALE_ENDPOINT_HANDLE` when the provider exposes it. Providers
  without a token retain legacy numeric-handle behavior; a close racing the
  identity query and a post-connect control operation before transitional AFD
  completion still require caller synchronization. Strict mode rejects an
  unavailable token at ADD with `EOPNOTSUPP`; it also suppresses a queued
  snapshot after a transient delivery-path identity-query failure, schedules
  rearm, and reports the identity error through a later wait. Best-effort mode
  may deliver that already-ready snapshot. Control-path identity-query errors
  fail in both modes. Normal pre-connect registrations adopt a new token only
  after an AFD completion proves continuity; the covered MOD-before-connect
  path preserves that evidence.
- Synchronized lifetime mode is an explicit performance contract for embedders
  that guarantee `EPOLL_CTL_DEL` before every native socket close. Windows DEL
  removes public registration even when cancellation fails; storage remains
  pinned for a later completion or safe close-time quarantine. Native
  close/reuse identity tests are skipped because the caller owns that safety.
  The nginx addon can opt into this mode, but no longer selects it by default.
- Windows builds set `_WIN32_WINNT=0x0602`; Windows 8 or later is the current
  compile/runtime assumption, not a validated compatibility floor for every
  AFD revision.
- `EPOLLONESHOT`, context delivery, RDHUP mapping, zero-timeout waits, native
  socket close cleanup and stable numeric reuse, and concurrent epoll close
  have regression coverage.
- `EPOLLET` is implemented as an observed-edge filter over AFD level
  snapshots: each interest bit is delivered once while continuously true, then
  suppressed until it drops out of the latest level and reappears. Empty edge
  completions defer re-arming to the next wait so permanently ready sockets do
  not spin. `EPOLLEXCLUSIVE` may be set only at ADD (MOD with the flag returns
  `EINVAL`) and submits AFD polls with `Exclusive=TRUE` so peer wepoll-ex
  instances watching the same provider handle are cancelled on wake.
- The nginx adapter leaves `ngx_event_actions.notify` unset. nginx 1.31.3
  rejects `--with-threads` on Win32 and its thread-pool sources are POSIX-only,
  so thread-pool integration is outside this prototype's supported boundary.
- Windows signal masks are opaque API placeholders. Non-null masks are
  accepted and ignored so portable `epoll_pwait*` call sites run; Windows has
  no POSIX process signal mask to apply. Linux extended waits pass masks
  atomically to native `epoll_pwait2` or the `epoll_pwait` fallback.
- `EPOLLEXCLUSIVE` cannot be combined with `EPOLLONESHOT` or `EPOLLET` on
  ADD (`EINVAL`), matching Linux. `EPOLLWAKEUP` is accepted and ignored.
- `epoll_ctl_batch` best-effort rolls back successful ADDs after a later
  failure. Earlier MOD and DEL operations remain applied.
- Fail-at-N hooks are internal, process-global test symbols. They are absent
  from the installed header and shared-library export surface; normal builds
  leave every point disabled.
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
- Per-port statistics report current queue/pool depths and cumulative work,
  stale-event, identity, asynchronous-error, and zero-timeout-budget counters.
  Global statistics report quarantine, successful reaping, irrecoverable
  ports, and public-reference close timeouts. Size-prefixed snapshots support
  older/newer structure prefixes but are diagnostic rather than atomic.
- A Windows close timeout consumes the virtual descriptor. Recoverable drain
  timeouts are reaped in the background; irrecoverable state remains leaked.
  A public-reference timeout is completed by the final referencing operation.
  In every failure case, retrying the removed epfd is invalid.
- `bench_latency` validates every transfer/wait, returns nonzero on an
  incomplete run, and reports p50/p95/p99. `bench_wait_scaling` measures empty
  and one-ready Linux extension waits. Windows `bench_windows` emits CSV
  percentiles for registration scaling, ready batches, oneshot rearm, and
  armed control churn, with an optional 50,000-socket production profile.

## Verification baseline

The July 23, 2026 strict GCC and Clang Linux builds each passed all 3 CTest
entries. The API executable passes 42 behavior checks, and ASan/UBSan passes
the API and 5 pool/MPSC checks. Coverage includes duplicate-data and reused-fd
metadata, ambiguous identity rejection, signal masks, multi-waiter close wake,
and the installed-package consumer.

Strict MinGW GCC 15.2 passed 40 combined, 39 static-only, and 12 shared-only
CTest entries. The synchronized-lifetime combined build also passes all 40;
the UDP ICMP-error mode and four native close/reuse modes are expected skips
where their prerequisites do not apply. Coverage includes TCP/UDP/IPv6,
provider-chain fallback, same-socket multi-epfd waits, lazy ADD failure,
pending MOD narrowing/expansion, transitional connect continuity, lifecycle
faults, a 513-packet IOCP burst, early `WAIT_TIMEOUT`, backpressure, packaging,
and static-winpthread dependency checks.

The July 24 worktree adds strict and best-effort endpoint-policy injection,
fail-at-N allocation/AFD/IOCP/ready-node tests, a bounded public-reference
close test, recoverable quarantine/reaper coverage, pthread-cancellation
cleanup, native/fallback `epoll_pwait2` checks, and a replayable randomized
Windows public-API stress test. `scripts/qualify-posix.sh`,
`scripts/qualify-mingw.sh`, CMake presets, and `scripts/repeat-ctest.sh` make
those lanes reproducible. A fresh all-variant result should replace the older
counts above before a release tag.

The nginx 1.31.3 adapter passes a strict full Win32 link, dependency inspection,
`nginx -t`, 100 loopback requests across a worker reload, and graceful quit
using `use wepoll`. Six alternating four-second `h2load` pairs against commit
`ebc247d` measured medians of 79.9k requests/s for this tree and 78.5k for the
checkpoint; paired deltas ranged from -4.4% to +4.1%, so the local result is
performance-neutral rather than evidence of a throughput improvement. These
results still do not constitute a supported Windows/compiler matrix.
