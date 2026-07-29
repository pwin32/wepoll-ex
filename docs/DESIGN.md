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

Until the ABI is frozen, installed CMake packages use `ExactVersion`
compatibility and ELF shared libraries use the full project version as their
SONAME. The package consumer rejects an older non-exact 0.x request, while an
export allowlist rejects accidental public symbols on both Linux and MinGW.
The tracked public `wepoll_ex_version.h` definitions are canonical: CMake
parses their three decimal components before `project()`, and both backends
derive their runtime number and string from the same macros. This also keeps
the nginx-embedded source build independent of a generated CMake header.

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
3. Socket IOCP completions translate both the AFD per-handle `Events` bits and
   its `Status`; a negative per-handle status contributes `EPOLLERR` even when
   the event bitset is empty. Ready nodes snapshot the data, context, socket
   number, and generation; they never retain a raw socket pointer.
4. `epoll_wait*` serializes consumers because the ready queue is
   single-consumer, drains ready snapshots, waits for more IOCP packets, and
   skips stale generations. Lock acquisition is included in finite timeouts,
   and an early `WAIT_TIMEOUT` is retried against the absolute deadline. A
   zero-timeout call returns immediately if another waiter owns the drain.
   After it acquires the lock, while internal packets keep arriving it
   processes at least 16 successful, nonempty IOCP dequeue batches before
   enforcing a 10 ms drain budget.
   Positive finite timespec waits lazily create a high-resolution waitable
   timer and thread-pool wait when the required Windows APIs are available.
   The requested duration is rounded up to 100-nanosecond timer units; the
   callback posts a generation-tagged IOCP sentinel, and stale generations
   are consumed without touching socket storage. An independently rounded-up
   millisecond IOCP deadline remains active as a safety backstop. Capability,
   initialization, arm, or callback-post failure therefore degrades to the
   coarse path without changing the public result. Timer resolution does not
   guarantee scheduler wake latency. Long finite waits are accepted and use
   bounded IOCP chunks rather than overflowing a `DWORD` timeout. Integer
   `epoll_wait` and `epoll_pwait` retain their millisecond contract. Durations
   too large for the internal 100-nanosecond representation bypass the precise
   timer and retain the longer millisecond/chunked deadline.
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

At most four detached reapers may run concurrently. Operational qualification
requires `active_quarantines` and `irrecoverable_ports` to return to zero after
the workload; otherwise the process may retain unreachable port storage by
design.

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
supports it. A build-time absence, runtime `ENOSYS`, or
`WEPOLL_EX_FORCE_EPOLL_PWAIT2_FALLBACK=ON` falls back to `epoll_pwait` for a
supplied mask, or `epoll_wait` otherwise, rounding a nonzero submillisecond
timeout up. Valid durations beyond the `int` millisecond range are divided
into `INT_MAX`-millisecond chunks. For a supplied mask, catchable signals stay
blocked between chunks and each `epoll_pwait` atomically installs the requested
mask; cleanup restores the caller's original mask on return and cancellation.
This preserves thread-directed delivery, but a process-directed signal may be
routed to another eligible thread during the userspace inter-chunk bridge; a
multi-syscall fallback cannot make that whole logical wait kernel-atomic.
The forced mode has a dedicated strict preset and qualification build.
Pthread cancellation cleanup releases the optional heap event buffer and
metadata reference before unwinding.

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
  preserved across that retry. The registration owns such a notification from
  before an immediate consumptive probe or callback post until a ready node is
  queued. Cancellation, callback retirement, ready-node allocation failure,
  and metadata rollback do not clear that ownership; a retry re-posts the
  observation before probing an object whose state may already have been
  consumed.
  Manual-reset events and process/thread termination provide persistent
  level behavior. Manual-reset event ET uses throttled reset detection;
  process/thread termination is monotonic, so its delivered ET registration
  stays idle until a later MOD rather than polling a level that cannot clear.
  Auto-reset events and semaphore counts are consumed once per delivered
  readiness notification, including one ET notification per consumed
  signal/count. A waitable whose effective readiness interest is zero is
  dormant: it has no probe, registered wait, rearm work, or ET holdoff. MOD from
  active interest to zero synchronously disarms the wait and is transactional
  if disarm fails. A callback can win just before that MOD linearizes; Windows
  cannot restore the signal/count to the object, so the logically dormant
  registration retains ownership and replays it if interest is enabled later.
  A posted canceled packet may outlive logical dormancy only long enough to
  retire IOCP accounting. If MOD or an early `epoll_rearm()` invalidates a
  queued consumptive/mode-unknown ready node, ownership returns to the
  registration; the next active generation replays it with current metadata
  before any new HANDLE probe. Other pending waitable or pipe operations cover
  every MOD mask and complete against the latest metadata. The conservative
  mode-unknown case covers synchronization
  timers and events whose reset mode could not be queried. Queued pipe readiness
  need not be preserved this way: it consumes no state and is re-evaluated from
  the current level after the stale snapshot is discarded. Flags-only
  `EPOLLET` is accepted while such a waitable is dormant. Waitable-timer ET and
  ET on an event whose reset mode cannot be queried are rejected with `EINVAL`
  once nonzero interest is requested; LT remains supported.
