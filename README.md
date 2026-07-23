# wepoll-ex

`wepoll-ex` is an experimental C11/CMake prototype for an epoll-shaped API on
Windows. Its Windows path uses IOCP and the undocumented AFD poll interface;
its POSIX path wraps the host `epoll` implementation for development and API
experiments. The project is not production-ready, and no Linux compatibility,
Windows performance, or nginx integration guarantee is made.

## Scope and current status

The repository is useful for exploring the API boundary and for reproducing
bugs in the two platform paths. It is not a drop-in replacement for Linux
`epoll`, and no throughput, latency, connection-count, or compatibility target
is promised. Treat all extension semantics as provisional until they have a
platform-specific regression test.

The archive `nginx-1.31.3.tar.gz` is reference material only. It is deliberately
not part of the tracked source and is not evidence that the adapter builds.
See [`docs/NGINX_INTEGRATION.md`](docs/NGINX_INTEGRATION.md) for the current
validation checklist.

## Architecture at a glance

### Windows implementation

`src/wepoll_ex_api.c` exposes the public integer `epfd` API and maps each id to
an internal `ep_port_t`. `src/wepoll_ex_port.c` owns an IOCP handle, an AFD
handle, a growable socket table, and a ready queue. `epoll_ctl` submits an
`AFD_POLL`; completions are translated by `src/wepoll_ex_afd.c`, queued as
immutable snapshots, and consumed by `epoll_wait`. The port retains socket
storage until cancellation completions are drained. AFD is undocumented and
the build currently targets Windows 8 or later (`_WIN32_WINNT=0x0602`). The
Windows path is validated only with the MinGW/MSYS2 checks below.

### POSIX development path

`src/wepoll_ex_posix.c` leaves the basic `epoll_create*`, `epoll_ctl`, and
`epoll_wait` symbols to the host libc. It keeps a small per-epfd table for
`user_ctx`, flags, counts, and extension helpers, then decorates native events
for `epoll_wait_ex`. The shared pool/queue code is also compiled here for its
unit tests; it does not make the POSIX wrapper an implementation of the
Windows engine.

### Public extensions

The header [`include/wepoll_ex.h`](include/wepoll_ex.h) declares
`epoll_create_ex`, `epoll_ctl_ctx`, `epoll_wait_ex`, `epoll_pwait2_ex`,
`epoll_ctl_batch`, `epoll_drain`, `epoll_rearm`, `epoll_fd_count`, version
helpers, and `wepoll_close`. `epoll_ctl_batch` applies operations in order and
best-effort rolls back ADDs; it is not transactional.

On Windows, registrations are socket-only. `EPOLLONESHOT` is supported;
`EPOLLET` and `EPOLLEXCLUSIVE` are rejected with `EOPNOTSUPP` because the AFD
backend does not provide their required semantics. `epoll_pwait*` accepts a
null signal mask only. Call `wepoll_close()` for the virtual epoll descriptor.

## Repository layout

```
include/   public headers
src/       Windows engine and POSIX wrapper
tests/     POSIX, Windows API, pool, and package-consumer tests
bench/     POSIX-only bench_latency source
nginx/     opt-in nginx 1.31.3 adapter and configure hook
docs/      design and integration status
```

## Build and test

The CMake target names and executable paths are:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DWEPOLL_EX_BUILD_BENCH=ON
cmake --build build --target wepoll_ex_shared wepoll_ex_static \
    test_wepoll_ex test_wepoll_ex_pool bench_latency
ctest --test-dir build --output-on-failure
./build/tests/test_wepoll_ex
./build/tests/test_wepoll_ex_pool
./build/bench/bench_latency 50000
```

For MinGW, use the toolchain shell explicitly:

```sh
/path/to/msys64/usr/bin/bash.exe -lc \
  'export PATH=/mingw64/bin:/usr/bin:$PATH; cd /e/personal/wepoll-ex; \
   cmake -S . -B build-mingw -G "MinGW Makefiles" \
     -DCMAKE_BUILD_TYPE=Debug -DWEPOLL_EX_BUILD_BENCH=OFF \
     -DCMAKE_C_FLAGS="-Wall -Wextra -Wpedantic -Werror"; \
   cmake --build build-mingw --parallel; \
   ctest --test-dir build-mingw --output-on-failure'
```

The POSIX baseline passes the API, pool, and package-consumer tests. The MinGW
baseline passes the Windows API and package-consumer tests with shared and
static libraries, including a shared-only configuration. MinGW final binaries
select the static winpthreads archive and CTest rejects an accidental
`libwinpthread-1.dll` dependency. Record the exact command, compiler, and OS
for new results.

## Install and consume

Version 0.1.0 is an experimental preview and does not promise a stable ABI.
Install it to an isolated prefix while evaluating it:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
    -DWEPOLL_EX_BUILD_TESTS=OFF
cmake --build build-release --parallel
cmake --install build-release --prefix /path/to/wepoll-ex-prefix
```

CMake consumers can use the installed package and its default target (shared
when both library forms are installed):

```cmake
find_package(wepoll_ex 0.1.0 CONFIG REQUIRED)
target_link_libraries(my_server PRIVATE wepoll_ex::wepoll_ex)
```

Use `wepoll_ex::wepoll_ex_static` or `wepoll_ex::wepoll_ex_shared` when the
linkage must be explicit. Set `CMAKE_PREFIX_PATH` to the install prefix, or set
`wepoll_ex_DIR` to its `<libdir>/cmake/wepoll_ex` directory.

Release-specific validation and limitations are recorded in
[`docs/RELEASE_NOTES.md`](docs/RELEASE_NOTES.md).

## Credits and license

New wepoll-ex contributions are distributed under the ISC terms in `LICENSE`.
The Windows AFD/IOCP implementation contains work derived from wepoll by Bert
Belder; the upstream BSD-2-Clause terms are preserved in `NOTICE`. Installed
packages include both files under `share/licenses/wepoll-ex`.
