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

Windows builds also provide `WEPOLL_EX_BUILD_EPOLL_COMPAT`, an interface target
exported as `wepoll_ex::epoll_compat`. Its isolated include root contains
`<sys/epoll.h>` and links the selected library. The header is installed below
`include/wepoll-ex-compat`, so the ordinary package target does not shadow a
platform header merely by being linked.

Until the ABI is frozen, installed CMake packages use `ExactVersion`
compatibility and ELF shared libraries use the full project version as their
SONAME. The package consumer rejects an older non-exact 0.x request, while an
export allowlist rejects accidental public symbols on both Linux and MinGW.
The tracked public `wepoll_ex_version.h` definitions are canonical: CMake
parses their three decimal components before `project()`, and both backends
derive their runtime number and string from the same macros. This also keeps
the nginx-embedded source build independent of a generated CMake header.

## Windows data flow and lifetime

1. `epoll_create*` creates an `ep_port_t`, IOCP, AFD poll state, pools, an
   `epfd_shared_t` ownership record, and its first virtual integer table entry.
   `wepoll_ex_dup()` adds another table entry referencing the same shared
   record. Operations acquire aggregate public references from that record, so
   removing one alias cannot invalidate a wait or control call already using
   another. Standard `epoll_create(size)` requires a
   positive size but ignores its value. The extension `epoll_create_ex()` uses
   a positive size only as an initial fd-table and pool-capacity hint whose
   value is capped at 4096; it is not a registration limit. The POSIX extension
   accepts and ignores the same hint.
2. Windows control calls classify a target non-destructively before applying
   the operation. An invalid or closed target returns `EBADF`; a valid socket,
   pipe, or waitable that is absent from the port returns `ENOENT` for MOD,
   DEL, or `epoll_rearm()`; and a valid unsupported object returns `EPERM`.
   ADD additionally surfaces registration-specific access and provider errors,
   including `EACCES` for a waitable without `SYNCHRONIZE`. For every numeric
   operation other than `EPOLL_CTL_DEL`, the event value is copied once at API
   entry and a null pointer returns `EFAULT` before epfd, target, or operation
   validation. DEL does not inspect its event pointer. Public failures also
   update a thread-local `wepoll_ex_error_info`: `portable_error` is the errno
   result, an exact Win32/Winsock/NTSTATUS source is retained where available,
   and a separately flagged canonical Winsock equivalent is supplied when
   meaningful. A normalized mapping without the exact flag is not represented
   as the original native source. The versioned getter preserves this channel
   on success.
3. `EPOLL_CTL_ADD` validates a Winsock socket, resolves its base provider
   handle, records an optional WFP ALE endpoint token for stable native-handle
   reuse detection, stores the requested data and context, assigns a
   generation, and submits the first asynchronous `AFD_POLL` before returning.
   Initial socket submission failures therefore fail ADD synchronously, and a
   successful idle registration retains readiness that occurs before the first
   wait. Waitable and pipe registrations remain lazy without an active waiter
   so ADD does not consume a notification or begin timer polling. An idle
   socket owns an AFD IRP while its poll remains pending; an immediately
   satisfied idle poll is consumed synchronously, discarded, and left queued
   for a fresh first-wait submission. Same-socket registrations across epoll
   ports may also own temporary duplicate/reservation HANDLEs. Provider
   resolution first uses
   `SIO_BASE_HANDLE`, then walks distinct `SIO_BSP_HANDLE_SELECT`,
   `SIO_BSP_HANDLE_POLL`, and generic `SIO_BSP_HANDLE` results with cycle and
   depth guards. A pending MOD keeps a covering request and replaces its
   delivery snapshot; only a mask expansion cancels and rearms it. If
   cancellation loses to a completion already queued in IOCP, the request
   remains pending until completion refreshes any newly uncovered classes, so
   a same-wait MOD cannot publish a partial AFD snapshot. Best-effort mode
   accepts a provider that
   cannot expose an endpoint token, strict mode rejects it with
   `EOPNOTSUPP`, and synchronized mode omits token queries entirely.
   The AFD control handle suppresses native completion packets for synchronous
   success. Each serialized public wait publishes an epoch; a socket packet
   queued during an idle interval, an earlier wait epoch, or a cancellation-
   losing same-wait MOD is refreshed before delivery, and an immediately
   satisfied refresh is translated in place to avoid a large ready-set FIFO
   treadmill. Because an eager AFD request can
   also snapshot the first matching class during the current wait, exact TCP
   registrations take one zero-time `WSAPoll` snapshot before event filtering
   and LT/ET/ONESHOT latching. POLLRDNORM/POLLWRNORM merge missing requested
   normal levels, graceful POLLHUP merges requested readable EOF and RDHUP even
   behind unread data, and POLLERR merges unrequested ERR|HUP without consuming
   the socket's later `WSAECONNRESET`. A separate zero-time `select()` keeps the
   established non-inline priority qualification. Providers that reject
   `WSAPoll`, error/HUP snapshots already supplied by AFD, and unknown protocol
   metadata retain the conservative prior path.