- Pipes use short timer-queue polls because anonymous pipe handles are not
  reliably waitable. Each poll combines
  `NtQueryInformationFile(FilePipeLocalInformation)` state, readable-byte, and
  writable-quota fields with the existing `PeekNamedPipe` fallback and the
  ADD-time granted-access classification. Readable data produces only the
  requested `EPOLLIN`/`EPOLLRDNORM` aliases. A read endpoint whose writer has
  closed retains those aliases while buffered data remains and adds unrequested
  `EPOLLHUP`; after the buffer drains, or when EOF starts empty, the result is
  `EPOLLHUP` alone. A connected write endpoint reports the requested
  `EPOLLOUT`/`EPOLLWRNORM` aliases while quota remains; a write endpoint whose
  reader has closed retains those requested aliases and adds unrequested
  `EPOLLERR`, without `EPOLLHUP`. Pipes do not produce `EPOLLRDBAND` or
  `EPOLLWRBAND`.
- Pipe ET normally reports only readiness aliases that rose since the previous
  valid sample, so a duplex registration does not repeat `OUT` merely because
  `IN` appeared or disappeared. A newly raised terminal condition includes the
  current normal aliases, and draining buffered EOF produces one final HUP
  snapshot; together these preserve `IN` -> `IN|HUP` -> `HUP` and
  `OUT` -> `OUT|ERR`. A rejected or unavailable native metadata sample does not
  clear the edge latch. A natively identified terminal client end stops polling
  after its stable final snapshot has been delivered. A named-pipe server end
  remains eligible for resampling because one HANDLE can serve another client
  after `DisconnectNamedPipe()` and `ConnectNamedPipe()`; readiness on the
  replacement client forms a fresh edge without MOD. Fallback-derived ends are
  also sampled conservatively because their client/server role is unknown. A
  peer-closed ONESHOT registration remains installed so MOD or `epoll_rearm()`
  can rearm it. Pending MOD and both anonymous and named pipe handles remain
  supported.
  Read/write readiness is filtered through the HANDLE's granted data access, so
  a read-only endpoint cannot report writable and a write-only endpoint cannot
  report readable.
- Writable backpressure and restoration normally follow quota exhaustion in
  `FILE_PIPE_LOCAL_INFORMATION`. If the native query is unavailable or returns
  `STATUS_ACCESS_DENIED`, the adapter uses `PeekNamedPipe`; write-only handles
  can then retain advisory writable readiness and peer closure may be
  indistinguishable. Pure write-only outbound named-pipe server handles are the
  known access-denied case. Other native-query errors produce no fabricated
  readiness and are retried by the timer path. Overlapped pipe HANDLEs use the
  same synchronous metadata snapshots, independently of application
  `OVERLAPPED` operations. Polling is a compatibility path, not a high-scale
  substitute for overlapped application I/O. Pipe and waitable registrations
  reject `EPOLLEXCLUSIVE`.
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
- AFD receive and accept map to the ordinary readable class; send maps to the
  ordinary writable class; and expedited receive maps to urgent data. The
  public result is filtered to the requested aliases: `EPOLLIN` and
  `EPOLLRDNORM` share one class, `EPOLLOUT`, `EPOLLWRNORM`, and `EPOLLWRBAND`
  share one class, and `EPOLLPRI` and `EPOLLRDBAND` share one class. A graceful
  disconnect reports readable EOF plus `EPOLLRDHUP` without `EPOLLERR` or
  `EPOLLHUP`. A TCP abortive close reports unrequested `EPOLLERR | EPOLLHUP`;
  an abort on a socket whose cached protocol metadata is an exact IPv4/IPv6
  UDP match reports `EPOLLERR` without HUP; and connect failure reports the
  requested readable/writable aliases plus unrequested `EPOLLERR | EPOLLHUP`.
  A negative AFD per-handle status reports unrequested `EPOLLERR`
  independently of the event bits. Missing or ambiguous provider protocol
  metadata retains the conservative abort mapping with HUP.
