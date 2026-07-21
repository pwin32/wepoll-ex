# nginx Adapter Status and Validation Plan

## Current status

The files in `nginx/` are an experimental event-module sketch. They are not a
supported drop-in backend, are not wired into CMake, and are not currently
buildable or validated against the local `nginx-1.31.3.tar.gz` reference
archive. Do not apply the old copy-and-patch instructions to an nginx tree: no
verified `auto/sources`, `auto/os/win32`, or linker recipe exists yet.

Keep `nginx-1.31.3.tar.gz` untracked. If it is unpacked for investigation, use
a temporary directory outside this repository.

## What the prototype contains

- `ngx_wepoll_module.c` attempts to implement an nginx event module using
  `epoll_ctl_ctx`, `epoll_wait_ex`, and `wepoll_close`.
- `ngx_wepoll_module.h` declares the module objects and depends on nginx's
  private headers.
- `ngx_wepoll_compat.h` contains incomplete IDE/test stubs; it is not a
  substitute for nginx 1.31.3 headers.

The current source also uses the undeclared name `epoll_event_ex_t` while the
public header defines `epoll_event_ex`. Its module structure, callback
signatures, event posting behavior, configuration directives, and Windows
linkage still need to be reconciled with nginx 1.31.3. The declared CMake
option `WEPOLL_EX_BUILD_NGINX` currently has no associated target.

## Validation checklist

1. Fix and pass the library's clean POSIX and MinGW configure/build checks.
2. Extract the reference archive outside the repository, for example:

   ```sh
   mkdir -p /tmp/wepoll-ex-nginx-reference
   tar -xzf nginx-1.31.3.tar.gz -C /tmp/wepoll-ex-nginx-reference
   ```

3. Compare `ngx_event_module_t`, `ngx_event_actions_t`, module initialization,
   and Windows event-selection code with the exact nginx 1.31.3 headers.
4. Compile `ngx_wepoll_module.c` against those headers before changing nginx's
   build scripts. Resolve all type and callback mismatches explicitly.
5. Add a repository-owned build target or a reproducible patch set that links
   the static library and its Windows dependencies. Do not rely on manual file
   copies as the source of truth.
6. Add adapter tests for add/modify/delete, read and write delivery, stale
   events, close races, posted events, timers, half-close, and shutdown.
7. Build and run nginx on Windows, record the compiler and OS version, run
   `nginx -t`, then exercise HTTP keep-alive and graceful reload/shutdown.
8. Only after functional tests pass, collect comparative latency or throughput
   data and document the exact workload. Do not publish scaling claims from the
   POSIX microbenchmark.

## MinGW environment

Use the requested MSYS2 shell and toolchain when performing the Windows build:

```sh
/path/to/msys64/usr/bin/bash.exe -lc \
  'export PATH=/mingw64/bin:/usr/bin:$PATH; cd /e/personal/wepoll-ex; \
   cmake -S . -B build-mingw -G "MinGW Makefiles" -DWEPOLL_EX_BUILD_TESTS=OFF; \
   cmake --build build-mingw --parallel'
```

This command currently reaches the known CMake export-generation failure; it
is included to define the environment, not to claim a successful nginx build.
