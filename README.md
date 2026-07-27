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

The archive `nginx-1.31.3.tar.gz` is reference material only. It is deliberately
not part of the tracked source and is not evidence that the adapter builds.
See [`docs/NGINX_INTEGRATION.md`](docs/NGINX_INTEGRATION.md) for the current
validation checklist.

## Architecture at a glance

### Windows implementation

`src/wepoll_ex_api.c` exposes the public integer `epfd` API and maps each id to
an internal `ep_port_t`. `src/wepoll_ex_port.c` owns an IOCP handle, AFD poll
state, a growable socket table, intrusive rearm/fired-oneshot worklists, and a
ready queue. Stable registrations defer their `AFD_POLL` until a waiter arms
the port; an already-active waiter and an unconnected transitional stream arm
immediately. A pending MOD whose mask is already covered keeps the request;
an expansion cancels once and rearms with the latest metadata. Wait work is
proportional to queued rearm and oneshot-probe work rather than all
registrations.

Internal failures after a successful control call are latched for the wait
path. Already-queued readiness is delivered before the deferred error; a later
wait returns `-1` with that error. `wepoll_close()` removes the virtual
descriptor immediately, bounds public-operation reference waits and AFD
completion draining, and hands recoverable late completions to a detached
reaper. An unrecoverable port remains quarantined rather than risking
use-after-free. AFD is undocumented, and the build currently targets Windows
8 or later (`_WIN32_WINNT=0x0602`). Release-qualified Windows evidence is
limited to x86-64 MinGW-w64 GCC 15.2 on Windows 10.0.19044. Windows 8 itself,
MSVC/clang-cl, x86/ARM64, and real alternative Winsock providers remain
unqualified.

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
timeout rounded up to milliseconds. Cancellation cleanup releases wait
buffers and metadata references, so cancelling a blocked thread cannot strand
a later `wepoll_close()`. The shared pool/queue code is also compiled here for
unit tests; it does not make the Linux wrapper a Windows-engine implementation.

### Public extensions

The header [`include/wepoll_ex.h`](include/wepoll_ex.h) declares
`epoll_create_ex`, `epoll_ctl_ctx`, `epoll_wait_ex`, `epoll_pwait2_ex`,
`epoll_ctl_batch`, `epoll_drain`, `epoll_rearm`, `epoll_fd_count`, version
helpers, socket-lifetime policy and statistics queries, and `wepoll_close`.
`epoll_ctl_batch` applies operations in order and best-effort rolls back ADDs;
it is not transactional.

On Windows, registrations accept Winsock sockets, anonymous/named pipes, and
selected waitable HANDLEs (events, semaphores, waitable timers, processes,
and threads). Waitable HANDLEs must grant `SYNCHRONIZE`; otherwise ADD returns
`EACCES`. Mutexes, jobs, ordinary disk files, and other unsupported object
types are rejected with Linux-compatible `EPERM`. Pipe readiness uses short
`PeekNamedPipe` timer polls and respects the HANDLE's granted read/write access;
writable readiness on a write-capable handle is advisory, so high-throughput
pipe users should still use overlapped I/O. Issue `EPOLL_CTL_DEL` before
`CloseHandle()` for every registered non-socket object. `EPOLLONESHOT` is
supported. Manual-reset events use ordinary observed-level ET filtering;
auto-reset events and semaphores deliver one ET notification per consumed
signal/count. Terminated process/thread handles deliver their terminal ET edge
once and then stay idle instead of entering the reset-detection retry loop.
MOD preserves an in-flight waitable/pipe operation and applies the latest
mask/data when it completes. A queued notification from a known-consumptive or
mode-unknown waitable is replaced with an equivalent current-generation
snapshot so MOD cannot discard a signal or timer expiration that the
underlying wait may already have consumed. ET is rejected for waitable timers
because an arbitrary timer HANDLE does not expose its reset mode. Blocking
auxiliary cancellation retires an unsignaled registration immediately when no
packet was posted; an already-posted packet keeps storage pinned until dequeue.
If callback retirement first fails after consuming an auto-reset event,
semaphore count, or mode-unknown wait, the consumed notification is preserved
and replayed after cleanup succeeds.

`EPOLLET` uses observed-readiness filtering with throttled re-sampling of an
already-seen level. `EPOLLEXCLUSIVE` applies only to socket registrations and
may be combined with `EPOLLET`, but not with `EPOLLONESHOT`, `EPOLLRDHUP`, or
unsupported event bits. Every MOD of a registration added exclusive returns
`EINVAL`, even when the MOD mask omits `EPOLLEXCLUSIVE`. An allocation-free
process-wide claim index uses intrusive registration nodes and hash buckets to
track read, write, and terminal readiness independently, so a continuously
writable exclusive owner does not suppress a disjoint read wake. Windows
`epoll_pwait*` accepts a non-null signal-mask pointer and ignores it (there is
no POSIX process signal mask). Linux `epoll_pwait2_ex` applies a supplied mask
atomically through the native wait. `EPOLLWAKEUP` is accepted and ignored on
Windows. Call `wepoll_close()` for the virtual Windows epoll descriptor and for
prompt Linux extended-wait wakeup.

Remaining platform limits are explicit: Windows signal masks and
`EPOLLWAKEUP` have no native effect, `epoll_pwait2*` has millisecond timeout
resolution, edge delivery is observed-level rather than Linux kernel queue
semantics, exclusive-claim updates serialize through one process-wide mutex,
and virtual epoll descriptors cannot be nested.

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
`wepoll_ex_get_stats()` and `wepoll_ex_get_global_stats()` copy versioned,
size-prefixed operational snapshots. Windows exposes registration, queue,
pool, stale-event, identity, asynchronous-error, drain-budget, quarantine,
reaper, and close-timeout counters. Linux reports its extension-owned
registration count and marks the socket policy not applicable; unsupported
Windows-only counters are zero. These are diagnostics, not an atomic
transactional view.

## Repository layout

```
include/   public headers
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
Its 50k point measures registration scaling, not a 50k armed-ready workload,
and the benchmark intentionally has no pass/fail latency thresholds.

Linux qualification covers API contracts, close/wait/cancellation races,
native `epoll_pwait2` where libc and the kernel provide it, plus a separately
forced fallback build, signal-mask waits, metadata changes, reused-fd identity,
the pool, and package consumption. The MinGW suite covers TCP and UDP
readiness, IPv6, provider-handle fallback,
multi-epfd waits, deferred ADD failure, pending MOD transitions, IOCP batch
draining, timeout deadlines, fail-at-N injection, bounded close/quarantine
cleanup, randomized lifecycle stress, and package consumers. MinGW
final binaries select the static winpthreads archive, and CTest rejects an
accidental `libwinpthread-1.dll` dependency. Record the exact command,
compiler, Windows version, and build flags for new results.

The installed-package test builds and runs the default target plus every
exported explicit shared/static target using the active compiler and linker
flags. It rejects incompatible preview-version requests and verifies that only
the requested components are exported. A companion shared-library test pins
the public symbol list and ELF SONAME contract.

## Install and consume

Version 0.1.0 is an experimental preview and does not promise a stable ABI.
Each preview release therefore uses exact CMake package compatibility and an
exact-version ELF SONAME. Install it to an isolated prefix while evaluating it:

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
