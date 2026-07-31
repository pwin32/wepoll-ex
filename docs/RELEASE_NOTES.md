# Release Notes

## 0.1.0 (unreleased)

This is an experimental preview of the extended epoll-shaped API, the Windows
IOCP/AFD backend, the Linux development wrapper, and the optional nginx 1.31.3
adapter. The API and ABI may change before a stable release.

The earlier July 23, 2026 validation run used Windows 10.0.19044 with MSYS2
MinGW GCC 15.2, plus Linux/WSL builds with GCC 14.2 and Clang 19.1. Strict
MinGW CTest then passed 40 combined, 39 static-only, and 12 shared-only entries;
the synchronized-lifetime combined build also passed 40. Linux GCC and Clang
each passed 3 CTest entries, while ASan/UBSan passed 42 API and 5 pool/MPSC
checks. Installed-package consumers and dependency checks that reject a dynamic
`libwinpthread-1.dll` import passed. The nginx adapter passed a strict full
link, `nginx -t`, 100 loopback requests across a worker reload, and graceful
quit.

The July 24, 2026 production-readiness matrix on the same hosts expanded that
coverage. Linux GCC and Clang Release CTest each passed 3/3; direct API and
pool executables passed 45 and 5 cases. ASan/UBSan CTest passed 3/3 after the
package-consumer lane clears `LD_PRELOAD` for nested compiler configure/build
while still running consumers under the sanitizer. MinGW CTest passed 57
combined best-effort, 56 static-only, 13 shared-only, 57 strict-identity, and
57 synchronized-lifetime entries, with only the documented environment-dependent
ICMP/UDP error skip and synchronized-mode native-reuse identity skips. The
bounded close-reference regression tolerates coarse Windows timer resolution
while still requiring a non-zero, sub-second wait and `ETIMEDOUT`.

The later July 24, 2026 non-socket parity matrix used MinGW GCC 15.2 with
`-O2 -Wall -Wextra -Wpedantic -Werror`. CTest passed 83 combined
best-effort, 82 static-only, 39 shared-only, 83 strict-identity, and 83
synchronized-lifetime entries. The combined/static/strict/synchronized lanes
had the environment-dependent UDP/ICMP skip; synchronized mode also skipped
the expected native-reuse identity cases. Repeated API, backpressure, stress,
and concurrent-control tests passed in every applicable lane. Linux/WSL GCC
14.2 strict Release and ASan/UBSan CTest each passed 3/3, and Clang 19.1.7
strict Release passed 3/3.

The July 27, 2026 parity follow-up used the same strict MinGW flags and
completed 99 combined best-effort, 98 static-only, 52 shared-only, 99
strict-identity, and 99 synchronized-lifetime CTest entries. The
combined/static/strict/synchronized variants retained the expected UDP/ICMP
environment skip, and synchronized mode retained its four native-reuse identity
skips. Repeated API, backpressure, stress, and concurrent-control lanes passed.
Linux/WSL GCC 14.2 strict Release and ASan/UBSan CTest each passed 3/3; Clang
19.1.7 strict Release passed 3/3.

The later July 27, 2026 hardening follow-up used MinGW GCC 15.2 with
`-O2 -Wall -Wextra -Wpedantic -Werror` plus Release optimization. The combined,
static-only, shared-only, strict-identity, and synchronized-lifetime lanes
contained 107, 106, 54, 107, and 107 CTest entries respectively. Their
passed/skipped counts were 106/1, 105/1, 54/0, 106/1, and 102/5. Combined,
static-only, strict, and synchronized each skipped the environment-dependent
UDP/ICMP case, while synchronized mode also skipped four native-reuse identity
cases. Linux/WSL GCC 14.2 and Clang 19.1.7 strict Release each passed 3/3; the
explicitly forced `epoll_pwait2` fallback also passed 3/3 with GCC. The GCC
API/pool lanes passed five repeats, and ASan/UBSan passed 3/3 through the
qualification wrapper's loader-safe environment.

The subsequent July 27, 2026 qualification-matrix run added shared-only DLL
coverage for the strict and synchronized lifetime policies. Strict shared-only
passed 54/54 entries. Synchronized shared-only passed 50/54 and skipped the
four native-reuse identity cases required by its DEL-before-close contract.
The full seven-variant MinGW wrapper completed successfully, including three
repeats of every applicable API, backpressure, stress, and concurrent-control
test.

