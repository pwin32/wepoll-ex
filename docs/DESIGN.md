# Design Notes

## Status

This document describes an experimental prototype, not a compatibility or
performance specification. The Windows socket path depends on undocumented AFD
interfaces; waitable kernel objects and pipes use separate IOCP/thread-pool
adapters. The development wrapper is Linux-specific and uses native `epoll`
and `eventfd`; passing its tests does not validate the Windows engine.

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
5. DEL and close remove public lookup immediately. Pending AFD requests and
   auxiliary registrations with an already-posted IOCP packet retain
   `ep_sock_t` storage until completion is consumed. Blocking auxiliary disarm
   retires immediately when no packet was posted, balancing pending accounting
   without manufacturing a cancellation packet. Hardened submissions
   re-resolve the provider base before each request. In synchronized lifetime
   mode, the base captured at ADD is reused for rearms. When the provider
   exposes a stable
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
   remaining storage unreachable rather than risking use-after-free. Callback
   and control posts hold a per-port IOCP lease; close revokes the posting alias
   before `CloseHandle`, preventing a late callback from targeting a reused
   numeric HANDLE. An unexpected post failure closes the IOCP to wake blocked
   waiters and latches the original error. A failed close has still consumed
   the epfd and must not be retried.

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

- Windows registrations accept Winsock sockets, anonymous/named pipes, and
  selected waitable kernel objects: events, semaphores, waitable timers,
  processes, and threads. Object types are identified without a
  destructive zero-time wait, so an initially signaled auto-reset event or
  semaphore is not consumed by ADD. Mutexes and other unsupported kernel
  objects, including jobs, ordinary disk files, and character handles, return
  `EPERM` (the Linux
  `epoll_ctl` error for a target that cannot be monitored).
- Waitable registrations use `RegisterWaitForSingleObject` and post at most
  one IOCP packet per arm. Successful delivery and cancellation synchronously
  retire the one-shot wait before registration storage can be reclaimed. If
  retirement fails, the registration and pending count remain pinned, the
  error is surfaced asynchronously, and a later wait/DEL/close retries; a
  persistent close-time failure follows the existing quarantine path. A
  consumed auto-reset event, semaphore count, or mode-unknown notification is
  preserved across that retry. Successful retry re-posts the observation
  rather than probing an object whose state may already have been consumed.
  Manual-reset events and process/thread termination provide persistent
  level behavior. Manual-reset event ET uses throttled reset detection;
  process/thread termination is monotonic, so its delivered ET registration
  stays idle until a later MOD rather than polling a level that cannot clear.
  Auto-reset events and semaphore counts are consumed once per delivered
  readiness notification, including one ET notification per consumed
  signal/count. A pending waitable or pipe operation covers every MOD mask and
  completes against the latest metadata. If MOD races a ready notification
  from a known-consumptive or mode-unknown waitable, the old queued generation
  is replaced before rearming so a consumed signal/count/timer expiration is
  still delivered with current metadata. The conservative mode-unknown case
  covers synchronization timers and events whose reset mode could not be
  queried. Queued pipe readiness need not be preserved this way: it consumes
  no state and is re-evaluated from the current level after the stale snapshot
  is discarded.
  Waitable-timer ET and ET on an event whose reset mode cannot be queried are
  rejected with `EINVAL`; LT remains supported.
- Pipes use short timer-queue polls with `PeekNamedPipe` because anonymous pipe
  handles are not reliably waitable. This supports read readiness, EOF/HUP,
  level, observed-edge, oneshot, pending MOD, and both anonymous and named pipe
  handles. Read/write readiness is filtered through the HANDLE's granted data
  access, so a read-only endpoint cannot report writable and a write-only
  endpoint cannot report readable. Writable readiness is still advisory on
  Windows because `PeekNamedPipe` does not expose exact remaining write quota.
  Polling is a compatibility path, not a high-scale substitute for overlapped
  application I/O. Pipe and waitable registrations reject `EPOLLEXCLUSIVE`.
- Applications must issue DEL before `CloseHandle()` for a registered pipe or
  waitable object. The socket identity policies do not extend to arbitrary
  HANDLE reuse.
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
- Socket `EPOLLET` is implemented as an observed-edge filter over AFD level
  snapshots: each interest bit is delivered once while continuously true, then
  suppressed until it drops out of the latest level and reappears. Empty edge
  observations and losing exclusive claims use a short deferred retry instead
  of a tight immediate-completion loop. Pipe and waitable ET use the adapter-
  specific readiness observations described above.
