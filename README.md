# wepoll-ex

`wepoll-ex` is an experimental C11/CMake prototype for an epoll-shaped API on
Windows. Its Windows path uses IOCP and the undocumented AFD poll interface;
its Linux path wraps native `epoll` for development and API experiments.
Other POSIX systems are rejected at configure time. The project is not
production-ready, and no complete Linux compatibility, Windows performance,
or nginx integration guarantee is made.

## Scope and current status

The repository is useful for exploring the API boundary and for reproducing
bugs in the two platform paths. It is not a drop-in replacement for Linux
`epoll`, and no throughput, latency, connection-count, or compatibility target
is promised. Treat all extension semantics as provisional until they have a
platform-specific regression test.

The nginx, libuv, Mio, and Asio source archives are reference material only.
They are deliberately not part of the tracked source and must never be staged.
See [`docs/UPSTREAM_EVENT_LOOP_AUDIT.md`](docs/UPSTREAM_EVENT_LOOP_AUDIT.md)
for the exact-source comparison,
[`docs/NGINX_INTEGRATION.md`](docs/NGINX_INTEGRATION.md) for the current
adapter validation checklist, and
[`docs/NGINX_NATIVE_EPOLL_PORT.md`](docs/NGINX_NATIVE_EPOLL_PORT.md) for the
native Linux module's portability boundary, and
[`docs/WINDOWS_PORTING.md`](docs/WINDOWS_PORTING.md) for the opt-in source and
lifetime bridges intended for direct Windows ports.

## Architecture at a glance

### Windows implementation

`src/wepoll_ex_api.c` exposes the public integer `epfd` API and maps each id to
an internal `ep_port_t`. `src/wepoll_ex_port.c` owns an IOCP handle, AFD poll
state, a growable socket table, intrusive rearm/fired-oneshot worklists, and a
ready queue. Socket ADD submits its first `AFD_POLL` before returning, so an
initial submission failure is reported synchronously and readiness can be
retained before the first wait. Waitable and pipe ADD remain lazy when no
waiter is active, avoiding notification consumption and timer polling. A
pending socket MOD whose mask is already covered keeps the request; an
expansion cancels once and rearms with the latest metadata. If cancellation
loses to an already queued AFD completion, that packet remains pending until
the completion path refreshes any newly uncovered classes, so a same-wait MOD
cannot publish a partial snapshot. Wait work is proportional to queued rearm
and oneshot-probe work rather than all registrations. An idle socket whose poll
remains pending owns an AFD IRP;
simultaneous registrations of one socket across epoll ports may also hold
temporary duplicate/reservation HANDLEs. An immediately satisfied idle poll
is consumed synchronously, discarded, and left on the rearm queue so the first
wait samples the then-current level without retaining an idle IOCP packet.

An eager AFD request can complete on the first ready class before another
class becomes ready. The AFD control handle suppresses native IOCP packets for
synchronous success. Completions queued during an idle interval or an earlier
serialized wait epoch, and completions whose submitted AFD mask no longer
covers a same-wait MOD, are refreshed before delivery; an immediately satisfied
refresh is translated in place rather than moved behind the completion
backlog. When the cached provider metadata is an exact TCP match, completion
handling also non-destructively samples current normal and terminal state with
zero-time `WSAPoll`, then separately qualifies non-inline priority. A graceful
`POLLHUP` merges requested readable EOF and `EPOLLRDHUP`, including a FIN queued
behind unread data; `POLLERR | POLLHUP` merges unrequested
`EPOLLERR | EPOLLHUP` for reset without clearing the application's later
`WSAECONNRESET`. Unknown protocols and providers that reject `WSAPoll` retain
the original conservative AFD/select result.

Direct Winsock local receive shutdown remains a platform boundary:
`shutdown(SD_RECEIVE)` and `shutdown(SD_BOTH)` do not raise an AFD, `WSAPoll`,
or `select` read event. `wepoll_ex_shutdown_socket()` is the opt-in
interposition point. For a TCP socket registered in the selected port it
records successful local read/write shutdown, publishes requested
`EPOLLIN`/`EPOLLRDNORM` plus `EPOLLRDHUP` after receive shutdown, and adds
unrequested `EPOLLHUP` after both directions are shut down. LT persistence,
observed ET suppression, ONESHOT, explicit class rearm, current ordinary
readiness, MOD metadata replacement, and blocked-wait wakeup compose with the
synthetic level. A pending AFD request is cancelled/refreshed; an idle or
already-ready registration is queued directly. Direct `shutdown()` calls and
registrations in another independent port remain unobserved. DEL discards the
registration-local record, so re-ADD starts without the earlier synthetic
shutdown state. Winsock data-call semantics also remain native: `recv()` after
`SD_RECEIVE` fails with `WSAESHUTDOWN` rather than returning Linux EOF. Peer
FIN/reset behavior is unaffected.

