# wepoll-ex design notes

This document is the architectural deep-dive. For build/usage see
`README.md`; for nginx integration see `NGINX_INTEGRATION.md`.

## Design goals

1. **Drop-in source compatibility.** Code that targets Linux
   `<sys/epoll.h>` must compile and run unchanged against
   wepoll-ex on Windows. The basic `epoll_create` / `epoll_ctl` /
   `epoll_wait` family takes identical arguments, returns identical
   values, and sets identical errno codes.

2. **Production server throughput.** nginx, envoy, haproxy, and
   redis all push hundreds of thousands of events per second per
   worker. wepoll-ex must not be the bottleneck. That means:
   - One kernel round-trip per registered fd's lifecycle (the
     initial `AFD_POLL`), and one kernel round-trip per delivered
     event (the IOCP completion).
   - Lock-free fast paths for the steady-state event-delivery case.
   - Bounded memory growth — the per-fd table grows geometrically
     but never leaks on `EPOLL_CTL_DEL`.

3. **Faithful edge-triggered semantics.** nginx and other Linux
   servers rely on `EPOLLET`'s "fire once per state transition"
   guarantee. wepoll-ex tracks per-fd pending events to suppress
   duplicate deliveries while still guaranteeing at-least-once
   notification per transition.

4. **Cheap one-shot re-arm.** nginx's worker model uses
   `EPOLLONESHOT` for SSL connections to avoid the SSL filter being
   re-entered while a previous read is still in flight. wepoll-ex's
   `epoll_rearm()` skips the full `EPOLL_CTL_MOD` validation path
   and just re-submits the cached AFD poll.

5. **Direct `ngx_event_t*` delivery.** On Linux nginx must hash
   `fd -> ngx_event_t*` after every `epoll_wait`. wepoll-ex's
   `user_ctx` extension lets the registration-time pointer ride
   through the IOCP completion into the delivered event, eliminating
   the lookup.

## Component overview

### `wepoll_ex_global.c`

Process-global one-shot initialization. Resolves
`NtDeviceIoControlFile` and `RtlNtStatusToDosError` from `ntdll.dll`
via `GetProcAddress`. The resolution is wrapped in
`pthread_once` so concurrent first-callers are safe.

On POSIX this layer is a no-op — the host libc already has epoll.

### `wepoll_ex_errno.c`

