# Design Notes

## Status

This is an architectural description of an experimental prototype. It records
what the source currently attempts to do; it is not a compatibility promise or
a performance specification. The Windows path depends on undocumented AFD
structures and has not been validated on a Windows test matrix. The POSIX path
is a wrapper around native `epoll`, so passing POSIX tests does not validate the
Windows engine.

## Build-time split

`CMakeLists.txt` selects one implementation set:

| Platform | Library sources | Purpose |
| --- | --- | --- |
| Windows | `wepoll_ex_global.c`, `wepoll_ex_errno.c`, `wepoll_ex_afd.c`, `wepoll_ex_pool.c`, `wepoll_ex_port.c`, `wepoll_ex_api.c` | IOCP/AFD prototype |
| POSIX | `wepoll_ex_posix.c`, `wepoll_ex_pool.c` | Native-epoll wrapper and queue/pool tests |

The `WEPOLL_EX_BUILD_NGINX` option is declared but does not currently wire the
`nginx/` sources into a build target. The adapter therefore requires a separate
integration experiment.

## Windows data flow

1. `epoll_create*` allocates an `ep_port_t`, creates an IOCP, opens AFD, and
   installs the port in the process-global integer-`epfd` table.
2. `epoll_ctl(ADD)` creates an `ep_sock_t`, stores the event mask/data/context,
   inserts it into the open-addressed fd table, and submits an `AFD_POLL`.
3. AFD reports readiness through an IOCP completion. The completion handler
   translates AFD bits to `EPOLL*`, applies the stored mask and provisional
   edge/oneshot bookkeeping, and pushes an `ep_ready_node_t`.
4. `epoll_wait` first drains the ready queue, then waits for completions and
   drains again. `epoll_wait_ex` copies the node's data, context, flags, and
   monotonic timestamp to the caller's buffer.
5. `EPOLL_CTL_DEL`, `wepoll_close`, and port teardown remove table entries and
   return pooled buffers.

The queue is implemented as a Michael–Scott-style producer append with a
single consumer; the pool is a lock-free LIFO freelist with malloc fallback.
These are implementation details under test, not established concurrency
guarantees. In-flight completion, deletion, and multi-consumer behavior still
need dedicated regression coverage.

## POSIX data flow

The host libc owns the basic epoll descriptor and readiness semantics.
`wepoll_ex_posix.c` lazily associates an epfd with a mutex-protected hash table
of `{ fd, user_ctx, user_flags }`. Extension calls forward to native epoll and
decorate returned events. `epoll_ctl_batch` applies operations sequentially and
attempts rollback; it is not truly atomic. `epoll_pwait2_ex` converts a
timespec to milliseconds, and the POSIX extension does not implement signal
mask handling.

## Boundaries and known gaps

- AFD is undocumented and socket-only; no support claim is made for arbitrary
  Windows handles.
- Windows `sigmask` arguments are accepted for API shape but have no signal
  semantics.
- Edge-triggered, oneshot, RDHUP, exclusive wakeups, timestamp meaning, and
  deletion races are not yet proven equivalent to Linux.
- The optional benchmark measures the POSIX wrapper only; its numbers must not
  be extrapolated to IOCP/AFD.
- The nginx adapter is source-only and unvalidated; see
  [`NGINX_INTEGRATION.md`](NGINX_INTEGRATION.md).

## Verification baseline

The intended commands and executable paths are documented in `README.md`.
Currently, a clean configure fails while exporting the static target because
`wepoll_ex_compile_defs` is not in an export set. In an existing generated
POSIX tree, `tests/test_wepoll_ex` builds and runs, while
`tests/test_wepoll_ex_pool` fails to compile because its target does not include
`src/`. Fix those build blockers before using test results as an architectural
signal.