Internal failures after a successful control call are latched for the wait
path. Already-queued readiness is delivered before the deferred error; a later
wait returns `-1` with that error. `wepoll_close()` removes the virtual
descriptor immediately, bounds public-operation reference waits and AFD
completion draining, and hands recoverable late completions to a detached
reaper. An unrecoverable port remains quarantined rather than risking
use-after-free. AFD is undocumented, and the build currently targets Windows
8 or later (`_WIN32_WINNT=0x0602`). Release-qualified Windows evidence is
limited to x86-64 MinGW-w64 GCC 15.2 and 16.1 on Windows 10.0.19044. Windows 8
itself, MSVC/clang-cl, x86/ARM64, and real alternative Winsock providers
remain unqualified.

At most four detached reapers run concurrently, each with a 60-second drain
window. A production workload should finish with `active_quarantines == 0` and
`irrecoverable_ports == 0`; a nonzero value requires investigation and may
represent intentional process-lifetime retention.

Auxiliary callbacks and control/error wakeups hold a short per-port IOCP post
lease across `PostQueuedCompletionStatus`. Close revokes that posting alias
before closing the HANDLE, so a callback cannot post through a stale numeric
HANDLE after reuse. An unexpected post failure makes the port unusable, closes
the IOCP to wake a blocked waiter, and reports the retained error.

### Linux development path

`src/wepoll_ex_posix.c` leaves the basic `epoll_create*`, `epoll_ctl`, and
`epoll_wait` symbols to the host libc. It keeps per-epfd metadata plus a stable
duplicate used by extended waits. Metadata tracks each successful ADD, even
when Linux keeps two open-file-description registrations under one reused fd
number; MOD/DEL use an `fstat` identity and reject ambiguous fingerprints
instead of changing an arbitrary entry. `wepoll_close()` wakes blocked
extended waiters, which return `EBADF`, before releasing that metadata.
Context lookup uses a reverse index; duplicate opaque data values, or metadata
changes that overlap a wait, deliberately produce `user_ctx == NULL` instead
of a stale association. `epoll_pwait2_ex()` uses native `epoll_pwait2` when
the libc and kernel provide it, retaining nanosecond timeout precision. An
`ENOSYS` or build-time absence falls back to `epoll_pwait`/`epoll_wait` with a
timeout rounded up to milliseconds. Valid finite timeouts longer than
`INT_MAX` milliseconds are split into bounded waits instead of failing with
`EOVERFLOW`; a masked multi-chunk fallback blocks catchable signals between
syscalls and restores the caller's original mask on return or cancellation.
As with any userspace multi-syscall fallback, a process-directed signal may be
routed to another eligible thread during the inter-chunk bridge; thread-
directed delivery to the waiting thread remains protected.
Cancellation cleanup releases metadata references and restores any temporary
signal-mask bridge state, so cancelling a blocked thread cannot strand a later
`wepoll_close()` or alter the caller's mask. The shared pool/queue code is
compiled here for unit tests; it does not make the Linux wrapper a
Windows-engine implementation.

### Public extensions

The header [`include/wepoll_ex.h`](include/wepoll_ex.h) declares
`epoll_create_ex`, `epoll_ctl_ctx`, `epoll_wait_ex`, `epoll_pwait2_ex`,
`epoll_ctl_batch`, `epoll_drain`, `epoll_rearm`, `epoll_rearm_classes`,
`epoll_fd_count`, version helpers, capability/socket-lifetime/statistics
queries, `wepoll_ex_get_last_error_info`, `wepoll_ex_wake`,
`wepoll_ex_wake_event`, `wepoll_ex_dup`, `wepoll_ex_close_socket`,
`wepoll_ex_shutdown_socket`, and `wepoll_close`.
`epoll_ctl_batch` applies operations in order and best-effort rolls back ADDs;
it is not transactional.

With a valid epoll descriptor, standard wait entry points reject
`maxevents <= 0` and values above the Linux UAPI ceiling before checking the
output pointer. Extended waits also reject a count whose larger
`epoll_event_ex` array would exceed `SIZE_MAX`; this is the tighter bound on
32-bit targets. On the qualified x86-64 Windows target both ceilings are
178,956,970, derived from Linux's packed 12-byte `epoll_event` transfer record;
Linux extended waits derive the standard ceiling from the host UAPI structure.
A legal large value is only a return-count upper bound.
Windows writes basic or extended records directly into the caller's array and
can fill beyond 4096 without allocating conversion storage proportional to
`maxevents`; large waits also coalesce already-queued IOCP completion batches
without rearming delivered LT registrations inside the call. Coalescing is
bounded for every timeout mode, so even a positive or infinite wait may return
fewer events than the supplied upper bound rather than monopolize the waiter
while completions keep arriving. MOD and `epoll_rearm()` of an already selected
registration defer their replacement generation to the next wait, which
prevents that registration from appearing twice in the same logical result.
When more level-ready registrations exist than `maxevents`, successive Windows
waits rotate through the persistent ready set. Multiple waiters on one Windows
epfd serialize ready-queue consumption; for an `EPOLLET` registration, one
readiness transition wakes one waiter rather than being copied to every waiter.
A blocking wait may begin with an empty Windows interest list. A concurrent
ADD arms the new registration immediately when a waiter is active, so adding
an already-ready socket wakes that wait with the new registration's event.
POSIX extension waits issue one native wait directly into the packed prefix of
the caller's wider array, then expand the returned records backward in place.
They can therefore fill beyond 4096 without proportional conversion storage or
repeated native waits that would need to deduplicate opaque `epoll_data`.
Long-duration fallback chunking applies only to the timeout; the first native
chunk that reports readiness returns that batch. `epoll_pwait2*` validates a
non-null timespec before the count and output pointer, matching Linux's syscall
order.