4. Socket IOCP completions translate both the AFD per-handle `Events` bits and
   its `Status`; a negative per-handle status contributes `EPOLLERR` even when
   the event bitset is empty. Ready nodes snapshot the data, context, socket
   number, and generation; they never retain a raw socket pointer.
5. `epoll_wait*` serializes consumers because the ready queue is
   single-consumer, drains ready snapshots, waits for more IOCP packets, and
   skips stale generations. Lock acquisition is included in finite timeouts,
   and an early `WAIT_TIMEOUT` is retried against the absolute deadline. A
   zero-timeout call returns immediately if another waiter owns the drain.
   One drain pass holds `fd_table_lock` across the whole batch rather than
   reacquiring it per delivered node, so a large ready set does not hand the
   lock back to concurrent control operations between every event.
   A delivered LT registration is rearmed only after its ready node is
   consumed, which places it behind already queued peers and rotates a ready
   set larger than `maxevents` across successive waits. ET observation state
   survives the serialized waiter handoff, so one transition on one epfd wakes
   one waiter rather than being redelivered to the next waiter.
   A wait on an empty interest list remains blocked. Concurrent ADD observes
   the active waiter and immediately arms the new target; an already-ready
   socket therefore publishes its completion and wakes that wait.
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
   `epoll_wait` and `epoll_pwait` retain their millisecond contract. Public
   standard waits with a valid epoll descriptor validate the Linux UAPI
   `maxevents` ceiling before the output pointer; extended waits additionally
   bound the larger output array by `SIZE_MAX`, which is tighter on 32-bit
   targets. `epoll_pwait2*` validates its timespec first. The qualified x86-64
   Windows ABI uses Linux's packed 12-byte event-record ceiling (178,956,970)
   for both forms, while POSIX extension waits derive the standard ceiling
   from the host `epoll_event`.
   Windows writes either packed basic records or extended records directly to
   the caller's array, so a legal wait can return beyond 4096 without a
   proportional conversion allocation. For requests above 4096, the
   single-consumer wait may append already-queued IOCP batches after the first
   readiness snapshot without running another arm pass; this prevents ordinary
   LT registrations from being resubmitted and duplicated within the logical
   call. Every timeout mode bounds this append phase by both elapsed work and
   dequeue count (currently 10 ms after at least 64 dequeues, with an absolute
   128-dequeue ceiling), so an active completion stream can produce a partial
   result rather than monopolize the waiter. Once the first snapshot is
   selected under the fd-table lock, a concurrent MOD or `epoll_rearm()` of
   that already selected registration leaves its replacement generation on the
   deferred-rearm worklist for the next wait. Durations too large for the
   internal 100-nanosecond representation bypass the precise timer and retain
   the longer millisecond/chunked deadline.
   POSIX extension waits pass the caller array to one native wait as packed
   `epoll_event` storage, then copy returned records backward into their wider
   `epoll_event_ex` slots. This permits legal result counts beyond 4096 without
   proportional scratch allocation or repeated readiness syscalls. The
   fallback may still split very long timeout durations, but returns as soon as
   one native chunk reports readiness.
   Level-triggered registrations are armed again on a later wait; oneshot
   registrations require MOD or `epoll_rearm()`. Failures discovered during
   completion processing or deferred rearming are latched and wake the wait
   path. Ready snapshots already in the queue are returned first; the pending
   error is reported by the next wait. Only the first currently pending error
   is returned, while the statistics counter records every occurrence.
   An application wake is one coalesced per-port state transition plus one
   IOCP control packet. `wepoll_ex_wake()` consumes that state as a zero-event
   return. `wepoll_ex_wake_event()` snapshots a supplied `epoll_event` and
   consumes it as a one-record basic or extended result; extended output adds
   `WEPOLL_FLAG_WAKE_EVENT`, a timestamp, and a null context. A tagged request
   may upgrade a pending plain wake without another post, while a pending
   tagged payload is not replaced. The ready queue and latched asynchronous
   errors are checked first, so notification does not reorder socket failure
   or readiness. A mutex protects payload publication and consumption; the
   atomic state remains the waiter's fast test. An IOCP post failure clears the
   pending state and follows the same fatal port path as other unexpected
   control-post failures.
   The public Windows x86/x86-64 `epoll_event` itself uses Linux's UAPI layout:
   `data` is at offset 4 and size/stride is 12, with alignment 4 on x86 and 1
   on x86-64. Native arrays and FFI declarations therefore share the Linux
   representation. Other architectures retain native Linux-style alignment.
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
   stale data to the replacement. `wepoll_ex_close_socket()` first applies DEL
   to one port, accepting ENOENT only after target validation, and calls
   `closesocket()` afterward. Other DEL failures leave the socket open. It does
   not remove registrations from other ports or retain a socket duplicate.
   `wepoll_ex_shutdown_socket()` similarly validates one port before applying
   native shutdown. Unregistered and non-TCP sockets are native passthroughs.
   A registered TCP socket retains local read/write shutdown bits in its
   `ep_sock_t`; that state is shared by virtual aliases of the port but not by
   independent ports containing the same socket, and DEL discards it with the
   registration.
