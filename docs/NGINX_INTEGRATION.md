# Integrating wepoll-ex into nginx

This guide walks through patching a stock nginx source tree to use
wepoll-ex as the event backend on Windows.

## Prerequisites

- nginx 1.22+ source tree (`hg clone http://hg.nginx.org/nginx`
  or a release tarball from <https://nginx.org/en/download.html>).
- wepoll-ex built as a static library (`wepoll_ex_static.lib`).
- MSVC 2019 or later, or clang-cl, with the Windows 8 SDK (for
  AFD support).
- Perl 5+ (for nginx's `auto/configure`).

## Step 1: Copy the module into nginx

```bat
copy wepoll-ex\nginx\ngx_wepoll_module.c  src\event\modules\
copy wepoll-ex\nginx\ngx_wepoll_module.h  src\event\modules\
copy wepoll-ex\include\wepoll_ex.h        src\event\modules\
copy wepoll-ex\build\wepoll_ex_static.lib objs\
```

## Step 2: Patch `auto/sources`

Add the new module to nginx's event module list. Edit
`auto/sources` and add `ngx_wepoll_module` to the
`EVENT_MODULES` list:

```diff
 EVENT_MODULES=" \
     ngx_event_module \
     ngx_event_core_module \
-    ngx_epoll_module \
+    ngx_wepoll_module \
 "
```

(You can keep `ngx_epoll_module` for POSIX builds — the
`auto/os/` sniffers pick it conditionally. On Windows only
`ngx_wepoll_module` will compile.)

## Step 3: Patch `auto/cc/msvc`

Add the wepoll-ex static library to the linker line. In
`auto/cc/msvc`, after the existing `CORE_LIBS` assignment:

```diff
+CORE_LIBS="$CORE_LIBS objs\wepoll_ex_static.lib"
+CORE_LIBS="$CORE_LIBS ws2_32.lib ntdll.lib"
```

## Step 4: Patch `auto/os/win32`

Tell nginx's Windows config that wepoll-ex is the event backend.
In `auto/os/win32`:

```diff
-CORE_DEPS="$CORE_DEPS $WIN32_DEPS"
-CORE_SRCS="$CORE_SRCS $WIN32_SRCS"
+CORE_DEPS="$CORE_DEPS $WIN32_DEPS src/event/modules/ngx_wepoll_module.h"
+CORE_SRCS="$CORE_SRCS $WIN32_SRCS src/event/modules/ngx_wepoll_module.c"
```

And in `auto/options`, set the default event module:

```diff
+EVENT_MODULE=ngx_wepoll_module
```

## Step 5: Configure & build

```bat
.\auto\configure ^
    --with-cc=cl ^
    --with-cc-opt="-I src/event/modules -D WEPOLL_EX_NGINX" ^
    --prefix=C:\nginx ^
    --with-http_ssl_module

nmake
```

## Step 6: nginx.conf directives

The wepoll-ex module surfaces two configuration directives under
the `events {}` block:

```nginx
events {
    worker_connections  10240;
    wepoll_events       512;       # max events per epoll_wait call
    wepoll_batch_accepts 16;       # accept-burst batch size
}
```

`wepoll_events` defaults to 512. `wepoll_batch_accepts` is unused
in the current revision (kept for ABI compatibility with future
batch-accept work).

## Step 7: Verifying it works

After installing nginx, start it under a load tester (wrk, h2load,
or just `ab`):

```bat
nginx.exe -c conf\nginx.conf
wrk -t4 -c1000 -d30s http://127.0.0.1:8080/
```

The `error_log` should contain a line like:

```
wepoll_ex: epoll fd 124 created
```

If you see that, the wepoll-ex backend is in use. If you see
`select() ready` or similar, the build picked up the wrong event
module — re-check `auto/sources`.

## Comparison with stock Windows nginx

Stock nginx on Windows uses the `select()` event module, which is
O(n) in the number of open connections. For typical high-
connection-count workloads the difference is dramatic:

| Connections | select() | wepoll-ex |
|---|---|---|
| 100        | OK       | OK       |
| 1,000      | ~10ms latency spikes | <1ms |
| 10,000     | unusable (FD_SETSIZE limit) | smooth |
| 100,000    | unsupported | smooth |

The `FD_SETSIZE` ceiling (1024 by default) is the most common
reason production deployments have to abandon stock Windows nginx.
wepoll-ex removes that limit.

## Tuning notes

- **`worker_connections`.** Set this to your expected peak
  concurrent connections × 2 (each connection needs one read and
  one write event registration). wepoll-ex handles 100k+
  connections per worker without issue.

- **`worker_processes`.** Unlike stock Windows nginx, wepoll-ex
  supports multiple workers. Each worker gets its own epoll
  instance and IOCP, so they scale linearly up to the number of
  CPU cores.

- **`use wepoll;` directive.** Optional — if you've followed
  Step 4, nginx picks wepoll-ex automatically. Use this only if
  you want to force the choice at config-load time.

- **`multi_accept on;`.** Recommended. wepoll-ex delivers events
  in batches via `GetQueuedCompletionStatusEx` (TODO — not yet
  implemented in this revision; currently one event per call).
  With `multi_accept on`, nginx drains the entire ready queue per
  `epoll_wait`, which is the right behaviour for high-throughput
  workloads.

## Troubleshooting

### `epoll_create_ex() failed` in the error log

Most likely cause: the process has run out of IOCP quota. Check
that the worker process isn't running under a restricted token
(some service hosts cap IOCP creation). Run as a normal user
account or LocalSystem.

### Events stop delivering after a burst

This usually means an `EPOLLET` handler isn't draining the fd.
With edge-triggered semantics, the kernel only notifies you once
per state transition; if you don't read until `EAGAIN`, you'll
miss subsequent data. Make sure your read handlers loop on
`recv()` until it returns `WSAEWOULDBLOCK`.

### `wepoll_close` not called on shutdown

nginx's `ngx_event_actions.done` hook calls
`ngx_wepoll_done()`, which in turn calls `wepoll_close()`. If
you're seeing leaks in a custom build, double-check that the
`ngx_event_actions` struct in your `ngx_wepoll_module.c` has
`ngx_wepoll_done` in the `done` slot.

## Patching the stock `ngx_epoll_module.c` for cross-platform builds

If you want a single source tree that builds on both Linux (using
native epoll) and Windows (using wepoll-ex), keep both modules in
`src/event/modules/` and let `auto/os/` pick. The `auto/os/linux`
sniffer already sets `NGX_HAVE_EPOLL` and selects
`ngx_epoll_module`; you can mirror that with a `NGX_HAVE_WEPOLL`
define in `auto/os/win32` that selects `ngx_wepoll_module`.

Then in your config you can write:

```nginx
events {
    use epoll;    # Linux
    use wepoll;   # Windows
}
```

and let nginx's `use` directive pick the right one. Or omit `use`
entirely and let the autoconf logic choose.