On Windows x86 and x86-64, `struct epoll_event` uses Linux's UAPI layout:
`data` begins at byte 4 and the structure size and array stride are 12 bytes.
Alignment is four bytes on x86 and one byte on x86-64. This preview ABI change
makes native and FFI event buffers layout-compatible with Linux on those
architectures.

The standard `epoll_create(size)` requires a positive argument but ignores its
value, matching modern Linux. `epoll_create_ex(size, flags)` retains the size
only as an extension hint: Windows caps the positive hint value at 4096, while
POSIX accepts and ignores it. The hint is not a registration limit.
Windows accepts `EPOLL_CLOEXEC`, but its epfds are process-local virtual
integers rather than inheritable OS descriptors, so close-on-exec does not
change their lifetime. `wepoll_ex_dup()` creates another virtual integer for
the same Windows port: registrations, readiness, waits, and statistics are
shared, closing a non-final alias removes only that alias, and final close
wakes waiters and destroys the port. This is an explicit library alias, not a
native `dup()` result. A Windows epfd still cannot be inherited, passed as a
HANDLE or CRT descriptor, duplicated by `DuplicateHandle`/`dup`, or operated
on with `fcntl` or `ioctl`. In particular, Linux's `EPIOCSPARAMS` and
`EPIOCGPARAMS` epoll busy-poll ioctls are not available on Windows. The POSIX
build returns a native epoll fd and naturally inherits native `dup()` and
those ioctls when the host headers and kernel support them;
`wepoll_ex_dup()` reports `EOPNOTSUPP` there.

Windows `epoll_create_ex()` also accepts
`WEPOLL_EX_CREATE_EXPLICIT_REARM`.  Socket `EPOLLET` delivery on such a port
disarms the returned read or write readiness class until
`epoll_rearm_classes()` acknowledges that the application drained it to
`WSAEWOULDBLOCK`; a terminal `EPOLLERR`/`EPOLLHUP` delivery disarms every
class.  Other directions remain natively armed, avoiding repeated observation
of nginx-style continuously writable registrations.  Rearming an
incompletely drained class immediately reports its still-true level once and
disarms it again.  MOD clears all disarms, `epoll_rearm()` acknowledges all
classes, and DEL works while the registration is idle or pending.

This explicit contract is socket-only and rejects `EPOLLEXCLUSIVE`; it does
not change ordinary observed-edge ports, pipe/waitable ET, or POSIX native
epoll. `EPOLLONESHOT` may be combined with it. A partial class acknowledgement
keeps the one-shot fired and submits no native request; acknowledging the last
delivered class starts the next one-shot generation. A fully disarmed
registration may have no AFD request in flight, so the embedder must DEL before
`closesocket()`. `wepoll_ex_close_socket()` provides that ordering for one
specified epfd and treats an already absent registration as success after
validating the target; it does not remove the socket from any other epoll
instances. `wepoll_ex_shutdown_socket()` similarly publishes local TCP
shutdown readiness only in the specified underlying port (including its
virtual aliases). Stock nginx's Linux module still needs handler-completion
rearm hooks, especially for posted events; this is not a symbol-only port. The
checked-in nginx adapter supplies those hooks behind `wepoll_edge on`, while
retaining level-triggered operation as its default. `wepoll_close_audit on`
(also the default) reports live registration/queue state before worker port
close and lifecycle quarantine state afterward. Audit every included addon
with `scripts/audit-nginx-close-paths.py`; compare identical running level and
edge endpoints with `scripts/nginx-h2load-compare.py`. The exact source audit,
runtime invariant, and measured tiny-response overhead are recorded in
`docs/NGINX_CLOSE_PATH_AUDIT.md` and `docs/NGINX_INTEGRATION.md`.

Windows control calls classify the target before membership errors where Linux
does: an invalid or closed target returns `EBADF`, a valid supported target
that is not registered returns `ENOENT` for MOD, DEL, `epoll_rearm()`, and
`epoll_rearm_classes()`, while a valid but unsupported object returns `EPERM`.
ADD alone also reports
registration eligibility failures such as missing HANDLE access (`EACCES`) or
a Winsock provider-resolution error. Every operation other than numeric
`EPOLL_CTL_DEL` snapshots one event value at entry; a null pointer returns
`EFAULT` before descriptor and operation validation, while DEL ignores the
pointer.
Normal ADD and MOD registrations accept `EPOLLMSG` and undefined event bits,
then filter them from delivered readiness, matching Linux. ADD-time
`EPOLLEXCLUSIVE` masks retain Linux's stricter allowed-bit validation.
As a userspace Windows API, wepoll-ex cannot safely probe every arbitrary
unreadable non-null pointer: callers must provide valid event storage or the
process may fault instead of receiving `EFAULT`.

