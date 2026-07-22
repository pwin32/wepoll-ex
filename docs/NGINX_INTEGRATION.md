# nginx Adapter Status and Validation Plan

## Current status

`nginx/` contains an experimental event-module sketch only. It is not wired
into CMake, is not a supported nginx backend, and has not yet been compiled
against the local nginx 1.31.3 headers. `nginx-1.31.3.tar.gz` is reference
material and must remain ignored; unpack it outside this repository for
experiments.

The sketch currently needs source-level reconciliation before it can be
considered an adapter. In particular, `ngx_wepoll_module.c` uses the stale
`epoll_event_ex_t` spelling while the public header declares
`epoll_event_ex`, assumes Linux edge-triggered behavior even though Windows
rejects `EPOLLET`, and uses nginx-private callback/configuration details that
must match the exact release headers. The `WEPOLL_EX_BUILD_NGINX` CMake option
therefore remains an explicit warning with no generated target.

## Architecture constraints

The adapter may use `epoll_ctl_ctx()` and `epoll_wait_ex()` to carry an
`ngx_event_t *`, but it must treat the context as a registration snapshot:
MOD, DEL, native socket close, and worker shutdown can invalidate it before a
queued event is consumed. It must also use level-triggered registrations on
Windows or implement a separate, tested drain/rearm state machine; simply
copying nginx's Linux `EPOLLET` mask is incorrect.

`wepoll_close()` is required for the virtual epoll descriptor. The adapter must
not call plain `close()` on it, and must coordinate teardown so no nginx event
handler can use a context after the port has begun closing.

## Validation sequence

1. Build the library and pass its POSIX and MinGW CTest baselines first.
2. Extract the reference archive outside the repository:

   ```sh
   mkdir -p /tmp/wepoll-ex-nginx-reference
   tar -xzf nginx-1.31.3.tar.gz -C /tmp/wepoll-ex-nginx-reference
   ```

3. Compare `ngx_event_module_t`, `ngx_event_actions_t`, module lifecycle,
   event flags, and Windows event-selection code with the exact headers.
4. Correct the adapter types and callbacks, then compile only the module source
   against those headers before changing nginx build scripts.
5. Add a reproducible CMake or patch-based integration target that links the
   library and its Windows dependencies. Do not rely on copied generated files.
6. Add adapter tests for add/modify/delete, read/write delivery, stale queued
   events, native close, half-close, timers, posted events, worker shutdown,
   and graceful reload.
7. Build nginx with the requested MinGW shell, record compiler/OS versions,
   run `nginx -t`, and exercise keep-alive plus graceful shutdown/reload.
8. Only after functional tests pass, collect latency or throughput data with a
   reproducible workload. The repository benchmark measures POSIX only.

## Toolchain reminder

Use `/path/to/msys64/usr/bin/bash.exe` with `/mingw64/bin:/usr/bin` first in
`PATH`, and keep temporary nginx extraction/build state outside the repository.