The final July 27, 2026 preview-contract qualification added exact package,
SONAME, and export-surface regressions. Combined best-effort, static-only,
shared-only, strict-identity, strict shared-only, synchronized-lifetime, and
synchronized shared-only contained 108, 106, 55, 108, 55, 108, and 55 CTest
entries. Their passed/skipped counts were 107/1, 105/1, 55/0, 107/1, 55/0,
103/5, and 51/4. Linux/WSL GCC 14.2 native and forced-fallback Release passed
4/4 each, the API/pool lanes passed five repeats, ASan/UBSan passed 3/3, and
Clang 19.1.7 strict Release passed 4/4.

The July 28, 2026 socket-event parity qualification used MinGW GCC 15.2 with
`-O2 -Wall -Wextra -Wpedantic -Werror`. Combined best-effort, static-only,
shared-only, strict-identity, strict shared-only, synchronized-lifetime, and
synchronized shared-only contained 116, 114, 65, 116, 65, 116, and 65 CTest
entries. Their passed/skipped counts were 115/1, 113/1, 64/1, 115/1, 64/1,
111/5, and 60/5. The only general skip was the environment-dependent UDP/ICMP
probe; synchronized variants also skipped the four native-reuse identity cases
owned by their DEL-before-close contract. Three repeats of the applicable API,
backpressure, stress, concurrent-control, AFD mapping/status, socket-alias,
urgent-data LT/ET/ONESHOT/MOD, and inline-urgent LT/ET tests passed. Linux/WSL
GCC 14.2 native and forced
fallback Release passed 4/4 each, the API/pool lanes passed five repeats,
ASan/UBSan passed 3/3, and Clang 19.1.7 strict Release passed 4/4.

The July 29, 2026 timeout-compatibility qualification used MinGW GCC 15.2 on
Windows 10.0.19044 with `-O2 -Wall -Wextra -Wpedantic -Werror`. Combined
best-effort, static-only, shared-only, strict-identity, strict shared-only,
synchronized-lifetime, and synchronized shared-only contained 124, 122, 65,
124, 65, 124, and 65 CTest entries. Their passed/skipped counts were 123/1,
121/1, 64/1, 123/1, 64/1, 119/5, and 60/5. The general skip remained the
environment-dependent UDP/ICMP probe; synchronized variants also skipped the
four native-reuse identity cases owned by their DEL-before-close contract.
Three repeats of every applicable API, backpressure, stress,
concurrent-control, socket-event, and precise-timeout lane passed. Linux/WSL
GCC 14.2 native and forced-fallback strict Release passed 4/4 each, repeated
API/pool tests passed five times, ASan/UBSan passed 3/3, and Clang 19.1.7
strict Release passed 4/4.

The later July 29, 2026 zero-interest waitable qualification used MinGW GCC
15.2 with the same strict flags. The seven best-effort combined, static-only,
shared-only, strict combined, strict shared-only, synchronized combined, and
synchronized shared-only lanes completed 149, 147, 84, 149, 84, 149, and 84
CTest entries respectively, plus three repeats of every applicable API,
stress, backpressure, control-race, socket-event, pipe, waitable dormancy,
ownership-replay, and fault-injection lane. Only the documented host-dependent
UDP/ICMP probe and synchronized-lifetime native-reuse cases skipped. Linux/WSL
GCC 14.2 native and forced-fallback strict Release passed 4/4 each, API/pool
tests passed five repeats, and ASan/UBSan passed 3/3.

On Windows 10.0.19044, the deterministic long stress profile completed all
250,000 operations on 128 sockets with 59,906 sends, 4,960 epoll rotations,
and zero backpressure in combined best-effort and best-effort, strict, and
synchronized shared-library builds. The four corresponding production
benchmarks completed all 13 CSV rows with a 50,000-socket maximum and 1,000
timed iterations; their internal measured sections took 8.487, 7.512, 8.973,
and 6.053 seconds respectively. Setup/teardown is excluded, ready batches stop
at 512, and no portable performance threshold is claimed.

