# Repository Guidelines

## Project Status and Layout

`wepoll-ex` is an experimental C11/CMake prototype, not a production-ready
Windows event library. `src/` contains the Windows IOCP/AFD path and the POSIX
development wrapper; public headers are in `include/`. API and queue tests are
in `tests/`, the POSIX-only microbenchmark is in `bench/`, and the unvalidated
nginx adapter is in `nginx/`. Design notes are in `docs/`. Keep
`nginx-1.31.3.tar.gz` as reference material only; never stage it.

## Build, Test, and Development Commands

Use a dedicated disposable build directory when investigating failures. The
intended POSIX commands (using `build/`) are:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DWEPOLL_EX_BUILD_BENCH=ON
cmake --build build --target wepoll_ex_shared wepoll_ex_static \
    test_wepoll_ex test_wepoll_ex_pool bench_latency
ctest --test-dir build --output-on-failure
./build/tests/test_wepoll_ex
./build/tests/test_wepoll_ex_pool
./build/bench/bench_latency 50000
```

For MinGW, invoke the requested shell and put its toolchain first:

```sh
/path/to/msys64/usr/bin/bash.exe -lc \
  'export PATH=/mingw64/bin:/usr/bin:$PATH; cd /e/personal/wepoll-ex; \
   cmake -S . -B build-mingw -G "MinGW Makefiles" -DWEPOLL_EX_BUILD_TESTS=OFF; \
   cmake --build build-mingw --parallel'
```

The current baseline has known configure and pool-test blockers; see
`docs/DESIGN.md` before treating a failed command as a regression. Windows
tests are not yet implemented and are excluded from the default build.

NEVER find/rg on root dir or windowsn drive mountpoint.

## Coding and Testing Conventions

Use strict C11, four-space indentation, and the surrounding brace style. Put
the project header before system headers. Use `snake_case` names, `_t` project
types, `ep_*` internals, and uppercase macros. There is no configured
formatter or linter. Add behavior-focused cases to the existing plain-C tests
and run both named executables plus CTest; record platform and compiler for
Windows experiments.

## Commits, Pull Requests, and Network

The repository currently has one local baseline commit, `598db2e` (`Initial
repository checkpoint`), on `master`. Remote `upstream` is
`ssh://git@192.168.50.180:2222/congjc/wepoll-ex.git`; do not push without an
explicit request. Use short, imperative commit subjects. PRs should state the
behavioral change, tests and toolchain, known limitations, and linked issues.
When network access is necessary (especially GitHub), use
`ALL_PROXY=socks5://127.0.0.1:2080` and do not commit credentials or generated
build state.