- TCP urgent-data readiness is qualified for LT persistence until
  `recv(MSG_OOB)`, observed ET suppression and re-edge, ONESHOT MOD rearm, and
  MOD filtering/data replacement. With `SO_OOBINLINE`, the urgent byte is
  qualified as ordinary `EPOLLIN`, is consumed by normal `recv()`, persists in
  LT, and re-edges under the observed ET rule. AFD does not expose a separate
  expedited class in that mode, so Windows does not also report Linux's
  `EPOLLPRI` indication. `EPOLLMSG` is accepted but AFD has no event class that
  produces it. UDP IPv4/IPv6 readiness is covered publicly; connected-UDP ICMP
  error delivery is also checked when
  `SIO_UDP_CONNRESET`, the provider, and host firewall expose it, and any
  observed event must contain `EPOLLERR` without `EPOLLHUP`.
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
  atomically to native `epoll_pwait2` or the chunked `epoll_pwait` fallback.
- Windows `epoll_pwait2*` represents positive finite durations in upward-
  rounded 100-nanosecond timer units when high-resolution timers are available,
  with an upward-rounded millisecond IOCP backstop and transparent coarse
  fallback. This improves requested deadline resolution but does not guarantee
  an equivalent scheduler wake latency. Windows virtual epoll descriptors also
  cannot be nested as monitored objects inside another epoll instance.
- `SO_OOBINLINE` exposes urgent bytes as ordinary readable data without a
  separate `EPOLLPRI` bit on Windows.
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

On July 29, 2026, strict MinGW GCC 15.2 with
`-O2 -Wall -Wextra -Wpedantic -Werror` completed 124 combined best-effort, 122
static-only, 65 shared-only, 124 strict-identity, 65 strict shared-only, 124
synchronized-lifetime, and 65 synchronized shared-only CTest entries. Their
passed/skipped counts were 123/1, 121/1, 64/1, 123/1, 64/1, 119/5, and 60/5.
All seven variants skipped the environment-dependent UDP/ICMP case;
synchronized modes additionally skipped the four native-reuse identity cases
owned by their DEL-before-close contract. Three repeats of every applicable
API, backpressure, stress, concurrent-control, AFD mapping/status,
socket-alias, urgent-data LT/ET/ONESHOT/MOD, inline-urgent LT/ET, and precise-
timeout conversion/generation/readiness/close/fallback lane passed. The shared
builds also passed exact public-export checks.

The same worktree passed Linux/WSL GCC 14.2 strict Release CTest 4/4, five
repeats each of the API and pool executables, an explicitly forced
`epoll_pwait2` fallback CTest 4/4, and ASan/UBSan CTest 3/3. Clang 19.1.7 strict
Release also passed 4/4. Coverage includes exact preview package compatibility,
ELF SONAME and Linux/MinGW export surfaces; socket alias, urgent-data,
status/error, ET/exclusive read, write, mixed-class, and stale-snapshot
transitions; direction-aware pipe adapters; waitable terminal ET and
pending/queued MOD races; consumptive notification counts; auxiliary-disarm
fault recovery and preserved consumptive retries; immediate auxiliary
cancellation reclamation; IOCP post/close lease races and fatal-post wakeups;
high-resolution timeout conversion, stale generations, readiness races,
fallback, close, and init/arm/post failures; long native/fallback timespecs and
masked cancellation/restoration; provider identity modes;
cancellation/close/quarantine; packaging; and static-winpthread dependency
checks. `scripts/qualify-posix.sh`,
`scripts/qualify-mingw.sh`, CMake presets, and `scripts/repeat-ctest.sh` make
those lanes reproducible.

The deterministic long stress profile completed 250,000 operations on 128
sockets with zero backpressure in combined best-effort and best-effort,
strict, and synchronized shared-library builds. The production benchmark also
completed all 13 CSV rows for those four builds at a 50,000-socket maximum and
1,000 timed iterations. This is registration scaling plus ready batches up to
512, not a 50,000-socket armed-wait result, and no performance threshold is
claimed.

The nginx 1.31.3 adapter passes a strict full Win32 link, dependency inspection,
`nginx -t`, 100 loopback requests across a worker reload, and graceful quit
using `use wepoll`. Six alternating four-second `h2load` pairs against commit
`ebc247d` measured medians of 79.9k requests/s for this tree and 78.5k for the
checkpoint; paired deltas ranged from -4.4% to +4.1%, so the local result is
performance-neutral rather than evidence of a throughput improvement. These
results still do not constitute a supported Windows/compiler matrix.