6. Closing a non-final virtual alias unlinks and frees only that integer table
   entry; it does not mark the shared port closing or wake its waits. Closing
   the final alias marks the shared record closing, wakes waiters, and removes
   the last logical epfd. Public API references aggregated across every alias
   and the first AFD-completion drain each have a five-second bound. If
   references remain, close returns
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

The Windows wait sink writes packed basic records or full extended records
directly, eliminating the former 64-event stack conversion and heap fallback.
Intrusive rearm and fired-oneshot worklists make the arm pass O(pending work),
including the native-close probes required for fired oneshots; it does not scan
every registration. List membership and visit counters are checked by internal
invariants and a synthetic 50,000-node worklist regression that does not
consume Winsock handles.

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
waits write native packed records into the prefix of the caller's
`epoll_event_ex` array and expand them backward in place after the syscall.
Descending expansion preserves unread packed records, avoids incompatible
struct aliasing, requires no proportional internal allocation, and lets one
native wait return any legal public `maxevents` count.
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
Pthread cancellation cleanup releases the metadata reference and restores any
temporary fallback signal mask before unwinding.

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
  writable-quota fields with `PeekNamedPipe`, a synchronous zero-byte write
  fallback for write-only handles, and the ADD-time granted-access
  classification. Readable data produces only the
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
  `STATUS_ACCESS_DENIED`, the adapter uses `PeekNamedPipe`; quota can then be
  unavailable and writable readiness remains advisory. A synchronous
  write-only handle that rejects `PeekNamedPipe` is sampled with a zero-byte
  `WriteFile`: success means connected/writable, `ERROR_PIPE_LISTENING` means
  no current peer, and broken/disconnected errors produce the requested
  writable aliases plus `EPOLLERR`. The probe consumes no quota and enqueues no
  byte or message. It is deliberately not issued on an overlapped or
  mode-unknown write-only handle because that HANDLE may be associated with an
  application IOCP; this narrow fallback remains advisory and may not
  distinguish peer closure.
  Other native-query errors produce no fabricated readiness and are retried by
  the timer path. Overlapped pipe HANDLEs use the same synchronous metadata
  snapshots, independently of application `OVERLAPPED` operations. Polling is
  a compatibility path, not a high-scale substitute for overlapped application
  I/O. Pipe and waitable registrations reject `EPOLLEXCLUSIVE`.
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
- The endpoint-token query is a kernel round trip, so hardened modes memoize
  one verdict per registration per serialized wait generation. Delivery and
  re-arm both consult that memo, which turns a socket producing several ready
  snapshots inside one `epoll_wait` into a single query instead of one per
  delivered event. Retirement removes a replaced registration from the fd
  table before any later drain observes it, so the memo cannot outlive the
  identity it describes. Transient `EP_IDENTITY_ERROR` results are never
  memoized because they carry an errno the caller must re-derive, and the
  memo is confined to the wait epoch: a new wait re-queries. Hardened
  submissions additionally cache the provider base handle against the token
  that validated it, so a re-arm skips the `SIO_BASE_HANDLE` traversal and its
  layered-provider fallbacks while any token change forces a fresh walk.
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
- AFD receive and accept map to the ordinary readable class; send maps only to
  the ordinary writable class; and expedited receive maps only to priority
  data. The public result is filtered to the requested aliases: `EPOLLIN` and
  `EPOLLRDNORM` share the normal-read class, `EPOLLOUT` and `EPOLLWRNORM`
  share the normal-write class, and `EPOLLPRI` represents expedited receive.
  `EPOLLRDBAND` and `EPOLLWRBAND` remain accepted for socket registrations but
  are inert: they do not arm an AFD readiness class or appear in a result.
  They do not suppress independently generated unrequested `EPOLLERR` or
  `EPOLLHUP`. A graceful disconnect reports readable EOF plus `EPOLLRDHUP`
  without `EPOLLERR` or `EPOLLHUP`. A TCP abortive close reports unrequested
  `EPOLLERR | EPOLLHUP`;
  an abort on a socket whose cached protocol metadata is an exact IPv4/IPv6
  UDP match reports `EPOLLERR` without HUP; and connect failure reports the
  requested readable/writable aliases plus unrequested `EPOLLERR | EPOLLHUP`.
  A negative AFD per-handle status reports unrequested `EPOLLERR`
  independently of the event bits. Missing or ambiguous provider protocol
  metadata retains the conservative abort mapping with HUP. Current
  established-TCP `WSAPoll` state merges requested `EPOLLRDHUP` for graceful
  HUP and unrequested `EPOLLERR | EPOLLHUP` for reset when either races behind
  an earlier AFD writable/readable snapshot. The poll is non-consuming, sees a
  FIN behind ordinary unread data, and leaves reset observable to `recv()`.
  A provider that rejects `WSAPoll` retains the conservative AFD snapshot;
  LT/ET rearm can observe the later terminal class, while ONESHOT needs the
  normal MOD rearm after a partial first-class delivery.