The current worktree limits the development wrapper to Linux instead of
assuming every non-Windows platform provides `epoll` and `eventfd`. It also
behaves cleanly as a CMake subproject: it does not force the parent build type,
and tests default off unless the project is top-level. Ordinary configuration
still supports CMake 3.16; checked-in presets require CMake 3.21. Preview
packages now use exact-version CMake compatibility and the ELF SONAME
`libwepoll_ex.so.0.1.0`; package tests reject an older non-exact 0.x request,
and shared-library allowlists pin the 14 Linux and 20 Windows public exports.
The installed `wepoll_ex_version.h` header now supplies the canonical version
components used by CMake package metadata, the shared-library identity, both
runtime backends, nginx's embedded build, and installed-consumer checks.
A fresh strict build on Linux/WSL with GCC 14.2 passed CTest 4/4, and MinGW
GCC 15.2 on Windows 10.0.19044 passed 107/108 with only the documented
UDP/ICMP environment skip; both used `-Werror` and passed package/export
contract checks.

Core creation and control validation now more closely follows Linux.
`epoll_create()` requires a positive legacy size but ignores its value;
`epoll_create_ex()` uses a positive Windows capacity hint capped at 4096 and
ignores the hint on POSIX. For every operation other than numeric
`EPOLL_CTL_DEL`, the event is snapshotted once at entry and a null pointer
returns `EFAULT` before epfd, target, and operation validation, while DEL
ignores its event pointer. Windows control calls distinguish an invalid or
closed target (`EBADF`), a valid supported but
unregistered target (`ENOENT` for MOD, DEL, and `epoll_rearm()`), and a valid
unsupported object (`EPERM`). ADD separately preserves access and provider
eligibility errors such as `EACCES`. Arbitrary unreadable non-null pointers
remain a userspace limitation and may fault instead of returning `EFAULT`.
Virtual Windows epoll descriptors still cannot be monitored or nested, so the
Linux self/nested-epoll `EINVAL` distinction is not always representable.

Fresh qualification for this control-validation slice passed on Linux
4.4.0-19041-Microsoft with GCC 14.2: strict release CTest 4/4, the forced
`epoll_pwait2` fallback 4/4, five repeated API/pool runs, and ASan/UBSan 3/3
using the qualification script's `-Werror` flag sets. MinGW GCC 15.2 on
Windows 10.0.19044 passed all seven Release lanes with
`-O2 -Wall -Wextra -Wpedantic -Werror`: combined best-effort, strict, and
synchronized builds each completed 150/150; static-only best-effort completed
148/148; and best-effort, strict, and synchronized shared-only builds each
completed 84/84. The focused correctness subsets also passed three repeats in
every lane. Skips were limited to the documented environment-dependent
UDP/ICMP case and synchronized-mode native-reuse identity cases.

The July 30, 2026 socket-band parity qualification used the same Linux and
Windows hosts and strict compiler flags. Linux native and forced-fallback
Release CTest passed 4/4 each, API/pool tests passed five repeats, and
ASan/UBSan passed 3/3. The seven MinGW Release lanes completed 150 combined
best-effort, 148 static-only, 84 shared-only, 150 strict combined, 84 strict
shared-only, 150 synchronized combined, and 84 synchronized shared-only
entries. Their passed/skipped counts were 149/1, 147/1, 83/1, 149/1, 83/1,
145/5, and 79/5. Three repeats of every applicable qualification subset
passed, and the exact AFD mapping, socket-alias, urgent LT, and urgent MOD
paths additionally passed 20 consecutive combined runs. Skips remained
limited to the host-dependent UDP/ICMP probe and the four synchronized-mode
native-reuse identity cases.

The later July 30, 2026 UDP receive-error qualification used Linux/WSL GCC
14.2 and Windows 10.0.19044 with MSYS2 MinGW GCC 15.2 and the same strict
compiler flags. Linux native and forced-fallback Release CTest passed 4/4
each, API/pool tests passed five repeats, and ASan/UBSan passed 3/3. The seven
MinGW best-effort combined, static-only, shared-only, strict combined, strict
shared-only, synchronized combined, and synchronized shared-only lanes
contained 154, 152, 88, 154, 88, 154, and 88 CTest entries. Their
passed/skipped counts were 153/1, 151/1, 87/1, 153/1, 87/1, 149/5, and 83/5.
Three repeats of every applicable qualification subset passed. The only
general skip was the host-dependent IPv4 ICMP/reset probe; synchronized mode
also skipped the four native-reuse identity cases required by its
DEL-before-close contract. IPv6 reset LT/ET/ONESHOT and application-owned IOCP
isolation passed in every applicable lane.