On Windows, registrations accept Winsock sockets, anonymous/named pipes, and
selected waitable HANDLEs (events, semaphores, waitable timers, processes,
and threads). Waitable HANDLEs must grant `SYNCHRONIZE`; otherwise ADD returns
`EACCES`. Mutexes, jobs, ordinary disk files, and other unsupported object
types are rejected with Linux-compatible `EPERM`. Pipe readiness uses short
timer polls and combines `NtQueryInformationFile(FilePipeLocalInformation)`
state/quota snapshots with `PeekNamedPipe`, a synchronous zero-byte write
fallback for write-only handles, and HANDLE access classification. Buffered
read-side EOF reports the requested
`EPOLLIN`/`EPOLLRDNORM` aliases plus
unrequested `EPOLLHUP`; empty or drained EOF reports `EPOLLHUP` alone. A write
endpoint whose reader has closed reports the requested `EPOLLOUT`/`EPOLLWRNORM`
aliases plus unrequested `EPOLLERR`, without `EPOLLHUP`. Pipes never synthesize
`EPOLLRDBAND` or `EPOLLWRBAND`. Pipe ET reports ordinary rising readiness
aliases without repeating unrelated active directions. A newly raised terminal
condition includes the current normal aliases, preserving the
`IN` -> `IN|HUP` -> `HUP` and `OUT` -> `OUT|ERR` sequences. Unavailable native
metadata samples do not clear the edge latch. A peer-closed ONESHOT registration
remains installed for MOD or `epoll_rearm()` rearm. A natively identified
terminal client pipe end stops polling after its final ET snapshot. A
named-pipe server HANDLE remains
sampled because `DisconnectNamedPipe()` and `ConnectNamedPipe()` can reuse the
same HANDLE; readiness on the next client produces a fresh edge without MOD.
Writable backpressure and restoration normally follow the reported quota.
When native local information is unavailable or access is denied, the
fallback can retain advisory writable readiness because quota is unknown.
For a synchronous write-only handle (notably an outbound named-pipe server), a
zero-byte write distinguishes listening, connected, and peer-closed states
without consuming quota or adding pipe data. The adapter does not issue that
probe on an overlapped or mode-unknown write-only handle because it may belong
to an application IOCP; that narrow fallback remains advisory and may not
distinguish peer closure. Other overlapped pipe HANDLEs use the same metadata sampling;
the adapter does not manage application `OVERLAPPED` requests. Issue
`EPOLL_CTL_DEL` before `CloseHandle()` for every registered non-socket object.
`EPOLLONESHOT` is supported. Manual-reset events use ordinary observed-level ET
filtering;
auto-reset events and semaphores deliver one ET notification per consumed
signal/count. Terminated process/thread handles deliver their terminal ET edge
once and then stay idle instead of entering the reset-detection retry loop.
A waitable with no requested readiness aliases is dormant: ADD and wait do not
probe it, register a callback, queue rearm work, or consume a signal/count.
MOD from active interest to zero synchronously disarms the callback before the
new mask becomes visible. An already-posted packet may remain physically
pending until dequeue, but it owns no active wait. If its callback already
consumed an auto-reset event, semaphore count, or mode-unknown notification,
that observation remains owned across dormancy and is replayed before probing
when interest is restored. The same transfer preserves a consumptive ready
node invalidated by MOD-to-zero or an early `epoll_rearm()`, and a notification
whose ready-node allocation failed. A callback that wins immediately before
MOD-to-zero linearizes cannot be un-consumed from the underlying Windows
object. Flags-only `EPOLLET` is
therefore accepted for an otherwise dormant timer or mode-unknown waitable;
adding nonzero interest while ET remains set is rejected because an arbitrary
HANDLE does not expose its reset mode. Other MOD operations preserve an
in-flight waitable/pipe operation and apply the latest mask/data when it
completes. Blocking auxiliary cancellation retires an unsignaled registration
immediately when no packet was posted; an already-posted packet keeps storage
pinned until dequeue.

Windows socket event translation consumes both the AFD event bits and the
per-handle completion status. A negative per-handle status reports
`EPOLLERR`; TCP abortive close reports unrequested `EPOLLERR | EPOLLHUP`; a
confirmed IPv4/IPv6 UDP abort reports `EPOLLERR` without the terminal HUP bit;
and a failed connect reports the requested readable/writable aliases plus
unrequested `EPOLLERR | EPOLLHUP`. Graceful disconnect remains readable EOF
and `EPOLLRDHUP`, without being promoted to an error. TCP urgent data maps the
AFD expedited-read class to `EPOLLPRI` only. LT readiness persists until
`recv(MSG_OOB)`, ET re-edges after the urgent level clears and reappears,
ONESHOT requires MOD rearm, and MOD applies the latest mask/data. AFD send
readiness maps only to the ordinary `EPOLLOUT` and `EPOLLWRNORM` aliases,
filtered to the bits requested by the registration. `EPOLLRDBAND` and
`EPOLLWRBAND` remain accepted for sockets but are inert: they do not arm or
synthesize readiness, although independently generated `EPOLLERR` and
`EPOLLHUP` remain unrequested terminal events. Public exact-event regressions
cover each socket alias and MOD transitions between ordinary, priority, and
band-only masks. With `SO_OOBINLINE`, urgent
bytes are qualified as ordinary `EPOLLIN` readiness, remain level-ready until
normal `recv()`, and follow the same observed ET suppression/re-edge rule. AFD
does not retain a separate priority indication in this mode, so Windows does
not additionally produce Linux's `EPOLLPRI` notification. `EPOLLMSG` is
accepted as an event bit but is never produced on Windows.