- Local Winsock receive shutdown is not an AFD readiness transition.
  `wepoll_ex_shutdown_socket()` is the explicit interception point. For a
  registered TCP socket, successful `SD_RECEIVE` records a READ bit and merges
  requested `EPOLLIN`/`EPOLLRDNORM` plus `EPOLLRDHUP`; once READ and WRITE are
  both recorded it also adds unrequested `EPOLLHUP`. `SD_SEND` alone records
  ownership but adds no synthetic readiness. Current ordinary TCP levels are
  sampled first so writable or reset state can share the snapshot.

  A direct idle publication reserves its ready node before the irreversible
  first Winsock shutdown. Pending AFD work is cancelled and its completion
  resubmits against the recorded state; losing cancellation is handled by the
  already-queued completion. A cancellation fault after native success leaves
  `needs_rearm` and the shutdown bits intact. A same-direction helper retry
  skips the already-completed native call and retries publication. Direct
  queueing replaces a queued generation when it adds bits, composes with the
  ET observed latch, ONESHOT, explicit disarms, and exclusive claims, and posts
  an internal IOCP wake only when a waiter is actually blocked. MOD and
  explicit rearm force a fresh local-state scan and preserve that wakeup rule.

  This mechanism does not alter Winsock I/O: `recv()` after `SD_RECEIVE`
  returns `SOCKET_ERROR`/`WSAESHUTDOWN`, not Linux's zero-byte EOF. Direct
  `shutdown()` calls and registrations in another independent port are still
  unobserved. DEL/re-ADD starts with no prior local-shutdown record. Peer
  FIN/reset readiness retains the exact behavior above.
