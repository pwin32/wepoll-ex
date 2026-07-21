# Repository Guidelines

## Project Structure

This is a C11/CMake project. `src/` contains the implementation: Windows IOCP/AFD code, the shared pool and queue, and the POSIX development shim. Public headers live in `include/`. `tests/` contains the CTest-backed API and MPSC/pool tests; `bench/` contains the optional latency benchmark. The nginx adapter is in `nginx/`, with design and integration notes in `docs/`. Keep `nginx-1.31.3.tar.gz` as reference material only—do not stage or commit it.

## Build, Test, and Development Commands

For MinGW work, use `/path/to/msys64/usr/bin/bash.exe` from the repository root with the toolchain on `PATH`. A POSIX development build is:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

For MinGW, add `-G "MinGW Makefiles"` to the configure command. Enable and run the benchmark with `-DWEPOLL_EX_BUILD_BENCH=ON`, then `cmake --build build --target bench_latency` and run the generated `bench_latency` executable. Current tests use POSIX headers and are excluded from the default Windows build; still compile the Windows targets when changing Windows-specific code.

## Coding Style & Naming

Use strict C11, four-space indentation, and the existing brace style. Put the project header before standard-library headers. Use `snake_case` for functions, variables, files, and `_t` for project types; use `ep_*` for internals, `epoll_*`/`wepoll_*` for public APIs, and uppercase names for macros. No formatter or linter is configured, so keep formatting consistent with nearby code.

## Testing Guidelines

Tests are plain C executables registered with CTest, not a third-party framework. Add behavior-focused cases to `tests/test_api.c` or `tests/test_pool.c` (using the existing `TEST`/`PASS`/`FAIL` pattern), and run the full CTest command above. There is currently no coverage threshold; include regression coverage for every changed behavior.

## Commits & Pull Requests

The repository has no commits, remote, or contribution template yet. Use a short, imperative subject such as `Fix edge-triggered wakeup handling`. PRs should explain the behavior and rationale, list commands and platform/toolchain results, link related issues, and include before/after benchmark data for performance changes. Screenshots are generally not applicable to this C library. Keep build directories, `.codex/` state, and reference archives out of commits.