errno shim. Translates Windows `WSA*` error codes to POSIX errno
values via a giant switch (mirrors Cygwin's `winerr_to_errno`).
Also translates NTSTATUS codes by first going through
`RtlNtStatusToDosError` and then the WSA map.

This is critical for portable code. A typical Linux pattern:

```c
if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
    if (errno == EEXIST) { /* already registered */ }
}
```

must behave the same on Windows. Without the errno shim, the
Windows error code is `WSAEINVAL` for "already registered", which
would map to POSIX `EINVAL` and confuse the caller.

### `wepoll_ex_afd.c`

Helpers for the Ancillary Function Driver:

- `ep_afd_open()` opens `\Device\Afd\WepollEx` via `NtCreateFile`.
  One AFD handle is shared by every fd on a given epoll instance —
  we never need a per-fd handle because `AFD_POLL_HANDLE_INFO`
  carries the socket's `HANDLE` directly.
- `ep_afd_poll_submit()` builds an `AFD_POLL_INFO` and submits it
  via `NtDeviceIoControlFile(IOCTL_AFD_POLL)`. The request is
  asynchronous: it completes via the IOCP associated with the AFD
  handle.
- `ep_afd_to_epoll_events()` / `ep_epoll_to_afd_events()` translate
  the AFD event bit space to/from Linux's `EPOLL*` bit space.

AFD's event bits cover everything we need for Linux parity:

| AFD bit                       | Linux epoll |
|-------------------------------|-------------|
| `AFD_POLL_RECEIVE`            | `EPOLLIN`   |
| `AFD_POLL_RECEIVE_EXPEDITED`  | `EPOLLPRI`  |
| `AFD_POLL_SEND`               | `EPOLLOUT`  |
| `AFD_POLL_DISCONNECT`         | `EPOLLHUP`  |
| `AFD_POLL_RECEIVE_DISCONNECT` | `EPOLLRDHUP`|
| `AFD_POLL_ABORT`              | `EPOLLERR \| EPOLLHUP` |
| `AFD_POLL_CONNECT_FAIL`       | `EPOLLERR`  |
| `AFD_POLL_LOCAL_CLOSE`        | `EPOLLHUP`  |

The `AFD_POLL_RECEIVE_DISCONNECT` bit is what makes `EPOLLRDHUP`
possible — it fires when the peer has sent a FIN but there may
still be data in the receive queue, which is exactly the
half-close condition `EPOLLRDHUP` was invented to signal.

### `wepoll_ex_port.c`

The heart of the engine. Manages:

1. **`ep_port_t` lifecycle** — `ep_port_create()` allocates the
   IOCP and AFD handle, pre-sizes the fd table if given a hint,
   initialises the AFD buffer pools and the MPSC ready queue, and
   returns an opaque pointer. `ep_port_destroy()` cancels all
   pending AFD polls, drains the ready queue, and frees all
   per-fd state.

2. **fd table** — open-addressing hash table with linear probing.
   Grows at 0.75 load factor, doubling in size. Removal uses the
   classic "re-insert subsequent chain entries" technique to
   preserve probe invariants.

3. **MPSC ready queue** — Michael-Scott lock-free queue with a
   sentinel stub. Producers (IOCP completion handler) append via
   `atomic_exchange(&tail, node)` + `atomic_store(&prev->next, node)`.
   Consumer (`epoll_wait`) drains via repeated standard MS-dequeue
   (CAS `stub->next` from `first` to `first->next`).

   The queue handles the classic "producer mid-publish" race by
   spinning briefly when `first->next == NULL` but `first != tail`.
   This is bounded and rare — in steady state the spin terminates
   in <10 ns.

4. **AFD buffer pools** — two LIFO stacks of pre-allocated
   buffers:
   - `afd_info_pool`: `AFD_POLL_INFO` buffers (one per registered
     fd, recycled on `EPOLL_CTL_DEL`).
   - `ready_node_pool`: `ep_ready_node_t` nodes (one per delivered
     event, recycled after `epoll_wait` consumes them).

   Both pools grow on-the-fly if exhausted (rare under steady
   state) by `malloc`-ing a fresh entry.  The fresh entry's header
   tags it with the owning pool so it returns to the right place
   on `give`.

5. **IOCP batch dispatch** — `ep_port_wait()` calls
   `GetQueuedCompletionStatusEx(iocp, entries, 64, &removed, ...)`
   to retrieve up to 64 completions in one syscall.  Each
   completion is dispatched via `ep_sock_handle_completion()`,
   which pushes a node onto the MPSC ready queue.  After all
   completions are dispatched, the ready queue is drained into
   the caller's `epoll_event_ex` buffer.

   Compared to `GetQueuedCompletionStatus` (one completion per
   syscall), `GetQueuedCompletionStatusEx` cuts syscall cost by
   ~10x under bursty workloads.

6. **IOCP dispatch** — `ep_sock_handle_completion()` is called
   for every IOCP completion. The function:
   - Reads the AFD output buffer to find which events fired.
   - Masks by the user-requested event mask.
   - For `EPOLLET`, suppresses bits already in `pending_events`.
   - For `EPOLLONESHOT`, sets `oneshot_fired` and skips re-arming
     AFD.
   - Otherwise re-arms AFD immediately for the next event.
   - Pushes an `ep_ready_node_t` onto the MPSC ready queue
     (allocated from `ready_node_pool` — no `malloc`).

### `wepoll_ex_pool.c`

Platform-independent AFD buffer pool + MPSC ready queue
implementation.  Compiled into both the Windows and POSIX builds.

Key design choices:

- **LIFO stack for the pool.**  A simple `atomic_compare_exchange`
  loop on a single `head` pointer.  No mutex.  When the pool is
  exhausted, a fresh `malloc`'d entry is returned and tagged
  with the owning pool — so it returns to the right place on
  `give`.  This handles burst growth gracefully.

- **Michael-Scott queue with a permanent sentinel.**  The stub
  is allocated once at `ep_ready_init` and freed at
  `ep_ready_destroy`.  It's never returned to the pool.  Producers
  only mutate `tail` (via `atomic_exchange`) and `prev->next`
  (via `atomic_store`).  Consumer mutates `stub->next` (via CAS)
  and `tail` (via CAS, only when the queue becomes empty).

- **One-node-at-a-time dequeue, batched.**  `ep_ready_drain`
  repeats the standard MS-dequeue up to `maxevents` times,
  building a singly-linked output chain.  Each dequeue is
  individually atomic, so we never orphan a producer's pending
  push.  The cost is one CAS per drained node — acceptable
  because the consumer is single-threaded and CASes are
  uncontended.

- **Spin-wait for mid-publish producers.**  When
  `first->next == NULL` but `first != tail`, a producer is
  mid-publish.  We spin on `pause`/`YieldProcessor` until either
  `first->next` becomes non-NULL (producer published) or
  `first == tail` (queue really ends here).  The spin is bounded
  by the producer's store latency, which is sub-microsecond in
  practice.

### `wepoll_ex_api.c`

Public API surface. Wraps the internal port layer with the
Linux-shaped integer `epfd` API.

Since Windows `SOCKET` handles are not small integers (they're
kernel HANDLEs), we can't return them directly as `int epfd`.
Instead we maintain a process-global table mapping small integers
(starting at 1) to `ep_port_t*` pointers. The table is
linear-probed and small (1024 slots); for typical server
configurations with a handful of epoll instances per process,
lookups are O(1).

### `wepoll_ex_posix.c`

POSIX wrapper. Used when building on Linux/macOS for testing and
for cross-platform code that uses the extension API.

The basic `epoll_create` / `epoll_ctl` / `epoll_wait` family is
provided by the host libc — we don't try to redefine them. Only
the extension API (`epoll_ctl_ctx`, `epoll_wait_ex`,
`epoll_ctl_batch`, `epoll_rearm`, `epoll_fd_count`, `wepoll_close`)
is implemented here, as a thin layer that maintains a per-epfd
hash table of `{fd, user_ctx, user_flags}` triples.

### `nginx/ngx_wepoll_module.c`

Drop-in replacement for `src/event/modules/ngx_epoll_module.c` in
the nginx source tree. Implements `ngx_event_module_t` with these
notable differences from stock:

- Uses `epoll_ctl_ctx()` to register `ngx_event_t*` per fd.
- Uses `epoll_wait_ex()` to retrieve `ngx_event_t*` directly from
  the event — no `ngx_cycle->connections[]` lookup.
- Defaults to `EPOLLET` for all events, matching nginx's Linux
  behaviour.
- Uses `EPOLLRDHUP` so nginx's existing `rev->pending_eof` path
  fires on peer half-close.
- Supports `EPOLLONESHOT` for SSL connections via
  `ngx_wepoll_rearm()`.

## Performance notes

### Steady-state event delivery

For a typical "accept -> read -> write -> close" cycle, the kernel
round-trips are:

1. `epoll_ctl(ADD)` -> one `AFD_POLL` submit (1 syscall).  Buffer
   for `AFD_POLL_INFO` is taken from the pool — no `malloc`.
2. Data arrives -> AFD poll completes (kernel-side, no syscall).
3. `epoll_wait` -> `GetQueuedCompletionStatusEx` returns up to 64
   completions (1 syscall amortised across all delivered events).
4. Each completion pushes a node onto the MPSC ready queue
   (lock-free, no syscall).
5. AFD is automatically re-armed by the completion handler (1
   syscall per delivered event).

So the cost is ~2 syscalls per delivered event amortised —
comparable to Linux's `epoll_wait + EPOLLET re-arm`.  The
`GetQueuedCompletionStatusEx` batch is the key win: under a
1000-event burst, we make 16 syscalls (1000/64) instead of 1000.

### Memory footprint

Per registered fd:

- `ep_sock_t` structure: ~96 bytes.
- `AFD_POLL_INFO` buffer (pooled): ~32 bytes.
- Hash table slot: ~8 bytes.

Total: ~136 bytes per connection. For a 100k-connection nginx
worker that's ~13 MB — negligible.

The AFD buffer pools pre-allocate `WEPOLL_AFD_POOL_SIZE` (default
256) buffers each at port creation.  That's ~16 KB upfront per
pool — invisible.  Under burst, the pools grow on-the-fly; the
peak usage is tracked via an atomic counter for diagnostics.

### Latency

On Linux (POSIX wrapper), round-trip latency is dominated by the
cost of `epoll_wait` + `write` + `read`:

```
min : 580 ns
avg : 645 ns
max : 25519 ns   (timer tick / scheduler)
```

On Windows (IOCP+AFD with batched delivery + MPSC queue), expect
roughly 2-3x this for the syscall cost, but the batched delivery
under bursty workloads closes the gap considerably — a 64-event
burst costs one `GetQueuedCompletionStatusEx` call instead of 64
`GetQueuedCompletionStatus` calls.

## Known limitations

1. **AFD is undocumented.** Microsoft has never published the
   `IOCTL_AFD_POLL` interface. wepoll pioneered the use of this
   interface and it has been stable since Windows 8, but there's
   no formal contract.

2. **No file handles.** AFD only polls sockets. Regular files on
   Windows are always "ready" and don't need polling — but if
   user code registers a non-socket HANDLE, `AFD_POLL` will
   fail. Linux's epoll similarly doesn't support regular files
   with `EPOLLET`, so this matches.

3. **`EPOLLEXCLUSIVE` is approximated.** True exclusive wake-up
   would require a kernel-level thundering-herd suppression that
   AFD doesn't provide. wepoll-ex's `EPOLLEXCLUSIVE` ensures only
   one epoll instance per fd (returning `EEXIST` if a second
   tries to register), which is the common case for nginx.

4. **`epoll_pwait` ignores `sigmask`.** Windows doesn't have
   POSIX signals. The `sigmask` argument is accepted for API
   compatibility but has no effect. Callers that need signal
   interruption should use a separate signal-handler-driven
   self-pipe.

5. **POSIX build is for testing only.** The POSIX wrapper doesn't
   implement the full `epoll_event_ex` semantics — in particular
   the `timestamp` field is set to the `epoll_wait` return time,
   not the actual kernel completion time. On Windows it's set to
   the IOCP completion time, which is what production code wants.

## Future work

- **Lock-free ready queue.** The current mutex-protected queue is
  fine for single-worker epoll instances, but multi-worker designs
  (where multiple threads call `epoll_wait` on the same epfd)
  would benefit from a lock-free MPSC queue.

- **`epoll_rearm` for non-oneshot fds.** Currently `epoll_rearm`
  is only meaningful for `EPOLLONESHOT` fds. A version that
  refreshes the AFD poll without going through `EPOLL_CTL_MOD`
  could shave ~200ns off the steady-state hot path.

- **`SO_REUSEPORT`-style fan-out.** For nginx with multiple
  workers accepting on the same port, we could implement a
  shared accept-queue with `EPOLLEXCLUSIVE`-style wake-up
  distribution in user mode.

- **AFD pool.** The `afd_pool` field in `ep_port_t` is currently
  unused. Pre-allocating `AFD_POLL_INFO` buffers and recycling
  them through a per-port pool would eliminate the `calloc` on
  every `EPOLL_CTL_ADD`, which shows up as a measurable cost on
  accept-burst workloads.
