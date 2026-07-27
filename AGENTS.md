# Repository Guidelines

## Project Status and Layout

`wepoll-ex` 0.1.0 is an experimental C11/CMake project. The Windows backend
uses IOCP and the undocumented AFD poll interface (Windows 8+ assumptions);
the Linux-only development build wraps native `epoll`. Use `include/` for
public headers, `src/` for implementations, `tests/`, `bench/`, `scripts/`,
`nginx/` for the adapter, and `docs/` for notes. The nginx, libuv, asio, and
mio archives are reference-only; never stage them.

## Build, Test, and Development Commands

Use disposable build directories. A strict Linux release check is:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DWEPOLL_EX_BUILD_BENCH=ON \
  -DCMAKE_C_FLAGS="-O3 -Wall -Wextra -Wpedantic -Werror"
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure
./build-release/bench/bench_latency 200000
ulimit -n 65536  # required when the scaling run exceeds the shell soft limit
./build-release/bench/bench_wait_scaling 1024 20000
```

`./scripts/qualify-posix.sh` runs strict release, an explicitly forced
`epoll_pwait2` fallback, repeated API/pool tests, and ASan/UBSan. Checked-in
presets require CMake 3.21; ordinary builds require CMake 3.16.

For Windows, use `/path/to/msys64/usr/bin/bash.exe`, put `/mingw64/bin:/usr/bin`
first in `PATH`, configure with `-G "MinGW Makefiles"`, and run CTest. Validate
combined, static-only, and shared-only best-effort builds plus combined and
shared-only strict and synchronized builds through
`WEPOLL_EX_SOCKET_LIFETIME_MODE`; `scripts/qualify-mingw.sh` automates them.
Synchronized mode requires DEL-before-`closesocket()` and skips native-reuse
identity tests. Build nginx with an explicit
`WEPOLL_EX_NGINX_LIFETIME_MODE`, run `nginx -t`, then use
`scripts/nginx-endurance.py` before benchmarking.

Never run `rg`, `find`, or similar searches on `/` or a Windows drive
mountpoint; scope searches to this repository or an explicitly named build.

## Coding and Testing Conventions

Use strict C11, four-space indentation, the surrounding brace style, project
headers before system headers, `snake_case` names, `_t` project types, `ep_*`
internals, and uppercase macros. There is no configured formatter or linter.
Add behavior-focused plain-C tests and run named executables plus CTest.
Linux behavioral changes need API regressions, including native/fallback
`epoll_pwait2`, cancellation, close/wait, and metadata races. Windows CTest
includes UDP/IPv6, multi-epfd, pending-MOD, provider, backpressure, fail-at-N,
bounded close/reaper, randomized stress, and IOCP regressions. Run
`bench_windows --production` only after correctness passes. Document every
behavior changes in README/design/release or nginx notes. Record compiler, OS,
lifetime mode, and flags for Windows runs.

## Packaging, Commits, and Network

Install through the generated CMake package and distribute `LICENSE` and
`NOTICE`. Until the preview ABI is frozen, keep exact package/ELF SONAME
compatibility and update the shared-export allowlist intentionally for every
public API change. Use imperative commit subjects and detailed bodies covering
behavior, tests, and documentation. PRs describe behavior, toolchain,
limitations, and linked issues.
Set a real Git author identity before release history is published. Upstream
is `ssh://git@192.168.50.180:2222/congjc/wepoll-ex.git`; do not push or tag a
shared release without explicit direction. For network access, especially
GitHub, use `ALL_PROXY=socks5://127.0.0.1:2080` and never commit credentials or
generated build state.