- TCP urgent-data readiness is qualified for LT persistence until
  `recv(MSG_OOB)`, observed ET suppression and re-edge, ONESHOT MOD rearm, and
  MOD filtering/data replacement. Exact-event regressions verify that urgent
  readiness produces only requested `EPOLLPRI`, writable readiness produces
  only requested `EPOLLOUT`/`EPOLLWRNORM`, band-only masks produce no band
  readiness absent a terminal condition, and MOD switches among those masks
  without leaking stale aliases. With
  `SO_OOBINLINE`, the urgent byte is
  qualified as ordinary `EPOLLIN`, is consumed by normal `recv()`, persists in
  LT, and re-edges under the observed ET rule. AFD does not expose a separate
  expedited class in that mode, so Windows does not also report Linux's
  `EPOLLPRI` indication. `EPOLLMSG` is accepted but AFD has no event class that
  produces it. UDP IPv4/IPv6 readiness is covered publicly. For a confirmed
  UDP socket whose cached provider-file mode permits overlapped I/O, each
  successful AFD receive completion is qualified with a one-byte direct
  `IOCTL_AFD_RECV` normal-plus-peek request issued through `DeviceIoControl`.
  Eligibility requires an unlayered base-provider protocol chain because this
  native request bypasses Winsock provider transformations. Each request
  duplicates the provider base handle, revalidates endpoint identity in
  hardened lifetime modes, and pins that duplicate through settlement. Its
  private low-bit event prevents a qualifier packet from entering an
  application-owned IOCP. Success or AFD buffer overflow preserves the
  requested readable aliases without consuming the datagram; an asynchronous
  network receive error removes those aliases and contributes
  exact unrequested `EPOLLERR` without HUP. A pending probe is cancelled and
  joined before its stack state is released. A qualifier-safe registration
  without `EPOLLIN`/`EPOLLRDNORM` adds an internal `AFD_POLL_RECEIVE` bit so
  receive-queue errors remain implicit. An ordinary unread datagram is
  suppressed and parks that bit; completion immediately rearms the remaining
  terminal interests even for ET, avoiding a persistent IOCP receive loop.
  MOD and ONESHOT rearm clear the parked state and perform a fresh scan, with
  full submit-failure rollback. Like every public AFD poll, the internal
  receive submission is non-exclusive so it cannot cancel another
  registration's legitimate read poll; process-local terminal claims still
  arbitrate exclusive error delivery.
  IPv4/IPv6 connected-UDP ICMP
  probes require repeated LT `EPOLLERR`, then `recv() == WSAECONNRESET`, when
  `SIO_UDP_CONNRESET`, the provider, and host firewall expose the condition.
  A synchronous/non-overlapped socket, a layered provider, or a provider that
  returns a recognized unsupported-operation error for the reverse-engineered
  receive request retains the legacy AFD readable mapping. Receive-class errors
  behind an unread datagram remain indistinguishable without consuming or
  reordering it. Once the application drains that receive head, MOD refreshes a
  parked readless registration; an exclusive registration requires DEL/ADD
  because Linux-compatible exclusive MOD is rejected.
- Socket `EPOLLET` is implemented as an observed-edge filter over AFD level
  snapshots: each interest bit is delivered once while continuously true, then
  suppressed until it drops out of the latest level and reappears. Empty edge
  observations and losing exclusive claims use a short deferred retry instead
  of a tight immediate-completion loop. Pipe and waitable ET use the adapter-
  specific readiness observations described above.
- A Windows port created with `WEPOLL_EX_CREATE_EXPLICIT_REARM` gives socket
  `EPOLLET` a separate readiness-class ownership contract. Delivery disables
  READ or WRITE while leaving undelivered classes in the next AFD mask;
  `EPOLLERR`/`EPOLLHUP` disables READ, WRITE, and TERMINAL together.
  `epoll_rearm_classes()` clears selected disarms and the corresponding
  observed bits. If its expanded AFD mask is not covered by an in-flight
  request, cancellation either wins and completion rearms the new mask or
  loses and the queued completion refreshes uncovered interest before
  delivery. An incompletely drained class therefore redelivers its current
  level exactly once per acknowledgement, while a drained submission remains
  pending across the next transition. A successful MOD clears every disarm;
  DEL may retire pending or fully idle state. The operation returns `EBUSY`
  while a ready node has not yet been consumed, and `epoll_rearm()` is the
  all-class shorthand after consumption.

  This mode is socket-only and rejects EXCLUSIVE. It supports ONESHOT: partial
  acknowledgement updates the delivered-class disarm set while keeping the
  fired one-shot on its probe list and without submitting native work; clearing
  the final class removes the fired state, starts a new generation, and submits
  or queues the next AFD request. Submission failure restores the generation,
  observation state, class disarms, and mutually exclusive rearm/oneshot
  worklist membership. A terminal delivery can remove every AFD interest, so
  explicit users must issue DEL before `closesocket()` instead of relying on
  an idle native-close probe. POSIX reports the creation flag and class-rearm
  API as unsupported. The mode supplies a deterministic drain/ack primitive
  for an nginx experiment; nginx still needs per-handler rearm hooks and cannot
  use its unmodified Linux module.