UDP error coverage has two layers. Deterministic internal completions verify
that an AFD status such as port-unreachable reaches public delivery as
`EPOLLERR`. For a confirmed UDP socket whose provider handle safely permits
overlapped I/O, every successful AFD receive completion is qualified with a
private one-byte direct `IOCTL_AFD_RECV` normal-plus-peek request. Readless
registrations temporarily add an internal `AFD_POLL_RECEIVE` interest so a
receive-queue error remains implicitly observable even when the caller did not
request `EPOLLIN` or `EPOLLRDNORM`. `DeviceIoControl` supplies a low-bit private
event, so the qualifier cannot place a packet on an application-owned IOCP; a
pending empty-queue request is cancelled and joined before its stack state is
released. A queued asynchronous receive error such as `WSAECONNRESET`
therefore reports exact unrequested `EPOLLERR`, without `EPOLLIN`,
`EPOLLRDNORM`, or `EPOLLHUP`, and remains level-ready until the application
consumes it. Normal and oversized datagrams remain queued and retain requested
readable aliases.

An ordinary datagram observed by a readless registration is suppressed and
parks the internal receive interest; the port immediately rearms only terminal
AFD conditions so the unread datagram cannot drive an IOCP completion loop.
Every successful MOD clears that parking state and performs a fresh scan, so
MOD to normal-read interest exposes the preserved datagram and same-mask MOD
refreshes implicit error observation. Flags-only ET and ONESHOT registrations
use the same hidden probe and retain their duplicate-suppression/rearm rules.
All public AFD polls use non-exclusive submission, including this hidden
receive interest, so an exclusive registration cannot cancel an ordinary
peer's legitimate poll. The process-local terminal claim still arbitrates
exclusive error delivery. Public IPv4/IPv6 probes enable
`SIO_UDP_CONNRESET`, arm before sending, require two LT error observations,
then verify `recv() == WSAECONNRESET`; a timeout is a genuine skip only when a
nonblocking peek also reports `WSAEWOULDBLOCK`.

Protocol metadata, an unlayered base-provider chain check, and an initial
asynchronous-I/O capability are cached at ADD. Direct AFD receive bypasses
Winsock provider transformations, so stacked/layered UDP providers deliberately
retain the legacy mapping. Each eligible probe duplicates the cached provider
base handle and keeps that duplicate pinned through cancellation or completion
settlement;
the duplicate's file mode and the public endpoint identity are revalidated
before submission in hardened lifetime modes.
Exact IPv4/IPv6 UDP matches suppress the terminal HUP bit, while unavailable or
ambiguous provider metadata retains the conservative `EPOLLERR | EPOLLHUP`
abort mapping. A synchronous/non-overlapped provider socket, a layered
provider, or a provider that returns a recognized unsupported-operation error
for the reverse-engineered receive request retains the legacy AFD readable
mapping. A reset queued behind an unread datagram cannot be distinguished
without consuming or reordering that payload. After the application drains the
receive head, a successful MOD refreshes a parked readless registration;
Linux-compatible exclusive registrations reject MOD and therefore require
DEL/ADD for that refresh.

`EPOLLET` uses observed-readiness filtering with throttled re-sampling of an
already-seen level. `EPOLLEXCLUSIVE` applies only to socket registrations and
may be combined with `EPOLLET`, but not with `EPOLLONESHOT`, `EPOLLRDHUP`, or
unsupported event bits. Every MOD of a registration added exclusive returns
`EINVAL`, even when the MOD mask omits `EPOLLEXCLUSIVE`. An allocation-free
process-wide claim index uses intrusive registration nodes and hash buckets to
track read, write, and terminal readiness independently, so a continuously
writable exclusive owner does not suppress a disjoint read wake. Kernel polls
remain non-exclusive. Because AFD can nevertheless couple concurrent requests
that name the same numeric target handle, an intrusive process-wide key index
assigns every outstanding wepoll-ex poll a distinct target value. The first
request can use the provider base directly; a collision uses a transient
duplicate only through submission, then normally reserves that numeric slot
with a non-socket event until completion. The logical index remains
authoritative if slot reservation is unavailable. This lets every ordinary
local epoll instance receive matching readiness without retaining the socket
or delaying native `closesocket()`/peer FIN, while the claim index admits at
least one local exclusive instance and filters the rest. Windows
`epoll_pwait*` accepts a non-null signal-mask pointer and ignores it (there is
no POSIX process signal mask). Windows waits do not emulate POSIX
signal-driven interruption and therefore do not produce Linux-style `EINTR`
from a signal or apply a supplied mask. Linux `epoll_pwait2_ex` applies a
supplied mask atomically through the native wait or its chunked fallback.
Positive finite
Windows `epoll_pwait2*` waits request an optional high-resolution waitable
timer using an upward-rounded 100-nanosecond duration while retaining an
upward-rounded millisecond IOCP deadline as a safety backstop. Missing timer
support or any initialization/arm failure transparently uses the millisecond
path; integer `epoll_wait`/`epoll_pwait` behavior is unchanged. Timer units are
not a scheduler wake-latency guarantee. `EPOLLWAKEUP` is accepted and ignored
on Windows. Call `wepoll_close()` for the virtual Windows epoll descriptor and
for prompt Linux extended-wait wakeup.

