# nginx Socket Close-Path Audit

## Purpose

The synchronized Windows socket-lifetime mode deliberately omits provider
identity probes. Every registered socket must therefore leave its wepoll-ex
port before `closesocket()` can make the numeric `SOCKET` reusable. For nginx,
the required path is:

1. a module calls `ngx_close_connection()`;
2. nginx invokes the active event module's `del_conn` action, or its
   close-event fallback;
3. the wepoll adapter performs `EPOLL_CTL_DEL`; and
4. nginx calls `ngx_close_socket()`.

The audit combines source checks with an exit-time runtime invariant. It
qualifies the exact source set that was scanned and exercised; it cannot
pre-certify an addon that has not been supplied.

## Repeatable source audit

Run the scanner against an extracted nginx tree and every addon included in
the build:

```sh
python3 scripts/audit-nginx-close-paths.py \
  --module-root nginx \
  /path/to/nginx-1.31.3
```

The scanner verifies these nginx 1.31.3 core contracts:

- `ngx_close_connection()` reaches `ngx_del_conn` and the close-event fallback
  before `ngx_close_socket()`;
- `ngx_close_listening_sockets()` deletes an active listener event before
  closing the listening socket;
- accepted connections are registered before the listening handler receives
  them; and
- outbound connections register before `connect()` and retire registered
  failures through `ngx_close_connection()`.

It then scans nginx HTTP, mail, stream, and OpenSSL event sources plus every
supplied `--module-root`. A module source is rejected if it calls
`ngx_close_socket()`, `closesocket()`, or `ngx_free_connection()` directly.
Correct modules retire owned connections through `ngx_close_connection()`.

A genuinely pre-registration temporary socket may be exempted by placing
`wepoll-close-audit: allow` on the same or immediately preceding source line.
That annotation is a review assertion, not a runtime safeguard, and should
state why the socket cannot yet belong to an event port.

On August 5, 2026, the exact nginx 1.31.3 reference tree and checked-in wepoll
addon passed across 168 source files. The scan found 29 calls to
`ngx_close_connection()` and zero unreviewed raw socket/free-connection
retirements. nginx core does contain raw closes for pre-registration sockets,
configuration/master-process listeners, syslog sockets, and failed allocation
paths; those are outside module-owned registered connection retirement and are
covered by the explicit core ordering checks instead of a blanket text ban.

Every future third-party addon must be added with another `--module-root`.
Passing the stock-source scan does not qualify an addon that was absent from
the command.

## Runtime close audit

`wepoll_close_audit on` is the adapter default. Immediately before closing the
event port, each worker snapshots:

- active registrations;
- ready, rearm, and one-shot probe queues;
- pending native polls;
- stale-event, identity, and asynchronous-error counts;
- wake and current-TCP probe counts; and
- the selected socket-lifetime policy.

A graceful worker logs an alert if a live registration or queued ownership
record remains. One cancellation-losing AFD request may still be pending after
the final registration has been deleted; `wepoll_close()` owns and drains that
port-level request, so `pending:1` with `active:0` and empty ownership queues is
reported but is not classified as a socket leak.

After `wepoll_close()`, the adapter snapshots global lifecycle state and alerts
if a graceful close left an active quarantined port. Quarantined, reaped,
irrecoverable, close-timeout, and active-quarantine counts remain visible in
the log even when they are zero.

A synchronized `-O2 -Werror` nginx 1.31.3 run on Windows 10.0.19044.1826
retired 20,000 HTTP/1.1 requests per mode with these final invariants:

- level: `active:0`, empty ownership queues, no stale/identity/asynchronous
  errors, and lifecycle counts `0/0/0/0/0`;
- explicit-rearm edge: the same clean close state, 20,064 READ deliveries,
  20,032 READ rearms, 32 initial WRITE deliveries, and zero WRITE rearms; and
- both modes: one reported port-level pending cancellation and zero active
  quarantines after `wepoll_close()`.

This runtime check catches a bypass exercised by the workload or an exit with
live registrations. It does not replace source review: a raw close followed by
numeric socket reuse can corrupt synchronized-lifetime assumptions before
worker exit is reached.

## Qualification boundary

The built-in nginx HTTP/TLS/proxy paths and the checked-in addon now satisfy
the source and runtime close gates used by this project. Unknown binary modules,
source trees not passed to the scanner, direct third-party Winsock calls, and a
socket registered in another independent epfd remain outside that evidence.