- `EPOLLEXCLUSIVE` applies only to socket registrations and may be set only by
  ADD. It may be combined with `EPOLLET`, but not with `EPOLLONESHOT`,
  `EPOLLRDHUP`, or unsupported event bits. Every MOD of a registration added
  exclusive fails with `EINVAL`, even if the MOD mask omits `EPOLLEXCLUSIVE`.
  Every AFD request is submitted non-exclusive because native AFD exclusivity
  cancels ordinary peer polls, contrary to Linux's mixed-registration rule.
  Non-exclusive requests can still interfere when their single AFD handle
  entry contains the same numeric process HANDLE value. An intrusive active
  target-key index therefore gives each outstanding wepoll-ex request a
  distinct numeric key. An uncontended request uses the provider base handle
  directly. On collision, submission retains colliding duplicates while it
  allocates a distinct provider-handle value, inserts that value before the
  IOCTL, and closes the duplicate after the kernel captures it. For a pending
  request it normally replaces the freed numeric slot with a non-socket event
  until completion; the logical hash index remains the correctness authority
  if that optional reservation loses an allocation race or is unavailable.
  An anomalous duplicate `CloseHandle` failure is never retried by numeric
  value because HANDLE reuse could target an unrelated object; the logical
  index remains active and a possibly live duplicate is deliberately leaked.
  Completion, including cancellation, removes the key exactly once before
  rearm or reclamation. This avoids retaining the endpoint, so native
  `closesocket()` still produces local-close completion and peer FIN.

  Distinct target keys let every ordinary local epoll instance receive
  matching readiness. A separate process-wide claim filter tracks read,
  write, and terminal readiness classes independently, admits at least one
  local exclusive instance, and removes only conflicting classes from other
  exclusive mixed snapshots. A covering AFD submission that returns
  `STATUS_PENDING`, or a zero-time sample proving one direction inactive,
  releases the corresponding class claims. Each live registration embeds one
  intrusive claim node whose class bitset is indexed through fixed hash
  buckets. Capacity therefore grows with registrations and delivery does not
  allocate or fail open, while process-wide locks serialize target ownership
  and exclusive claims without holding them across kernel submission. Neither
  index crosses process boundaries or coordinates unrelated raw AFD users, so
  separate processes may each receive an exclusive wake for the same socket.
  The claim and active-target indexes coordinate only registrations within one
  process and one loaded wepoll-ex image. Separately linked static copies,
  distinct loaded DLL images, unrelated raw AFD users, and separate processes
  maintain independent state and do not coordinate.
- The nginx adapter leaves `ngx_event_actions.notify` unset. nginx 1.31.3
  rejects `--with-threads` on Win32 and its thread-pool sources are POSIX-only,
  so thread-pool integration is outside this prototype's supported boundary.
- Windows signal masks are opaque API placeholders. Non-null masks are
  accepted and ignored so portable `epoll_pwait*` call sites run; Windows has
  no POSIX process signal mask to apply. Windows waits also have no POSIX
  signal-interruption path and do not emulate signal-generated `EINTR`. Linux
  extended waits pass masks atomically to native `epoll_pwait2` or the chunked
  `epoll_pwait` fallback.
- Windows `epoll_pwait2*` represents positive finite durations in upward-
  rounded 100-nanosecond timer units when high-resolution timers are available,
  with an upward-rounded millisecond IOCP backstop and transparent coarse
  fallback. This improves requested deadline resolution but does not guarantee
  an equivalent scheduler wake latency.
- Windows control entry points recognize a null event pointer without
  dereferencing it, but cannot safely validate every arbitrary unreadable
  non-null userspace pointer. Callers must supply valid event storage; an
  invalid pointer may fault the process instead of returning `EFAULT` as a
  kernel syscall would.
