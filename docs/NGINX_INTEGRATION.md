# nginx Adapter Status and Validation Plan

## Current status

`nginx/` contains an experimental nginx 1.31.3 event-module adapter. It is
now wired to nginx's `--add-module` hook and has passed strict object/full-link
checks plus a disposable MinGW HTTP loopback, reload, and graceful-quit smoke
test. It is not a supported nginx backend or a production validation matrix.
`nginx-1.31.3.tar.gz` is reference material and must remain ignored; unpack it
outside this repository for experiments.

The adapter follows nginx's level-triggered poll action layout and uses the
connection instance bit to reject stale queued events. The Windows backend
rejects `EPOLLET`, so the adapter intentionally does not request it. The
`nginx/config` hook compiles the static wepoll-ex sources into an nginx build;
the opt-in CMake target only checks the adapter object against generated nginx
headers.

The Win32 nginx 1.31.3 configure script rejects `--with-threads`, and its
thread-pool sources are Unix/POSIX-only. This adapter therefore leaves nginx's
optional `notify` action unset and does not claim thread-pool support on
Windows. A configuration that requires `thread_pool` must use a separately
validated platform/backend.

The addon compiles wepoll-ex with its Windows 8+ API assumptions even though
stock nginx 1.31.3 headers advertise an older Win32 target. Treat Windows 8 or
later (`_WIN32_WINNT=0x0602`) as the current minimum for this experiment.

## Architecture constraints

The adapter stores the nginx connection pointer and its instance bit in the
standard epoll event data. Every queued event is revalidated against the
connection's current descriptor and instance before dispatch. Registrations
must remain level-triggered on Windows unless a separate, tested drain/rearm
state machine is implemented; copying nginx's Linux `EPOLLET` mask is
incorrect.

`wepoll_close()` is required for the virtual epoll descriptor. The adapter must
not call plain `close()` on it, and must coordinate teardown so no nginx event
handler can use a context after the port has begun closing.

The `wepoll_events` directive is restricted to a positive value that fits the
`epoll_wait()` event-array and allocation limits; the default is 512.

## Validation sequence

1. Build the library and pass its POSIX and MinGW CTest baselines first.
2. Extract the reference archive outside the repository:

   ```sh
   mkdir -p /tmp/wepoll-ex-nginx-reference
   tar -xzf nginx-1.31.3.tar.gz -C /tmp/wepoll-ex-nginx-reference
   ```

3. Compare `ngx_event_module_t`, `ngx_event_actions_t`, module lifecycle,
   event flags, and Windows event-selection code with the exact headers.
4. Configure nginx with the addon, then compile it with nginx's own Makefile:

   ```sh
   ./configure --crossbuild=win32 --builddir=objs-wepoll \
       --with-cc=gcc --without-pcre \
       --without-http_rewrite_module --without-http_gzip_module \
       --without-select_module --without-poll_module \
       --add-module=/e/personal/wepoll-ex/nginx
   mingw32-make -f objs-wepoll/Makefile -j4
   mingw32-make -f objs-wepoll/Makefile \
       objs-wepoll/addon/nginx/ngx_wepoll_module.o \
       objs-wepoll/ngx_modules.o
   ```

   The tracked `nginx/config` hook uses Windows-style paths so native
   `mingw32-make` can track addon dependencies.
5. For a minimal end-to-end check, build nginx with HTTP (disabling optional
   PCRE-dependent rewrite/gzip modules), configure `events { use wepoll; }`,
   and request a static loopback resource. This has passed locally with nginx
   1.31.3 and the requested MSYS2 GCC toolchain.
6. Add adapter tests for add/modify/delete, read/write delivery, stale queued
   events, native close, half-close, timers, posted events, worker shutdown,
   and graceful reload.
7. Build nginx with the requested MinGW shell, record compiler/OS versions,
   run `nginx -t`, and exercise keep-alive plus graceful shutdown/reload. This
   has passed locally in normal master/worker mode with 100 loopback requests
   spanning a reload. Stock Win32 nginx's `master_process off` path is a stub
   that handles hard stop only, so it is not suitable for quit/reload testing.
8. Only after functional tests pass, collect latency or throughput data with a
   reproducible workload. The repository benchmark measures POSIX only.

## Opt-in CMake compile check

Configure the library with an already configured nginx tree and its generated
headers (for example `objs-wepoll`):

```sh
cmake -S . -B build-nginx-adapter-check -G "MinGW Makefiles" \
  -DWEPOLL_EX_BUILD_NGINX=ON \
  -DWEPOLL_EX_NGINX_SOURCE_DIR=D:/path/to/nginx-1.31.3 \
  -DWEPOLL_EX_NGINX_BUILD_DIR=D:/path/to/nginx-1.31.3/objs-wepoll
cmake --build build-nginx-adapter-check --target wepoll_ex_nginx_adapter
```

This target does not claim that nginx's full executable or runtime behavior is
supported; it verifies the adapter against the exact generated ABI.

## Toolchain reminder

Use `/path/to/msys64/usr/bin/bash.exe` with `/mingw64/bin:/usr/bin` first in
`PATH`, and keep temporary nginx extraction/build state outside the repository.
