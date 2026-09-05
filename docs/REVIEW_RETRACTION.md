# Retraction: 2026-09-05 architecture review "critical bugs"

An AI-assisted architecture review on 2026-09-05 reported three P0
"production blocker" concurrency bugs in `src/wepoll_ex_port.c`, and
several P0/P1 performance findings. Fixes for all three bugs were
written, failed CI, and were reverted in full. The tree is unchanged.

This note exists so the claims are not re-implemented. The review
documents themselves (`IMPLEMENTATION_NOTES.md`, `PHASE1_STATUS.md`,
`WORK_SUMMARY.md`) were removed because their premise is false
throughout.

## The three "critical bugs" were false positives

**1. "Use-after-free in `ep_sock_drop_closed_locked`"** — claimed a TOCTOU
race between the `poll_status` load and `ep_sock_free_locked`.
Not reachable. All 12 call sites of `ep_sock_drop_closed_locked` hold
`fd_table_lock`, and `ep_sock_handle_completion` acquires that same lock
as its first action before touching any socket field. The two paths are
already mutually exclusive; no completion can run between the load and
the free. The attempted CAS fix broke a state invariant, and the
accompanying change to the completion handler — widening the free
condition to accept `EP_POLL_IDLE` — would have freed live sockets
mid-resubmit.

**2. "Waitable notification lost when the IOCP post fails"** — claimed
`waitable_notification_owned` must be cleared on post failure.
Backwards. The retained flag *is* the recovery mechanism: the block near
the top of the waitable branch in `ep_sock_submit_locked` replays the
post on the next submission. Clearing it discards the replay, which is
what would make the loss permanent. See the "replayed before probing
when interest is restored" contract in README.md.

**3. "Completion flag race on `completion_posted`"** — claimed concurrent
posts can be clobbered by a store of 0, and proposed a counter.
Premise false. The design guarantees one outstanding post per socket:
the wait registration is one-shot (`WT_EXECUTEONLYONCE`) and the
completion handler disarms via `ep_waitable_unregister_locked` before
clearing the flag. `completion_posted` is a binary in-flight flag, and
`ep_port_worklists_valid_locked` asserts it is 0. The counter change
also left an unbalanced decrement on the disarm-failure path, which
could underflow the unsigned field to `0xFFFFFFFF`.

## How the CI failure was misread

The failing test reported `errno=5`. That is `EIO`, which
`tests/test_windows_state.c` sets itself when a state assertion fails —
not Win32 `ERROR_ACCESS_DENIED`. Because of that misreading, a
deterministic self-inflicted invariant violation (0.01 s, all three
Windows configs, passing on master) was initially dismissed as a flaky
pre-existing test.

## Performance findings: measured, mostly false

Baseline from CI run 33958449218 (all 8 jobs green, 201/201 tests on each
of mingw-combined / mingw-strict / mingw-synchronized):

| benchmark | p50 | p99 | ops/sec |
|---|---|---|---|
| registration_add (1000 sockets) | 3900 ns | 9100 ns | 215045 |
| registration_del | 6400 ns | 7000 ns | 155080 |
| ready_batch b=1 | 9600 ns | 17500 ns | 100442 |
| ready_batch b=512 | 6222200 ns | 9453100 ns | 80365 |
| oneshot_rearm | 1300 ns | 1500 ns | 753864 |
| control_churn | 8400 ns | 11300 ns | 351062 |

Linux: round-trip p50 1012 ns / p99 1522 ns, 956880 ops/sec. Wait
scaling at 1024 registrations: empty 428 ns/wait, ready 1204 ns/event.
Windows MT contention (256 active / 32 churn / 3 s): `ctl_add` p50
12500 ns, `ctl_mod` p50 106300 ns, `ctl_del` p50 2100 ns.

The decisive structural fact: **every** `ep_afd_pool_take`/`give` call and
**all three** `ep_ready_push` calls are already inside `fd_table_lock`.
The pools are per-port, not global, and the drain takes the same lock.

- *"Pool mutex bottleneck, 5M -> 500K ops/sec, 4-5x available"* — false.
  The pool mutex is never contended; callers already hold
  `fd_table_lock`, so threads serialize on the outer lock and never
  reach the pool concurrently. The "5M ops/sec" baseline was never
  measured; actual is 215K add / 754K rearm.
- *"Pool lock / atomics false sharing"* — false. `in_use`/`peak` are
  written only inside the pool mutex.
- *"Make `fd_table` `_Atomic` for lock-free reads"* — false, and unsafe.
  The table is open-addressed with compaction on remove; a lock-free
  reader could observe a slot mid-relocation and miss a live entry.
- *"`ep_ready_node_t` producer/consumer false sharing"* — false.
  Producers and consumer are mutually exclusive under `fd_table_lock`.
  The struct is 56 bytes and already fits one line.
- *"Peak-tracking atomic race"* — false. The read-modify-write is inside
  the mutex.
- *"`ep_sock_t` spans 7 cache lines, 15-20% loss"* — real observation,
  wrong magnitude. A few extra L2 hits (~20 ns) against a measured
  9600 ns `ready_batch` p50 is ~0.2%; the syscall path dominates.
  Low-priority cleanup at most.
- *"Pool header overhead"* / *"one-entry-at-a-time growth, never
  shrinks"* — real but small. The `max_align_t` member is redundant
  (`_Alignas` on `data[]` would do). Growth matters only for
  burst-then-idle workloads.