Wait-count validation now follows the Linux UAPI contract instead of treating
`maxevents` as an allocation request. Windows core and extension waits reject
values above the qualified x86-64 Linux ceiling of 178,956,970, derived from
the packed 12-byte Linux event record; POSIX extension waits derive the same
formula from the host UAPI type. For a valid epoll descriptor, invalid counts
precede null output pointers, while `epoll_pwait2*` validates its timespec
first. One Windows or POSIX
extension wait returns at most 4096 events, avoiding multi-gigabyte basic-
event conversion buffers and POSIX extension allocations.
Regressions cover exact-limit empty waits, limit-plus-one rejection, errno
precedence, 4097-event empty and ready requests, and every Windows wait entry
point including `epoll_drain()`.

The July 31, 2026 qualification reran strict POSIX native/fallback and
ASan/UBSan coverage plus all seven MinGW lifetime/linkage lanes. Their full
CTest suites passed 153/154, 151/152, 87/88, 153/154, 87/88, 149/154, and
83/88 tests respectively; the remaining cases were the established UDP
capability skip and synchronized-mode native-reuse skips. Every applicable
focused Windows test also passed three consecutive repetitions.

The Windows x86/x86-64 public `struct epoll_event` now matches Linux's UAPI
representation: `data` is at offset 4 and structure size/array stride is 12
bytes, with alignment 4 on x86 and 1 on x86-64. The previous x86-64 natural
Windows layout used a 16-byte stride and was incompatible with Linux-shaped
FFI and serialized event buffers. This intentionally changes the experimental
0.1.0 preview ABI; all consumers must rebuild against the updated header. The
`maxevents` ceiling is now derived directly from the public structure size.

Qualifier-safe Windows UDP registrations now observe receive-queue errors as
implicit `EPOLLERR` even without `EPOLLIN` or `EPOLLRDNORM`. Readless masks add
a hidden `AFD_POLL_RECEIVE`, qualify successful completions with the existing
non-consuming direct AFD peek, suppress ordinary datagrams, and park that
hidden interest before immediately rearming terminal-only state. This retains
the payload without an IOCP hot loop. Successful MOD and ONESHOT rearm perform
a fresh probe with exact rollback on submission failure. The hidden request is
submitted without native AFD exclusivity so it cannot cancel a peer's real read
poll; process-local terminal claims still suppress duplicate exclusive error
delivery. Public regressions cover zero/explicit-ERR LT, flags-only ET and
ONESHOT, write-only ET reset delivery, plus preserved normal data delivered
after MOD to `EPOLLIN`; deterministic state regressions cover parking, masks,
ET duplicate suppression, successful and failed rearm, rollback, and exclusive
submission. Layered or synchronous providers retain the conservative
path, and an error behind a parked unread datagram remains unavailable until
the payload is drained and the registration is refreshed (DEL/ADD for an
exclusive registration).

The later July 31, 2026 qualification used Linux/WSL GCC 14.2 and Windows
10.0.19044 with MSYS2 MinGW GCC 15.2 under `-O2 -Wall -Wextra -Wpedantic
-Werror`. POSIX native and forced-`epoll_pwait2`-fallback Release suites passed
4/4 each, API/pool tests passed five repeats, and ASan/UBSan passed 3/3. The
seven MinGW best-effort combined, static-only, shared-only, strict combined,
strict shared-only, synchronized combined, and synchronized shared-only lanes
contained 163, 161, 94, 163, 94, 163, and 94 tests. Their passed/skipped counts
were 162/1, 160/1, 93/1, 162/1, 93/1, 158/5, and 89/5. Three repeats of every
applicable focused UDP, socket-event, waitable, pipe, API, stress, backpressure,
compatibility, timing, state, and fault subset passed. The only general skip
was the host-dependent IPv4 ICMP/reset probe; synchronized mode also skipped
the four native-reuse identity cases required by its DEL-before-close contract.