- `EPOLLEXCLUSIVE` applies only to socket registrations and may be set only by
  ADD. It may be combined with `EPOLLET`, but not with `EPOLLONESHOT`,
  `EPOLLRDHUP`, or unsupported event bits. Every MOD of a registration added
  exclusive fails with `EINVAL`, even if the MOD mask omits `EPOLLEXCLUSIVE`.
  AFD requests use `Exclusive=TRUE`; a process-wide claim filter tracks read,
  write, and terminal readiness classes independently and removes only the
  conflicting classes from mixed snapshots. A covering AFD submission that
  returns `STATUS_PENDING`, or a zero-time sample proving one direction
  inactive, releases the corresponding class claims. Each live registration
  embeds one intrusive claim node whose class bitset is indexed through fixed
  hash buckets. Claim capacity therefore grows with registrations and the
  delivery path does not allocate or fail open, while one process-wide mutex
  serializes cross-port ownership changes.
- The nginx adapter leaves `ngx_event_actions.notify` unset. nginx 1.31.3
  rejects `--with-threads` on Win32 and its thread-pool sources are POSIX-only,
  so thread-pool integration is outside this prototype's supported boundary.
- Windows signal masks are opaque API placeholders. Non-null masks are
  accepted and ignored so portable `epoll_pwait*` call sites run; Windows has
  no POSIX process signal mask to apply. Linux extended waits pass masks
  atomically to native `epoll_pwait2` or the `epoll_pwait` fallback.
- Windows `epoll_pwait2*` rounds a positive submillisecond timeout up to one
  millisecond because the IOCP dequeue API accepts millisecond timeouts.
  Windows virtual epoll descriptors also cannot be nested as monitored objects
  inside another epoll instance.
- `EPOLLWAKEUP` is accepted and ignored on Windows.
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
- Cancelled AFD registrations and auxiliary registrations with an already-
  posted packet remain internally allocated until a wait or close drains their
  IOCP completion, although they are absent from public lookup. An auxiliary
  registration whose blocking disarm finds no posted packet retires and
  balances its pending count immediately.
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

On July 27, 2026, strict MinGW GCC 15.2 with
`-O2 -Wall -Wextra -Wpedantic -Werror` completed 107 combined best-effort, 106
static-only, 54 shared-only, 107 strict-identity, and 107 synchronized-lifetime
CTest entries. The combined/static/strict/synchronized lanes had the expected
environment-dependent UDP/ICMP skip; synchronized mode also skipped the four
native-reuse identity cases owned by its DEL-before-close contract. Repeated
API, backpressure, stress, and concurrent-control lanes passed in every
applicable variant.

The same worktree passed Linux/WSL GCC 14.2 strict Release CTest 3/3, repeated
API/pool runs, and ASan/UBSan CTest 3/3. Clang 19.1.7 strict Release also passed
3/3. Coverage includes socket ET/exclusive read, write, mixed-class, and stale
snapshot transitions; direction-aware pipe adapters; waitable terminal ET and
pending/queued MOD races; consumptive notification counts; auxiliary-disarm
fault recovery and preserved consumptive retries; immediate auxiliary
cancellation reclamation; IOCP post/close lease races and fatal-post wakeups;
provider identity modes; cancellation/close/quarantine; packaging; and
static-winpthread dependency checks. `scripts/qualify-posix.sh`,
`scripts/qualify-mingw.sh`, CMake presets, and `scripts/repeat-ctest.sh` make
those lanes reproducible.

The nginx 1.31.3 adapter passes a strict full Win32 link, dependency inspection,
`nginx -t`, 100 loopback requests across a worker reload, and graceful quit
using `use wepoll`. Six alternating four-second `h2load` pairs against commit
`ebc247d` measured medians of 79.9k requests/s for this tree and 78.5k for the
checkpoint; paired deltas ranged from -4.4% to +4.1%, so the local result is
performance-neutral rather than evidence of a throughput improvement. These
results still do not constitute a supported Windows/compiler matrix.