Remaining platform limits are explicit: Windows signal masks and
`EPOLLWAKEUP` have no native effect, high-resolution timeout support is
optional and Windows scheduling can wake later than the requested deadline,
edge delivery is observed-level rather than Linux kernel queue semantics,
socket `EPOLLRDBAND` and `EPOLLWRBAND` interests are accepted but inert,
`EPOLLMSG` is never produced, `SO_OOBINLINE` collapses priority into ordinary
readability, UDP resets on synchronous/non-overlapped or layered sockets and
errors queued behind a parked unread datagram retain the receive-qualification
caveats above, exclusive arbitration is process-local so separate processes
may each receive an exclusive wake for the same socket, the claim and active-
target indexes coordinate only registrations within one process and one loaded
wepoll-ex image, separately linked static copies and distinct loaded DLL images
do not coordinate, the active-target index cannot arbitrate unrelated raw AFD
consumers, a provider that rejects `WSAPoll` retains the AFD snapshot when a
same-wait FIN/reset loses to an earlier nonterminal class (a later rearm can
observe it, while ONESHOT requires the normal MOD rearm), unknown provider
protocol metadata retains a conservative abort mapping, pipe writable
readiness can remain advisory when native local information is unavailable,
an overlapped or mode-unknown write-only pipe cannot be safely
zero-write-probed, local receive/full shutdown has no asynchronous Winsock
read/RDHUP transition when the application bypasses
`wepoll_ex_shutdown_socket()`,
exclusive-claim updates serialize through one process-wide mutex, and virtual
epoll descriptors cannot be monitored or nested. Consequently, Windows cannot
reproduce Linux's distinct self-registration and nested-epoll `EINVAL` cases;
the virtual integer may instead classify as an invalid or unrelated native
target. Virtual epfds have explicit same-process aliases through
`wepoll_ex_dup()`, but no native descriptor duplication, inheritance,
`fcntl`, `ioctl`, or epoll busy-poll configuration semantics;
`EPOLL_CLOEXEC` is accepted only for source compatibility.

On Linux, `epoll_fd_count()` reports registrations owned by the extension
metadata, including successful `epoll_ctl_batch` operations. Native
`epoll_ctl()` additions are not counted until a successful
`epoll_ctl_ctx(..., EPOLL_CTL_MOD, ...)` adopts them; later native MOD/DEL
operations do not update the extension metadata view.

### Socket lifetime and diagnostics

Select the Windows policy at configure time with
`-DWEPOLL_EX_SOCKET_LIFETIME_MODE=best-effort|strict|synchronized`:

- `best-effort` is the default and accepts providers without a stable WFP ALE
  endpoint token, using legacy numeric-handle behavior for those sockets.
- `strict` rejects such an ADD with `EOPNOTSUPP`; transient identity-query
  failures also suppress queued delivery and are reported through a wait.
- `synchronized` skips endpoint-token probes and requires the embedder to
  complete `EPOLL_CTL_DEL` before every `closesocket()`.

`wepoll_ex_get_socket_lifetime_policy()` reports the compiled policy.
`wepoll_ex_close_socket()` can enforce DEL-then-close for one epfd without
retaining a duplicate socket or changing peer-FIN behavior. A socket registered
in multiple ports must still be removed from every other port first.
`wepoll_ex_shutdown_socket()` validates the epfd before effect and passes
through to native `shutdown()` for unregistered/non-TCP sockets. For a
registered TCP socket it publishes local shutdown state only to that port.
The state lasts for that registration; DEL/re-ADD does not reconstruct a
shutdown that happened before the new ADD.
When an immediate ready-node allocation is required for the first transition,
the node is reserved before the irreversible Winsock call. A later
cancellation/publication failure can return `-1` after native shutdown has
already succeeded; the recorded state lets a retry on a still-usable port
resume publication without issuing the same native shutdown again. A fatal
IOCP post failure still fails the port.
`wepoll_ex_get_stats()` and `wepoll_ex_get_global_stats()` copy versioned,
size-prefixed operational snapshots. Windows exposes registration, queue,
pool, stale-event, identity, asynchronous-error, drain-budget, quarantine,
reaper, close-timeout, wait-wake, and current-TCP-probe counters. Linux reports
its extension-owned registration count and marks the socket policy not
applicable; unsupported Windows-only counters are zero. These are diagnostics,
not an atomic transactional view.