Windows socket lifetime is now an explicit CMake policy:
`WEPOLL_EX_SOCKET_LIFETIME_MODE=best-effort|strict|synchronized`.
Best-effort remains the default, strict rejects providers without stable WFP
endpoint identity using `EOPNOTSUPP`, and synchronized mode requires the
embedder to DEL before every `closesocket()`. The public policy getter and
versioned per-port/global statistics report the selected mode plus registration,
queue, pool, rearm, stale-event, identity, asynchronous-error, drain-budget,
quarantine, reaper, and close-timeout diagnostics. Linux reports its extension
registration count and a not-applicable lifetime policy.

Windows registrations now also accept anonymous/named pipes via short
timer-queue polls, `NtQueryInformationFile(FilePipeLocalInformation)` state and
quota snapshots, and the existing `PeekNamedPipe` fallback, plus waitable
timers through the existing waitable-HANDLE path. Ordinary disk files return
Linux-compatible `EPERM`. Pipe readiness honors the HANDLE's granted
read/write data access and matches Linux masks for directional pipe states:
buffered EOF reports only the requested `EPOLLIN`/`EPOLLRDNORM` aliases plus
`EPOLLHUP`, drained or initially empty EOF reports `EPOLLHUP` alone, and a
write endpoint whose reader closed reports only requested
`EPOLLOUT`/`EPOLLWRNORM` aliases plus `EPOLLERR`, without `EPOLLHUP`. Pipes never
synthesize `EPOLLRDBAND` or `EPOLLWRBAND`. ET emits ordinary rising aliases
without repeating an unrelated active direction, while terminal transitions
preserve `IN` -> `IN|HUP` -> `HUP` and `OUT` -> `OUT|ERR`. Invalid native
metadata samples retain the existing edge latch. Terminal ONESHOT registrations
remain MOD/`epoll_rearm()`-rearmable, writable quota drives LT/ET backpressure
and restoration, and a reused named-server HANDLE re-edges for its next client
without MOD. Regressions cover exact normal and terminal aliases,
mixed-direction ET, LT terminal redelivery, both terminal ONESHOT rearm paths,
both native named-pipe endpoint orientations with overlapped handles, rejected
native snapshots, the outbound-server advisory fallback, server reconnect,
pending MOD, exclusive rejection, and cancellation cleanup.

Windows registrations now accept waitable HANDLEs in addition to
Winsock sockets. Events, semaphores, waitable timers, processes, and threads
use `RegisterWaitForSingleObject` and wake through the port IOCP. Object types
are classified without a destructive wait; jobs and mutexes return `EPERM`,
HANDLEs lacking `SYNCHRONIZE` return `EACCES`, and `EPOLLEXCLUSIVE` is rejected
for non-socket registrations.
Auto-reset events and semaphores consume exactly one signal/count per delivered
notification, including ET delivery. Zero-interest waitables are now truly
dormant: they do not probe, register a wait, enter the rearm queue, consume a
notification, or spin on a terminal object. MOD-to-zero synchronously disarms
an active callback and rolls back on disarm failure. A callback-consumed,
MOD-hidden, early-`epoll_rearm()`-invalidated, or allocation-failure
notification remains owned until it can be replayed with current metadata
before a new probe. An already-posted canceled packet may remain pending only
until IOCP dequeue; Windows cannot un-consume a callback that wins immediately
before MOD-to-zero linearizes. Flags-only ET is accepted for a dormant timer or
mode-unknown event. Nonzero-interest ET remains rejected because the reset mode
cannot be recovered from an arbitrary HANDLE. Auxiliary callback retirement
failures now keep storage and pending accounting pinned, surface an asynchronous
error, and retry through wait/DEL/close rather than permitting a stale cookie or
premature free.

Auxiliary cancellation no longer manufactures an IOCP cancellation packet
when blocking disarm proves that no callback or queued packet can reference the
registration. Unsignaled waitables can therefore be DELed and reclaimed
immediately without a later wait or close drain; already-posted packets remain
pinned until dequeue. If a disarm failure follows a consumptive wait, the
auto-reset event, semaphore count, or mode-unknown notification is preserved
and replayed after cleanup succeeds.