The one scaling concern the review missed: `fd_table_lock` is held for
the whole drain batch rather than per event. That, not the pool mutex,
is the real serialization point. `bench_mt_contention` is the harness
for it — but read its header first: per-operation figures are
position-dependent in the churn sequence, and an unmodified binary has
been seen varying >40% at `ctl_mod` p50 run to run. Establish an A/A
floor and compare same-operation-same-slot, or the number means nothing.

## What was actually kept

One genuine fix, already on master (84e8ee3): relaxing the
`early_timeout_calls` assertion in `tests/test_windows_iocp_batch.c`
from `>= 2` to `>= 1`. The test verifies that a finite wait respects its
deadline; the number of internal retry calls is timing-dependent and a
fast runner can satisfy the 25 ms wait in one call.

## Remaining findings: second pass

The rest of the review (sections 2.5, 3.1, 3.2, 4.1-4.7, 6.1-6.2) was then
checked the same way. 56 findings total; one real, and it is unreachable.

**The one real defect — generation wraparound guard (was filed MEDIUM, is
P3).** Six of the seven `sock->generation = ++port->next_sock_generation`
sites follow the assignment with a `== 0` guard that skips generation zero.
`ep_port_unregister` did not. Fixed by applying the same three lines.

The review's stated impact was wrong: it claimed a stale ready node could
falsely match across the wrap. It cannot — the socket is being retired,
`delete_pending` is set, and the delivery check requires `!delete_pending`.
The actual (and purely theoretical) issue is that leaving the counter at
zero would let a later `ep_sock_alloc_locked` mint generation 0 while a
node carrying 0 still exists. Reaching it requires 2^64 registrations on
one port. This was fixed for consistency across the seven sites, not
because it was reachable.

**False positives:**

- *2.5 "TOCTOU race in `ep_afd_cancel`"* — no race. The file header of
  `wepoll_ex_afd.c` states the contract: "Callers enter from port code
  while holding fd_table_lock." Both reads happen under that lock. The
  proposed fix is a reordering with identical semantics.
- *3.1 "unbounded `DuplicateHandle` loop"* — cannot spin. Every colliding
  duplicate is *retained* in the `collisions` array rather than closed and
  retried, so each iteration consumes a distinct handle value from a finite
  table; it terminates by exhaustion, returning `ENOMEM` from the growth
  path. The retention comment in the loop says exactly this. An iteration
  cap would add nothing.
- *3.2 "audit seq_cst atomics under locks"* — self-refuting. It proposes
  relaxing an `atomic_load` that its own section 6.2 then shows already
  uses `memory_order_relaxed`. Claimed 1-2% is far below the measurement
  floor (see below).
- *4.2 "exclusive claim list race"* — the global SRW lock serializes all
  mutations. The finding text itself concedes "the global lock is held, so
  concurrent modification shouldn't occur," then files it as MEDIUM.
- *4.6.2 "batch array bounds not validated"* — not fixable and not a
  defect. `epoll_ctl_batch(epfd, ops, fds, events, count)` receives bare
  pointers with no size information; validating `count` against them is
  impossible, exactly as for `memcpy`. Caller contract, already documented.
- *4.6.1 / 4.6.3 / 4.6.4* — `epfd_bucket` is only reachable after
  `epfd_find_locked` rejects `fd <= 0`; `epfd_put`'s `refs > 0` test is
  defensive depth, not a swallowed bug; `epfd_allocate_id_locked` breaks on
  the first free slot and returns -1/EMFILE when full. Optional assertion
  hygiene at most.
- *4.7 "other medium/low issues"* — six items with no file, line, or
  reproduction. Not actionable as written.
- *4.5 / 6.1 MPSC queue "verified correct"* — independently confirmed. The
  acquire/release pairing and stub protocol are right. This holds up.

## Windows performance cannot be measured on this CI

Two runs of byte-identical code (33958449218 and 33961500052, both green,
201/201 on all three Windows configs):

| metric (p50) | run A | run B | delta |
|---|---|---|---|
| registration_add | 3900 ns | 5100 ns | +30.8% |
| registration_del | 6400 ns | 10500 ns | +64.1% |
| ready_batch b=1 | 9600 ns | 12400 ns | +29.2% |
| ready_batch b=512 | 6222200 ns | 7581000 ns | +21.8% |
| oneshot_rearm | 1300 ns | 1600 ns | +23.1% |
| control_churn | 8400 ns | 10400 ns | +23.8% |
| ctl_add | 12500 ns | 15400 ns | +23.2% |
| ctl_mod | 106300 ns | 115100 ns | +8.3% |
| ctl_del | 2100 ns | 2600 ns | +23.8% |

Identical code, +21% to +64% swing on shared GitHub Windows runners. Linux
is stable by comparison (p50 1012 -> 1005 ns, -0.7%; rate +2.2%).

No Windows performance claim below roughly 65% is measurable here. The
review's "15-20% cache layout gain" sits under the floor. `bench_mt_contention`
warns about this in its own header (">40% at ctl_mod p50"); the measured
reality is worse. Windows perf work needs a dedicated quiet machine, and any
claim needs an A/A floor established first.

## Final tally

56 findings. 1 real (P3, unreachable, fixed for consistency). 0 that
required a behavioural code change. 3 labelled CRITICAL that were actively
harmful to implement.

## Root cause

The findings were produced by reading functions in isolation, without
checking lock discipline at the call sites or the documented ownership
contracts. Every `_locked` suffix was a signal that was not followed up.
In 2.5 the governing contract was thirty lines above the cited function;
in 3.1 and 4.2 the code comments stated the answer outright.

`ARCHITECTURE_REVIEW.md` and `REVIEW_SUMMARY.md` are superseded by this
document and are not a backlog. Do not mine them for work items.
