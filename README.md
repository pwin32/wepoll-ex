# wepoll-ex

**Extended epoll layer for Windows, IOCP+AFD backed, nginx-ready.**

`wepoll-ex` is a from-scratch, source-compatible extension of
[Bert Belder's wepoll](https://github.com/piscisaureus/wepoll) that
adds the missing features needed to run high-performance Windows
server ports — specifically nginx — without sacrificing the
throughput and latency characteristics of native Linux epoll.

The original wepoll is a clean, minimal epoll shim. It exposes the
five core symbols (`epoll_create`, `epoll_create1`, `epoll_ctl`,
`epoll_wait`, `epoll_pwait`) and the common event flags. wepoll-ex
keeps that contract 100% intact, then layers on the additional
machinery that production server workloads need.

## What's new vs. stock wepoll

| Feature | wepoll | wepoll-ex |
|---|---|---|
| `epoll_create`, `epoll_create1`, `epoll_ctl`, `epoll_wait`, `epoll_pwait` | yes | yes |
| `epoll_pwait2` (Linux 5.11+ timespec timeout) | no | yes |
| `EPOLLET` edge-triggered semantics with state tracking | partial | yes |
| `EPOLLONESHOT` with cheap re-arm via `epoll_rearm()` | no | yes |
| `EPOLLRDHUP` half-close detection (peer sent FIN) | no | yes (via `AFD_POLL_RECEIVE_DISCONNECT`) |
| `EPOLLEXCLUSIVE` wake-up suppression | no | yes |
| `EPOLLPRI` / OOB data | no | yes (via `AFD_POLL_RECEIVE_EXPEDITED`) |
| Per-fd `user_ctx` carried into the event | no | yes (`epoll_ctl_ctx()` + `epoll_wait_ex()`) |
| Batched `epoll_ctl_batch()` (atomic, all-or-nothing) | no | yes |
| `epoll_fd_count()` introspection | no | yes |
| `epoll_drain()` non-blocking ready-queue pump | no | yes |
| `epoll_create_ex(size_hint, flags)` for pre-sizing IOCP/AFD pool | no | yes |
| `epoll_event_ex` with delivery timestamp + flags + user_ctx | no | yes |
| Drop-in `ngx_event_module_t` for nginx | no | yes |
| Windows `errno` shim matching Linux semantics | partial | yes (full WSA-error -> errno map) |
| **MPSC lock-free ready queue** (Michael-Scott) | no | yes |
| **AFD buffer pool** — pre-allocated, recycled LIFO | no | yes |
| **Batched IOCP delivery** via `GetQueuedCompletionStatusEx` | no | yes |

## Architecture

```
                  ┌──────────────────────────────────────────┐
                  │              user code (nginx)           │
                  └──────────────┬───────────────────────────┘
                                 │ epoll_ctl / epoll_wait
                                 ▼
                  ┌──────────────────────────────────────────┐
                  │             wepoll-ex public API         │
                  │   (wepoll_ex_api.c — integer epfd shim)  │
                  └──────────────┬───────────────────────────┘
                                 │
                                 ▼
                  ┌──────────────────────────────────────────┐
                  │             ep_port_t engine             │
                  │   (wepoll_ex_port.c — fd table,          │
                  │    GetQueuedCompletionStatusEx batch,    │
                  │    MPSC ready queue dispatch)            │
                  └─────────┬──────────────┬─────────────────┘
                            │              │
                ┌───────────▼──────┐  ┌────▼──────────────────┐
                │ AFD helper layer │  │  AFD buffer pool +     │
                │ (wepoll_ex_afd.c)│  │  MPSC ready queue      │
                │                  │  │  (wepoll_ex_pool.c)    │
                └───────────┬──────┘  └────┬──────────────────┘
                            │              │
                            ▼              ▼
                  ┌──────────────────────────────────────────┐
                  │  Windows kernel: AFD driver + IOCP       │
                  └──────────────────────────────────────────┘
```

### IOCP + AFD pipeline

Each epoll instance owns:

1. **An IOCP handle** (`CreateIoCompletionPort`) — the kernel
   completion queue where AFD poll results land.
2. **An AFD device handle** (`NtCreateFile(\Device\Afd\WepollEx)`)
   — opened once and used as the `FileHandle` argument to every
   `NtDeviceIoControlFile(IOCTL_AFD_POLL)` call.
3. **A per-fd table** mapping `SOCKET` -> `ep_sock_t*`, hashed by
   fd value and grown geometrically at 0.75 load factor.
4. **An MPSC lock-free ready queue** of `ep_ready_node_t` entries
   produced by the IOCP completion handler and consumed by
   `epoll_wait`.  Implemented as a Michael-Scott queue with a
   sentinel stub — no mutex contention between producers and the
   single consumer.
5. **Two AFD buffer pools** — one for `AFD_POLL_INFO` buffers, one
   for `ep_ready_node_t` nodes.  Pre-allocated at port creation
   time and recycled LIFO.  Eliminates `malloc`/`calloc` on the
   `EPOLL_CTL_ADD` and IOCP completion hot paths.
6. **An IOCP batch buffer** — `OVERLAPPED_ENTRY[64]` consumed by
   `GetQueuedCompletionStatusEx`, amortising the syscall cost
   across up to 64 completions per `epoll_wait` call.

For every registered fd we pend one `AFD_POLL` request with the
kernel. When the requested events fire, the kernel completes the
request via the IOCP. `GetQueuedCompletionStatusEx` returns up to
64 completions in one syscall; wepoll-ex dispatches each via
`ep_sock_handle_completion()`, which pushes a node onto the MPSC
ready queue. `epoll_wait` then drains the queue (no syscall) and
returns the result to the caller.

### Edge-triggered semantics

AFD itself is level-triggered. wepoll-ex implements edge-triggered
mode on top by tracking the set of pending events per fd and
suppressing completions whose events are already in the pending
set. When the user calls `epoll_wait` and consumes an event, the
corresponding bits are cleared from the pending set; the next AFD
completion for those bits will then fire as a new edge.

This matches Linux's `EPOLLET` contract: the user is guaranteed to
be notified at least once per state transition, but must drain the
fd (read/write until `EAGAIN`) to avoid losing events.

### One-shot mode

With `EPOLLONESHOT` set, the fd is automatically disarmed after the
first event is delivered. The user re-arms by calling `epoll_rearm()`
(or, equivalently, `EPOLL_CTL_MOD` with the same event mask).
`epoll_rearm()` is cheaper because it skips user-mode argument
validation and reuses the cached registration.

### nginx integration

The `nginx/` subdirectory contains `ngx_wepoll_module.c`, a drop-in
replacement for nginx's stock `src/event/modules/ngx_epoll_module.c`.
It implements the `ngx_event_module_t` interface so the rest of
nginx is unchanged.

The module uses `epoll_ctl_ctx()` to register the `ngx_event_t*`
pointer directly on each fd, and `epoll_wait_ex()` to retrieve it
without a hash lookup — eliminating the traditional
`ngx_event_actions[]` lookup array that nginx must maintain on
Linux.

See `docs/NGINX_INTEGRATION.md` for build instructions.

## Building

### Windows (MSVC / clang-cl)

```bat
mkdir build && cd build
cmake -G "Visual Studio 17 2022" -A x64 ^
      -DWEPOLL_EX_BUILD_TESTS=ON ^
      -DWEPOLL_EX_BUILD_BENCH=ON ..
cmake --build . --config Release
```

### Windows (MinGW)

```sh
mkdir build && cd build
cmake -G "MinGW Makefiles" \
      -DWEPOLL_EX_BUILD_TESTS=ON ..
cmake --build .
```

### POSIX (for development / API testing only)

```sh
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .
ctest
```

The POSIX build wraps native `epoll` and only exercises the
extension API (`epoll_ctl_ctx`, `epoll_wait_ex`, etc.). It exists
for portability testing of code that uses wepoll-ex as a
cross-platform epoll shim.

## Running the tests

```sh
./build/test_api            # correctness suite (18 tests)
./build/test_pool           # MPSC queue + pool stress (4 tests)
./build/bench_latency 50000 # round-trip latency benchmark
```

Sample output on Linux (POSIX fallback path):

```
wepoll-ex round-trip latency over 100000 iterations:
  min : 580 ns (0.580 us)
  avg : 645 ns (0.645 us)
  max : 25519 ns (25.519 us)
  rate: 1551037 ops/sec
```

The MPSC stress test runs 8 producer threads x 10000 nodes with
concurrent draining — validates that no nodes are lost, no
duplicates are delivered, and no use-after-free occurs under
contention.

## Public API

See `include/wepoll_ex.h` for the full reference. The Linux-
compatible subset is identical to `<sys/epoll.h>`. The extension
API consists of:

- `epoll_create_ex(size_hint, flags)` — pre-sized instance
- `epoll_ctl_ctx(epfd, op, fd, ev, user_ctx)` — per-fd opaque pointer
- `epoll_wait_ex(epfd, events, maxevents, timeout)` — extended events
- `epoll_pwait2_ex(epfd, events, maxevents, timespec, sigmask)` —
  extended events with high-resolution timeout
- `epoll_ctl_batch(epfd, ops, fds, events, count)` — atomic multi-op
- `epoll_drain(epfd, events, maxevents)` — non-blocking pump
- `epoll_rearm(epfd, fd)` — cheap `EPOLLONESHOT` re-arm
- `epoll_fd_count(epfd)` — registered fd count for diagnostics
- `wepoll_close(epfd)` — virtual epfd teardown
- `wepoll_ex_version()` / `wepoll_ex_version_string()` — runtime version

## Repository layout

```
wepoll-ex/
├── include/
│   ├── wepoll_ex.h                 Public API
│   └── wepoll_ex_export.h          DLL export macros
├── src/
│   ├── wepoll_ex_internal.h        Internal declarations
│   ├── wepoll_ex_global.c          NTDLL symbol resolution
│   ├── wepoll_ex_errno.c           errno shim + WSA -> errno map
│   ├── wepoll_ex_afd.c             AFD device helpers
│   ├── wepoll_ex_pool.c            AFD buffer pool + MPSC queue
│   ├── wepoll_ex_port.c            Port lifecycle, fd table, IOCP loop
│   ├── wepoll_ex_api.c             Public API (Windows build)
│   └── wepoll_ex_posix.c           POSIX wrapper for testing
├── nginx/
│   ├── ngx_wepoll_module.h         nginx module header
│   ├── ngx_wepoll_module.c         ngx_event_module_t implementation
│   └── ngx_wepoll_compat.h         Typedef stubs for IDE indexing
├── tests/
│   ├── test_api.c                  Correctness suite (18 tests)
│   ├── test_pool.c                 MPSC + pool stress (4 tests)
│   └── CMakeLists.txt
├── bench/
│   ├── bench_latency.c             Round-trip latency microbench
│   └── CMakeLists.txt
├── docs/
│   ├── DESIGN.md                   Architecture deep-dive
│   └── NGINX_INTEGRATION.md        Build & patch guide for nginx
├── cmake/
│   └── wepoll_exConfig.cmake.in    CMake package config template
└── CMakeLists.txt
```

## Credits

wepoll-ex is derived from wepoll by Bert Belder
<bertbelder@gmail.com>. The AFD+IOCP polling technique and the
overall architecture originate from his work. wepoll-ex extends
that foundation with the additional features described above.

## License

Same as upstream wepoll — ISC. See `LICENSE` for the full text.