Auxiliary and control posts now hold a per-port IOCP HANDLE lease. Close
revokes the posting alias before closing the completion port, eliminating the
callback-versus-close stale-HANDLE reuse window. A fatal post failure closes
the IOCP to wake an infinite waiter and reports the original error. Regressions
cover a blocked post versus close, callback and immediate-post failures,
already-posted cancellation pinning, closed-IOCP reclamation, and a 64-object
no-wait cancellation batch.

Process and thread HANDLEs now classify as monotonic terminal waitables: after
their ET edge is delivered they remain idle instead of generating throttled
empty completions forever. MOD keeps pending waitable and pipe operations alive
so an already-posted callback uses the latest mask/data. MOD also replaces a
queued known-consumptive or mode-unknown waitable notification with a
current-generation snapshot, preserving any signal/count or timer expiration
already consumed by the underlying wait. Queued pipe state is safe to discard
and re-sample because pipe observation is non-consumptive.

Windows `epoll_pwait*` now accepts opaque non-null signal-mask pointers
and ignores them. Exclusive ADD accepts `EPOLLET` but rejects `EPOLLONESHOT`,
`EPOLLRDHUP`, and unsupported event bits. `EPOLLWAKEUP` is accepted and ignored.

Windows socket completion translation now consumes the AFD per-handle status
as well as its event bits. A negative status contributes unrequested
`EPOLLERR`; TCP abortive close guarantees unrequested `EPOLLERR | EPOLLHUP`;
an abort on a confirmed IPv4/IPv6 UDP socket omits HUP; and a failed connect
guarantees `EPOLLERR | EPOLLHUP` alongside the requested readable/writable
aliases. Graceful disconnect remains readable EOF plus `EPOLLRDHUP`, without
being promoted to an error. AFD send readiness now produces only the requested
`EPOLLOUT`/`EPOLLWRNORM` aliases, and expedited receive produces only requested
`EPOLLPRI`. `EPOLLRDBAND` and `EPOLLWRBAND` remain accepted for sockets but are
inert; independently generated `EPOLLERR` and `EPOLLHUP` remain unrequested
terminal events. Public regressions cover exact ordinary, priority, and inert
band-event masks plus MOD transitions, urgent LT/ET/ONESHOT behavior,
`SO_OOBINLINE` LT/ET delivery as ordinary `EPOLLIN`, UDP IPv4/IPv6 readiness,
and connected-UDP ICMP errors without HUP. Confirmed UDP normal-read
completions on overlapped provider handles are now qualified with a private
one-byte direct AFD normal-plus-peek request. Only an unlayered base-provider
chain is eligible; a duplicated provider handle is file-mode checked,
endpoint-identity checked in hardened lifetime modes, and pinned
through request settlement. `DeviceIoControl` low-bit event suppression keeps
qualifier completions out of application-owned IOCPs; datagrams remain queued,
while a queued `WSAECONNRESET` produces exact `EPOLLERR` without readable aliases
and repeats under LT until consumed. Missing or ambiguous provider protocol
metadata retains the conservative abort mapping. Synchronous/non-overlapped UDP sockets,
layered providers, providers that return a recognized unsupported-operation
error for the reverse-engineered receive request, registrations without
normal-read interest, and errors queued behind unread datagrams retain the
documented receive-qualification caveats.
`SO_OOBINLINE` does not retain Linux's separate `EPOLLPRI` indication, and
`EPOLLMSG` is never produced.

Windows `EPOLLET` and ADD-time `EPOLLEXCLUSIVE` are no longer rejected.
Socket edge-triggered delivery latches observed interest bits from AFD level
snapshots and suppresses redelivery until those bits clear and reassert; empty
edge observations use throttled deferred re-sampling. Exclusive registrations
submit AFD polls with `Exclusive=TRUE`; a process-wide claim filter tracks
read, write, and terminal readiness independently, filters only conflicting
classes from mixed snapshots, and releases a class after pending submission or
an inactive directional sample proves it quiescent. Every MOD of an exclusive
registration returns `EINVAL`, even when the MOD mask omits
`EPOLLEXCLUSIVE`.

