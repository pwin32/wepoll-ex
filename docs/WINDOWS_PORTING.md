# Porting epoll-based Projects to Windows

## Scope

wepoll-ex can now remove several mechanical obstacles from a Windows port of
an epoll-based event loop. It can provide the familiar header name, preserve a
safe DEL-before-close order, combine explicit readiness ownership with
`EPOLLONESHOT`, clone a virtual epoll instance, and retain useful native error
details. These are opt-in porting bridges, not a claim that a Windows virtual
epfd is a native Linux file descriptor.

The practical boundary is:

- socket registration, control, waiting, data payloads, ET/ONESHOT state, and
  same-process epfd sharing can usually keep an epoll-shaped design;
- descriptor inheritance, `fcntl`/`ioctl`, nested epoll, eventfd/timerfd,
  signals, Linux file AIO, io_uring, and general IOCP completion dispatch still
  require platform-specific integration; and
- an edge-driven loop must preserve its drain-to-`WSAEWOULDBLOCK` and socket
  lifetime rules. Renaming functions alone cannot supply those rules.

The public capability query lets a port reject an unsuitable backend instead
of inferring semantics from the operating system name.

## Opt-in `<sys/epoll.h>` target

Windows builds enable `WEPOLL_EX_BUILD_EPOLL_COMPAT` by default and export the
`wepoll_ex::epoll_compat` CMake target:

```cmake
find_package(wepoll_ex 0.1.0 EXACT CONFIG REQUIRED COMPONENTS epoll_compat)
target_link_libraries(my_event_loop PRIVATE wepoll_ex::epoll_compat)
```

The target adds an isolated include root containing `<sys/epoll.h>` and links
the selected shared or static library. The header includes `wepoll_ex.h`, so
ordinary epoll constants, structures, and entry points are available without
editing every include directive. The compatibility header is installed below
`include/wepoll-ex-compat`, not the package's ordinary include root. Linking
`wepoll_ex::wepoll_ex` therefore does not globally shadow another
`sys/epoll.h`.

This target provides source declarations only. It does not turn the virtual
Windows epfd into a CRT descriptor or implement Linux facilities that happen
to compose with epoll.

## DEL-before-close helper

`wepoll_ex_close_socket(epfd, socket)` performs `EPOLL_CTL_DEL` against one
epoll instance and calls `closesocket()` only after the registration is no
longer publicly reachable. An absent registration is accepted after the
target has been validated. Any other DEL failure leaves the socket open, so
the caller can report or recover from the ordering failure.

```c
if (wepoll_ex_close_socket(epfd, socket_fd) != 0) {
    /* The socket is still open unless closesocket itself failed. */
    handle_close_error(socket_fd);
}
```

The helper does not retain a duplicate socket and therefore does not delay
peer FIN. It also does not know which other epoll instances contain the same
socket. A multi-epfd owner must DEL from every other instance before invoking
the helper on the final instance. The helper is especially useful with the
synchronized lifetime policy and explicit-rearm ports, where a fully disarmed
registration may have no native AFD request available to observe an arbitrary
direct `closesocket()`.

On Linux the same API performs DEL followed by `close()`. It is an ordering
convenience rather than a replacement for application-level ownership.

## Local shutdown readiness helper

Winsock does not turn local `shutdown(SD_RECEIVE)` into AFD, `WSAPoll`, or
`select` readability. An epoll-shaped loop can therefore block forever waiting
for the Linux local-EOF event unless it owns the shutdown call.
`wepoll_ex_shutdown_socket(epfd, socket, how)` supplies that ownership point:

```c
if (wepoll_ex_shutdown_socket(epfd, socket_fd, SD_RECEIVE) != 0) {
    handle_shutdown_or_publication_error(socket_fd);
}
```

For a recognized TCP socket currently registered in the selected Windows
port, a successful receive shutdown publishes the requested
`EPOLLIN`/`EPOLLRDNORM` aliases plus `EPOLLRDHUP`. Once both local directions
are shut down, it also publishes unrequested `EPOLLHUP`. `SD_SEND` alone does
not manufacture a new Linux readiness class. Existing ordinary readiness is
merged into the same snapshot. LT remains persistent; ET reports only newly
observed bits; ONESHOT remains disabled until normal MOD/rearm; and an
explicit-rearm port disarms/redelivers the applicable readiness classes under
its existing acknowledgement contract. MOD and class rearm perform a fresh
synthetic scan and wake an already blocked waiter.

The helper validates the epfd and target before the first native effect. If an
idle/queued registration needs a ready node immediately, it reserves that node
before calling Winsock. Cancellation or IOCP publication can still fail after
native shutdown succeeds. The TCP state remains recorded; when the port is
still usable, calling the helper again retries publication without repeating
the already-recorded native direction. A fatal IOCP-post failure still makes
the port unusable and should follow normal teardown/error handling.

This is readiness emulation, not data-call emulation. After `SD_RECEIVE`,
Winsock `recv()` continues to fail with `WSAESHUTDOWN`; it does not return the
zero-byte EOF that Linux returns. The state belongs to one underlying epoll
port (and therefore all `wepoll_ex_dup()` aliases of it). Direct `shutdown()`
calls and independent epoll ports containing the same socket remain outside
observation. The state also belongs to the current registration: DEL discards
it, and a later ADD does not infer an already-completed local shutdown. An
unregistered or non-TCP Windows socket, and every POSIX call, uses native
`shutdown()` without synthetic state.

## Explicit rearm with `EPOLLONESHOT`

A Windows port created with `WEPOLL_EX_CREATE_EXPLICIT_REARM` gives socket
`EPOLLET` registrations direction-aware ownership. Delivered READ, WRITE, and
TERMINAL classes remain disarmed until the application acknowledges that its
corresponding operation drained to `WSAEWOULDBLOCK`:

```c
int classes = drained_read ? WEPOLL_EX_REARM_READ : 0;
classes |= drained_write ? WEPOLL_EX_REARM_WRITE : 0;
if (classes != 0 && epoll_rearm_classes(epfd, socket_fd, classes) != 0) {
    handle_rearm_error(socket_fd);
}
```

`EPOLLONESHOT` can be combined with this contract. Acknowledging only part of
the classes delivered by a fired one-shot records that ownership transfer but
keeps the registration disabled and submits no native poll. Clearing the final
delivered class starts the next one-shot generation. `epoll_rearm()`
acknowledges all delivered classes, while a successful MOD resets all class
disarms and starts a fresh generation.

The ready record must first be consumed; rearming while it is still queued
returns `EBUSY`. `EPOLLEXCLUSIVE` remains incompatible with explicit-rearm ET,
and pipes/waitable HANDLEs retain their separate observed-edge adapters. POSIX
reports the creation mode and class-rearm operations unsupported.

This facility makes Mio-style `WouldBlock` ownership and an nginx-style
handler-completion experiment expressible. It does not automatically discover
that a handler drained a socket. Posted handlers, TLS layers, proxy chains, and
third-party modules must place acknowledgement at their real operation
boundary.

## Virtual epfd aliases

`wepoll_ex_dup(epfd)` creates another virtual integer referring to the same
Windows `ep_port_t`. All aliases share registrations, ready state, wait
serialization, wake state, errors, and statistics. Closing a non-final alias
removes only that integer and does not wake waits through the other aliases.
Closing the final alias begins normal port shutdown, wakes waiters, and owns
bounded/deferred destruction.

This supplies the lifetime contract needed by a same-process selector clone,
but it is intentionally not named or implemented as native `dup()`:

- the alias is not a HANDLE or CRT fd;
- it cannot be inherited or passed to another process;
- `DuplicateHandle`, `fcntl`, and `ioctl` do not operate on it; and
- epoll instances still cannot be registered in or nested inside another
  Windows epoll instance.

On Linux, use native `dup()` on the returned epoll descriptor;
`wepoll_ex_dup()` reports `EOPNOTSUPP` because the wrapper does not attempt to
merge extension metadata across independently duplicated native descriptors.

## Native error details

Public calls continue to report portable `errno`. After a failed wepoll-ex
operation, `wepoll_ex_get_last_error_info()` copies a per-thread, versioned
record containing:

- `portable_error`, the public errno value;
- an exact native Win32, Winsock, or NTSTATUS domain/code when the failing path
  retained that source; and
- a canonical Winsock equivalent when one is meaningful for the normalized
  error.

`WEPOLL_EX_ERROR_NATIVE_EXACT` distinguishes an original native code from a
portable error that was normalized later. `WEPOLL_EX_ERROR_WINSOCK_EQUIVALENT`
means `winsock_error` is a documented translation; it does not prove that
Winsock produced the failure. The channel is defined after a library call
returns `-1`, is local to the calling thread, and is not overwritten by a
successful getter. Callers should size-check the versioned prefix and continue
to use the primary return value and `errno` for control flow.

POSIX returns the current `errno` with empty native fields. The channel does
not capture unrelated failures produced by application `recv`, `send`, or
other system calls that bypass wepoll-ex.

## Capability checks

A port that depends on these bridges should query
`wepoll_ex_get_capabilities()` and require the applicable bits:

- `WEPOLL_EX_CAP_CLOSE_SOCKET_HELPER`;
- `WEPOLL_EX_CAP_EXPLICIT_REARM_ONESHOT`;
- `WEPOLL_EX_CAP_VIRTUAL_EPOLL_DUP`;
- `WEPOLL_EX_CAP_SHUTDOWN_SOCKET_HELPER`; and
- `WEPOLL_EX_CAP_ERROR_INFO`.

The older edge-delivery, explicit-rearm, wake, tagged-wake, signal-mask,
descriptor-kind, and exclusive-scope bits remain necessary when those
semantics affect the event loop.

## Remaining drop-in gaps

The opt-in bridges reduce source churn but do not remove these material porting
boundaries:

1. Windows `EPOLLET` is either an observed-level filter or the opt-in explicit
   drain/ack contract, not Linux's in-kernel ready-list implementation.
2. Direct socket close, shutdown, or handle reuse outside the selected
   lifetime contract can still bypass library observation. In particular,
   local receive shutdown is visible only when every relevant caller uses
   `wepoll_ex_shutdown_socket()` for each independent port that needs it, and
   DEL/re-ADD does not carry the recorded state into the new registration.
3. `eventfd`, `timerfd`, signalfd/signal masks, Linux file AIO, io_uring, and
   general IOCP operation completions need separate Windows facilities.
4. `EPOLLEXCLUSIVE` arbitration is process- and loaded-image-local, not a
   cross-process nginx worker guarantee.
5. Virtual epfds are not native descriptors: there is no inheritance,
   descriptor passing, nesting, busy-poll ioctl, or transparent use by code
   that calls CRT/POSIX fd APIs on the epfd.
6. AFD is undocumented. Current qualification is strongest on x86-64
   MinGW-w64 and Windows 10; additional Windows versions, architectures,
   compilers, and Winsock providers require their own evidence.

For the exact nginx, libuv, Mio, and Boost.Asio source patterns, see
`UPSTREAM_EVENT_LOOP_AUDIT.md`. The nginx handler-level contract is detailed in
`NGINX_NATIVE_EPOLL_PORT.md`.