`wepoll_ex_get_capabilities()` distinguishes native Linux edge queues from the
Windows observed-edge filter, reports the optional explicit edge-rearm
contract and process-local exclusive arbitration, and identifies which wait
families support `wepoll_ex_wake()`. Windows basic and extended waits consume
one coalesced plain wake as an early zero-event return after ready events and
pending errors. `wepoll_ex_wake_event()` instead snapshots an application
event and returns it as a one-record synthetic batch; extended waits add
`WEPOLL_FLAG_WAKE_EVENT`. A tagged request upgrades a pending plain wake, while
the first pending tagged payload is retained when later requests coalesce.
`WEPOLL_EX_CAP_TAGGED_WAKE_EVENT` advertises the tagged form. The POSIX wrapper
reports both wake operations unsupported because its basic waits are direct
libc calls. Additional capability bits advertise the close helper, explicit
rearm plus ONESHOT, virtual epfd aliases, the shutdown helper, and the
error-info channel.

`wepoll_ex_get_last_error_info()` copies the calling thread's details after a
failed library operation. `portable_error` is the public `errno`; Windows also
reports an exact Win32, Winsock, or NTSTATUS source when the failing path
retained it, plus a canonical Winsock equivalent when meaningful. A normalized
error without `WEPOLL_EX_ERROR_NATIVE_EXACT` must not be interpreted as the
original native failure. POSIX reports the current `errno` and leaves native
fields empty. The successful getter does not overwrite the saved channel.

The prioritized follow-up work and nginx qualification gates are recorded in
[`docs/COMPATIBILITY_ROADMAP.md`](docs/COMPATIBILITY_ROADMAP.md).

## Repository layout

```
include/   public headers
compat/    opt-in Windows <sys/epoll.h> compatibility include root
src/       Windows engine and Linux wrapper
tests/     Linux, Windows API, pool, and package-consumer tests
bench/     Linux latency/scaling and Windows qualification benchmarks
nginx/     opt-in nginx 1.31.3 adapter and configure hook
docs/      design and integration status
scripts/   repeatable Linux/MinGW qualification and nginx endurance tools
```

## Build and test

The project requires Windows or Linux. CMake 3.16 is sufficient for ordinary
configuration; checked-in presets require CMake 3.21. A direct strict Linux
run is:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DWEPOLL_EX_BUILD_BENCH=ON
cmake --build build --target wepoll_ex_shared wepoll_ex_static \
    test_wepoll_ex test_wepoll_ex_pool bench_latency bench_wait_scaling
ctest --test-dir build --output-on-failure
./build/tests/test_wepoll_ex
./build/tests/test_wepoll_ex_pool
./build/bench/bench_latency 50000
ulimit -n 65536  # raise the soft limit for large registration counts
./build/bench/bench_wait_scaling 1024 20000
```

Equivalent presets and the release/sanitizer qualification script are:

```sh
cmake --preset posix-release
cmake --build --preset posix-release
ctest --preset posix-release
cmake --preset posix-pwait2-fallback
cmake --build --preset posix-pwait2-fallback
ctest --preset posix-pwait2-fallback
./scripts/qualify-posix.sh
```

`WEPOLL_EX_FORCE_EPOLL_PWAIT2_FALLBACK=ON` suppresses the native Linux
`epoll_pwait2` path even when libc exports it. The dedicated preset and
`qualify-posix.sh` use that switch to exercise the `epoll_pwait`/`epoll_wait`
fallback with strict release flags.

For MinGW, use the toolchain shell explicitly:

```sh
/path/to/msys64/usr/bin/bash.exe -lc \
  'export PATH=/mingw64/bin:/usr/bin:$PATH; cd /e/personal/wepoll-ex; \
   cmake -S . -B build-mingw -G "MinGW Makefiles" \
     -DCMAKE_BUILD_TYPE=Debug -DWEPOLL_EX_BUILD_BENCH=ON \
     -DWEPOLL_EX_SOCKET_LIFETIME_MODE=best-effort \
     -DCMAKE_C_FLAGS="-Wall -Wextra -Wpedantic -Werror"; \
   cmake --build build-mingw --parallel; \
   ctest --test-dir build-mingw --output-on-failure'