The exclusive claim filter now stores each owner's readiness-class bitset in
an intrusive registration node indexed through process-wide hash buckets. This
removes the fixed 128-entry table and its exhaustion fail-open path without
allocating during completion delivery. A regression holds 129 distinct read
claims concurrently, verifies that a peer port receives no duplicate wake,
then exercises individual DEL and bulk port-close claim release.

Rearm and fired-oneshot tracking now use intrusive worklists. Wait preparation
is proportional to pending work rather than all registrations; a regression
places 50,000 synthetic inert socket-state nodes beside one rearm to guard that
property without consuming Winsock handles.
Asynchronous failures from completion handling and deferred rearming are
latched for waits. Existing ready snapshots are delivered first, and the
deferred error is returned by the next wait.

Windows close no longer waits indefinitely for a public API reference. The
reference wait and initial completion drain are each bounded at five seconds.
A reference timeout removes the epfd, returns `ETIMEDOUT`, and transfers final
destruction to the last referencing operation. A recoverable completion-drain
timeout transfers ownership to a detached reaper for up to 60 seconds;
irrecoverable state remains quarantined rather than being freed unsafely.

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

Linux extended waits now hold a stable duplicate of the epoll descriptor.
`wepoll_close()` wakes all blocked extended waiters, which fail with `EBADF`,
and waits for their metadata references before teardown. Context decoration is
suppressed (`user_ctx == NULL`) when duplicate opaque data values make a match
ambiguous or an extension control change overlaps the wait. Metadata now keeps
separate registrations when Linux retains distinct open file descriptions
under one reused numeric fd. MOD/DEL use an `fstat` fingerprint and return
`EOPNOTSUPP` instead of mutating an arbitrary registration when multiple
entries are indistinguishable. On Linux, `epoll_fd_count()` explicitly counts
extension-owned registrations; native `epoll_ctl()` additions are outside
that view until an extension MOD adopts them. `epoll_pwait2_ex()` uses native
`epoll_pwait2` when available and falls back after build-time absence or
runtime `ENOSYS`, preserving atomic signal-mask application but rounding the
fallback timeout up to milliseconds. Valid timespecs beyond the `int`
millisecond range are now split into bounded waits rather than rejected with
`EOVERFLOW`; masked multi-chunk waits block catchable signals between calls and
restore the caller's mask on return or cancellation. The new
`WEPOLL_EX_FORCE_EPOLL_PWAIT2_FALLBACK` option, matching preset, and
`qualify-posix.sh` lane make that fallback reproducible even on current libc
and kernels. Pthread cancellation cleanup releases the wait buffer and
metadata reference before unwinding.

Windows finite waits now retry an early `WAIT_TIMEOUT` against their absolute
deadline. Positive finite `epoll_pwait2*` waits now request an optional
high-resolution waitable timer with an upward-rounded 100-nanosecond duration,
generation-tagged IOCP timeout packets, and an independently rounded-up
millisecond safety deadline. Missing capability and timer initialization, arm,
or post failures transparently retain the coarse wait path; requested timer
units are not a scheduler wake-latency guarantee. Long finite timespecs are
accepted and IOCP waits are chunked as needed, while integer-millisecond API
behavior remains unchanged. When internal packets keep arriving, a
zero-timeout wait processes
at least 16 successful, nonempty IOCP dequeue batches before enforcing a 10 ms
budget; any readiness found during the drain is still returned. Winsock
provider resolution uses `SIO_BASE_HANDLE` first and then guarded SELECT, POLL,
and generic BSP fallbacks, rejecting malformed responses and cycles while
continuing past one cyclic candidate if a later fallback advances the chain.
Deterministic timeout regressions cover conversion, stale/current generations,
early timeout packets, readiness racing a timeout, concurrent close, and the
forced coarse fallback. Fault injection covers timer initialization, arm, and
callback-post failures; precise-only cases report a capability skip on hosts
without the optional timer path.
New regressions exercise UDP IPv4/IPv6 readiness, optional ICMP error delivery,
DEL/ADD reuse, provider fallback, concurrent controls, a 513-packet internal
burst, and an injected early timeout.