- Windows virtual epoll descriptors cannot themselves be monitored or nested.
  Because an `epfd` is a process-local integer rather than a kernel HANDLE,
  self-registration and nested-epoll attempts cannot always reproduce Linux's
  distinct `EINVAL` result and may instead classify as an invalid or unrelated
  native target. `wepoll_ex_dup()` can create another process-local virtual
  integer sharing the same port and final-close lifetime. The integer still
  cannot be duplicated through native descriptor APIs, inherited, passed as an
  OS descriptor, or used with `fcntl` or `ioctl`. `EPOLL_CLOEXEC` is accepted
  for compatibility, but all virtual epfds are process-local regardless.
  Linux's `EPIOCSPARAMS`/`EPIOCGPARAMS` busy-poll ioctls therefore have no Windows
  counterpart; the POSIX build exposes a native epoll fd and inherits host
  ioctl support.
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

On August 5, 2026, Windows 10.0.19044 with MSYS2 MinGW GCC 16.1 and
`-O2 -Wall -Wextra -Wpedantic -Werror` completed 201 combined best-effort, 199
static-only, 115 shared-only, 201 strict-identity, 115 strict shared-only, 201
synchronized-lifetime, and 115 synchronized shared-only CTest entries. Their
passed/skipped counts were 200/1, 198/1, 114/1, 200/1, 114/1, 196/5, and 110/5.
All seven variants skipped the environment-dependent UDP/ICMP case;
synchronized modes additionally skipped the four native-reuse identity cases
owned by their DEL-before-close contract. Three repeats of 111 focused tests in
each internal-capable build and 68 focused tests in each shared-only build
passed. The shared builds also passed exact public-export checks.

The same worktree passed Linux/WSL GCC 14.2 strict Release CTest 5/5 in both
native and explicitly forced `epoll_pwait2` fallback lanes, five repeats each
of the API, large-wait, and pool executables, and ASan/UBSan CTest 4/4. The API
executable reported 56 passed, one focused legacy-WSL1 ready-list-rotation
skip, and zero failures. Coverage includes exact preview package compatibility,
ELF SONAME and Linux/MinGW export surfaces; socket alias, urgent-data,
status/error, ET/exclusive read, write, mixed-class, and stale-snapshot
transitions; persistent ready-set rotation, same-epfd ET single-wake behavior,
duplicate descriptors for one endpoint, and concurrent ready ADD wakeup from
an empty interest list; cancellation-losing same-wait MOD expansion refresh;
mixed normal/urgent LT convergence and persistence; live unread-data TCP FIN
and reset state, same-wait RDHUP/ERR/HUP merging, and preserved
`WSAECONNRESET`; local-shutdown LT/ET/ONESHOT/explicit-rearm publication,
blocked-wait MOD/rearm wakeups, and allocation/cancellation retry faults;
direction-aware pipe adapters; waitable terminal ET and
pending/queued MOD races; consumptive notification counts; auxiliary-disarm
fault recovery and preserved consumptive retries; immediate auxiliary
cancellation reclamation; IOCP post/close lease races and fatal-post wakeups;
high-resolution timeout conversion, stale generations, readiness races,
fallback, close, and init/arm/post failures; long native/fallback timespecs and
masked cancellation/restoration; provider identity modes;
cancellation/close/quarantine; packaging; and static-winpthread dependency
checks. `scripts/qualify-posix.sh`, `scripts/qualify-mingw.sh`, CMake presets,
and `scripts/repeat-ctest.sh` make those lanes reproducible.

The deterministic long stress profile completed 250,000 operations on 128
sockets with zero backpressure in combined best-effort and best-effort,
strict, and synchronized shared-library builds. On August 1, 2026, after eager
socket ADD was enabled, the production benchmark completed all 13 CSV rows for
the same four builds at a 50,000-socket maximum and 1,000 timed iterations.
The 50k registration row now includes provider submission plus pending-IRP or
synchronous-success handling and cancellation-initiation work; final close
drains the resulting completion burst outside the per-operation samples. The
run took about 99.765 seconds overall, and no performance threshold is
claimed.

The nginx 1.31.3 adapter passes a strict full Win32 link, dependency inspection,
`nginx -t`, 100 loopback requests across a worker reload, and graceful quit
using `use wepoll`. Six alternating four-second `h2load` pairs against commit
`ebc247d` measured medians of 79.9k requests/s for this tree and 78.5k for the
checkpoint; paired deltas ranged from -4.4% to +4.1%, so the local result is
performance-neutral rather than evidence of a throughput improvement. These
results still do not constitute a supported Windows/compiler matrix.
