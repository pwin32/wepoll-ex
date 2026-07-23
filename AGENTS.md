# Repository Guidelines

## Project Status and Layout

`wepoll-ex` 0.1.0 is an experimental C11/CMake project. The Windows backend
uses IOCP and the undocumented AFD poll interface (Windows 8+ assumptions);
the POSIX build wraps native `epoll` for API development. Public headers are
in `include/`, implementations are in `src/`, tests are in `tests/`, POSIX
benchmarks are in `bench/`, the opt-in nginx adapter is in `nginx/`, and
design/release notes are in `docs/`. Keep `nginx-1.31.3.tar.gz` as reference
material only. The `libuv-v1.52.1.tar.gz`, `asio-develop.zip`, and
`mio-master.zip` archives are also reference-only; never stage any of them.

## Build, Test, and Development Commands

Use disposable build directories. A strict POSIX release check is:

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

For Windows, use `/path/to/msys64/usr/bin/bash.exe`, put `/mingw64/bin:/usr/bin`
first in `PATH`, configure with `-G "MinGW Makefiles"`, and run CTest. Validate
combined, static-only, shared-only, and (for nginx)
`WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME` builds. That contract skips
native close/reuse identity modes. Build the nginx adapter with its configured
tree, then run `nginx -t` and a bounded loopback smoke before benchmarking.

Never run `rg`, `find`, or similar searches on `/` or a Windows drive
mountpoint; scope searches to this repository or an explicitly named build.

## Coding and Testing Conventions

Use strict C11, four-space indentation, the surrounding brace style, project
headers before system headers, `snake_case` names, `_t` project types, `ep_*`
internals, and uppercase macros. There is no configured formatter or linter.
Add behavior-focused plain-C tests and run both named executables plus CTest.
POSIX behavioral changes need API regressions, including close/wait and
metadata races where relevant. Windows CTest includes UDP/IPv6, multi-epfd,
pending-MOD, provider, backpressure, control-race, and bounded IOCP regressions.
Record compiler, Windows version, and relevant build flags for Windows runs.

## Packaging, Commits, and Network

Install consumers through the generated CMake package and keep `LICENSE` and
`NOTICE` in distributed artifacts. Use short imperative commit subjects and
detailed bodies covering behavior, tests, and documentation. PRs should
describe behavior, tests/toolchain, limitations, and linked issues.
Set a real Git author identity before release history is published. Upstream
is `ssh://git@192.168.50.180:2222/congjc/wepoll-ex.git`; do not push or tag a
shared release without explicit direction. For network access, especially
GitHub, use `ALL_PROXY=socks5://127.0.0.1:2080` and never commit credentials or
generated build state.