Small waits avoid allocation: Windows basic waits use a 64-event stack buffer,
and Linux extended waits use 32 events before falling back to the heap. The
Linux context lookup uses a reverse data index, while Windows consumes only its
rearm and fired-oneshot worklists. Under the synchronized socket lifetime
contract, Windows also reuses the base provider handle captured at ADD instead
of resolving it for every rearm; best-effort and strict builds continue to
re-resolve it. `bench_wait_scaling` measures empty and one-ready Linux extended
waits as registration count grows. `bench_latency` now validates all I/O,
fails incomplete runs, and reports percentiles. The new Windows CSV benchmark
covers registration scaling through 50,000 sockets, ready batches, oneshot
rearm, and armed control churn.

Deterministic fail-at-N hooks now cover pool allocation/growth, AFD open,
submit and cancel, endpoint identity/policy, provider resolution, IOCP create,
post and dequeue, and ready-node allocation. A seeded public-API Windows stress
test randomizes ADD/MOD/DEL, oneshot rearm, socket reuse, waits, and epfd
rotation with bounded defaults and replay output. Qualification presets and
scripts cover strict Linux native/fallback release and sanitizers plus MinGW
combined, static-only, and shared-only best-effort builds and combined/shared
strict-identity and synchronized variants.

The endpoint-identity fault mode now drives the WFP endpoint `WSAIoctl`
response through an internal callback seam. It verifies all supported
unavailable-provider errors, mapped hard failures, exact eight-byte results,
malformed byte counts, caller-output preservation, fault bypass, and tokens
whose high 32 bits are set; the ordinary Winsock call remains a smoke test.

The nginx addon now defaults to best-effort lifetime validation. Configure it
with `WEPOLL_EX_NGINX_LIFETIME_MODE=strict` or `synchronized` only when that
contract is intentional. `scripts/nginx-endurance.py` provides bounded,
seeded normal, keep-alive, slow-partial, reset, and half-close traffic, with
optional reload invocation and recovery checks.

A quick nginx A/B used six alternating four-second HTTP/1.1 `h2load` pairs,
32 connections, two client threads, one nginx worker, and the `empty_gif`
handler. This tree measured a 79.9k requests/s median versus 78.5k for commit
`ebc247d`; individual paired deltas ranged from -4.4% to +4.1%. That spread is
local noise, so this run does not demonstrate a throughput improvement.

This validation is not a support matrix. Release-qualified Windows evidence is
limited to x86-64 MinGW-w64 GCC 15.2 on Windows 10.0.19044. MSVC/clang-cl,
x86/ARM64, real alternative Winsock providers, and Windows 8 itself remain
unqualified. AFD is undocumented, and `_WIN32_WINNT=0x0602` remains the
Windows 8-or-later compile/runtime assumption.
Windows now accepts `EPOLLET` and ADD-time `EPOLLEXCLUSIVE`. Socket edge
delivery is an observed-bit filter over AFD level reports rather than a kernel
edge queue, and exclusive wake uniqueness relies on AFD exclusive-poll
cancellation plus a readiness-class-granular process-wide claim index among
wepoll-ex instances. Non-null
Windows signal-mask pointers are accepted and ignored, `EPOLLWAKEUP` is a
no-op, and high-resolution `epoll_pwait2*` deadlines remain subject to Windows
scheduler latency and a transparent millisecond fallback. `EPOLLEXCLUSIVE`
may combine with `EPOLLET`, but not with
`EPOLLONESHOT`, `EPOLLRDHUP`, or unsupported event bits; virtual Windows epoll
descriptors cannot be nested, socket `EPOLLRDBAND` and `EPOLLWRBAND` interests
are accepted but inert, `EPOLLMSG` is not emitted, `SO_OOBINLINE` collapses
priority into ordinary readability, UDP receive-side errors have the
synchronous-socket, uninterested-read, and receive-head caveats described
above, unknown provider protocol metadata retains a conservative abort
mapping, and pipe writable readiness can remain advisory
when native local information is unavailable or access is denied. Pure
write-only outbound named-pipe server handles are the known access-denied case.
Performance measurements are local loopback observations, not portable
throughput guarantees.

See `README.md`, `docs/DESIGN.md`, and `docs/NGINX_INTEGRATION.md` for current
contracts and integration constraints.