```

`scripts/qualify-mingw.sh` automates combined, static-only, and shared-only
best-effort variants plus combined and shared-only strict-identity and
synchronized-lifetime variants. The seeded Windows stress test has bounded
defaults and accepts `--long` or `WEPOLL_EX_STRESS_*` overrides.
`bench_windows` emits CSV percentiles for 1k/10k/50k registration points,
ready batches, oneshot rearming, and armed control churn:

```sh
./build-mingw/tests/test_wepoll_ex_windows_stress.exe --long
./build-mingw/bench/bench_windows.exe --production
```

The production profile creates 50,001 UDP sockets but binds only the first 512.
Its 50k point now measures armed-idle AFD ADD and cancellation-initiation
scaling, not 50,000 ready sockets. Final port close drains the resulting
completion burst outside the per-operation samples. The benchmark
intentionally has no pass/fail latency thresholds.

`bench_mt_contention` covers what the single-threaded benchmark cannot: it
runs a wait thread that keeps an active socket set continuously ready, so that
thread is repeatedly inside a real drain holding the port's fd-table lock, and
reports the latency distribution of a concurrent `epoll_ctl` thread working on
a disjoint socket set. Use it to make changes to the drain's lock scope
falsifiable rather than assumed.

```sh
./build-mingw/bench/bench_mt_contention.exe 256 32 2
```

Its control-path percentiles have a wide run-to-run spread on a shared
machine — an unmodified binary compared against itself has been observed
varying by more than 40% at `ctl_mod` p50. Establish that A/A floor first,
balance A/B orderings, and treat any delta smaller than the floor as noise.

Linux qualification covers API contracts, close/wait/cancellation races,
native `epoll_pwait2` where libc and the kernel provide it, plus a separately
forced fallback build, signal-mask waits, metadata changes, reused-fd identity,
the pool, and package consumption. The MinGW suite covers TCP/UDP IPv4 and
IPv6 readiness; exact ordinary, priority, and inert band-event masks; LT, ET,
ONESHOT, and MOD transitions; per-handle AFD status errors; reset and
refused-connect terminal flags; provider-handle fallback; multi-epfd waits;
synchronous socket ADD failure; IOCP batch draining; timeout deadlines;
fail-at-N injection; bounded
close/quarantine cleanup; randomized lifecycle stress; and package consumers.
MinGW final binaries select the static winpthreads archive, and CTest rejects
an accidental `libwinpthread-1.dll` dependency. Record the exact command,
compiler, Windows version, and build flags for new results.

The installed-package test builds and runs the default target plus every
exported explicit shared/static target using the active compiler and linker
flags. It rejects incompatible preview-version requests and verifies that only
the requested components are exported. A companion shared-library test pins
the public symbol list and ELF SONAME contract.

## Install and consume

Version 0.1.0 is an experimental preview and does not promise a stable ABI.
Each preview release therefore uses exact CMake package compatibility and an
exact-version ELF SONAME.

The installed `wepoll_ex_version.h` header is the canonical version source.
`WEPOLL_EX_VERSION_MAJOR`, `WEPOLL_EX_VERSION_MINOR`,
`WEPOLL_EX_VERSION_PATCH`, `WEPOLL_EX_VERSION_NUMBER`, and
`WEPOLL_EX_VERSION_STRING` match the CMake package, shared-library identity,
and `wepoll_ex_version*()` runtime results.

Install it to an isolated prefix while evaluating it:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
    -DWEPOLL_EX_BUILD_TESTS=OFF
cmake --build build-release --parallel
cmake --install build-release --prefix /path/to/wepoll-ex-prefix
```

CMake consumers can use the installed package and its default target (shared
when both library forms are installed):

```cmake
find_package(wepoll_ex 0.1.0 EXACT CONFIG REQUIRED)
target_link_libraries(my_server PRIVATE wepoll_ex::wepoll_ex)
```

Use `wepoll_ex::wepoll_ex_static` or `wepoll_ex::wepoll_ex_shared` when the
linkage must be explicit. Set `CMAKE_PREFIX_PATH` to the install prefix, or set
`wepoll_ex_DIR` to its `<libdir>/cmake/wepoll_ex` directory.

Windows builds also install an opt-in source-compatibility target:

```cmake
find_package(wepoll_ex 0.1.0 EXACT CONFIG REQUIRED COMPONENTS epoll_compat)
target_link_libraries(my_linux_epoll_port PRIVATE wepoll_ex::epoll_compat)
```

That target alone adds the isolated installed include root containing
`<sys/epoll.h>` and links the selected wepoll-ex library. The compatibility
header is deliberately not placed in the ordinary include root, so consumers
that use `wepoll_ex::wepoll_ex` do not globally shadow another platform's
`sys/epoll.h`. Set `WEPOLL_EX_BUILD_EPOLL_COMPAT=OFF` to omit the target. It
provides declarations and constants, not Linux descriptor semantics such as
native `dup`, inheritance, nested epoll, eventfd, timerfd, or file AIO.

When included with `add_subdirectory`, wepoll-ex no longer changes the parent
project's build type and its tests default off. Enable both `BUILD_TESTING` and
`WEPOLL_EX_BUILD_TESTS` when subproject tests are desired. The selected Windows
socket-lifetime mode is a property of the built library; consumers can query
it at runtime rather than assuming the package's configuration.

Release-specific validation and limitations are recorded in
[`docs/RELEASE_NOTES.md`](docs/RELEASE_NOTES.md).

## Credits and license

New wepoll-ex contributions are distributed under the ISC terms in `LICENSE`.
The Windows AFD/IOCP implementation contains work derived from wepoll by Bert
Belder; the upstream BSD-2-Clause terms are preserved in `NOTICE`. Installed
packages include both files under `share/licenses/wepoll-ex`.
