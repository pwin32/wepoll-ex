# Windows performance review — September 15, 2026

This report covers the initial changes in `6749abb`. The
[follow-up review](WINDOWS_INDEX_PERFORMANCE_REVIEW.md) investigates and
optimizes the remaining AFD target-index cost.

The review found two useful changes: bound native cancellation work with AFD
control-handle groups, and reuse the existing TCP readiness sample during edge
delivery. The public API, lifetime policies, and completion-ownership rules
are unchanged.

## Measurements

Measurements compare baseline `e820ab2f07f77c1272139662e023fd02ec117d22`
with this change on Windows 10 Enterprise LTSC 10.0.17763.316, x86-64,
Intel Core i7-4700HQ (4 cores / 8 logical processors), MSYS2 MinGW GCC 16.2.0,
and CMake 4.4.2. Both libraries used best-effort socket lifetime, static linkage,
and Release with `-O2 -Wall -Wextra -Wpedantic -Werror`; CMake appended
`-O3 -DNDEBUG -std=c11`, making `-O3` the effective optimization level.
Timing comparisons ran separately from this review's builds and correctness
tests.

Two production runs per version used 50,000 sockets and 1,000 readiness
iterations. The table gives the range of run-level p50 values, not a pooled
percentile. Setup and final completion draining are outside per-operation
registration samples.

| Operation | Baseline p50 | Candidate p50 |
| --- | ---: | ---: |
| DEL with 1,000 registrations | 14.2–16.5 µs | 4.1–5.9 µs |
| DEL with 10,000 registrations | 295.3–424.5 µs | 4.0–5.0 µs |
| DEL with 50,000 registrations | 2,042.3–2,299.5 µs | 4.1–5.5 µs |
| Entire production benchmark, elapsed | 123.5–135.3 s | 15.2–19.8 s |

Instrumenting `NtCancelIoFileEx` in a separate 10,000-socket baseline probe
accounted for 2,663 ms of 2,715 ms spent deleting registrations (98.1%).
Distributing the same pending requests across 80 ports reduced native
cancellation to 61 ms. This isolated the expensive per-file request search
before changing the implementation; fd-table hashing was not the main cause
of the deletion cliff.

The TCP benchmark uses the same updated `bench_rearm_batch.c` linked against
each version. It keeps both directions ready, acknowledges READ and WRITE
with the batch API, and checks unique, exact readiness delivery. Three runs
per version used 2,000 measured iterations per batch size, with an initial
A/B/B/A ordering and another adjacent A/B pair. The final pair had matching
acknowledgement-only costs at 1 and 16 sockets:

| TCP explicit acknowledgement plus delivery | Baseline p50 | Candidate p50 | Change |
| --- | ---: | ---: | ---: |
| 1 socket | 9.2 µs | 6.8 µs | -26.1% |
| 16 sockets | 137.3 µs | 100.8 µs | -26.6% |
| 64 sockets | 560.1 µs | 408.7 µs | -27.0% |
| 256 sockets | 3,872.5 µs | 3,264.7 µs | -15.7% |

The 256-socket improvement was 13.3–17.5% across all three pairs. These are
explicit-rearm TCP cycles, not nginx throughput or general LT wait results.
The benchmark intentionally acknowledges persistent levels to isolate the
library's control and delivery work; it does not model application I/O cost.

Ordinary UDP waits were noisy. Three additional alternating comparisons used
512 sockets and 500 iterations: the unchanged baseline's 1-ready p50 ranged
from 14.4 to 22.2 µs, and its 512-ready p50 from 8.52 to 11.02 ms. Some earlier
candidate runs were slower. In the final adjacent pair, both versions measured
14.4 µs for one ready socket and 787.2 µs for 64, with 8.522 versus 8.494 ms for
512. No ordinary-UDP speedup or tight regression bound is claimed from these
shared-workstation measurements.

Raw local CSVs and logs are under `build/windows-perf-20260915/`; they are
disposable build output and are not distributed source files.

## Implementation

- Each AFD control handle serves at most 128 registrations. Groups share the
  existing IOCP, and the available-group list makes assignment/release O(1).
  A deleted registration retains its group until its final completion is
  consumed. Empty extra groups close immediately; the embedded first group
  lasts until port teardown. A compact 50,000-registration port uses 391 AFD
  handles instead of one. Growth can now fail at ADD with allocation or
  native-handle errors, with complete registration rollback.
- TCP completion handling already called `WSAPoll` to merge current levels.
  Its successful nonterminal result now also qualifies read/write readiness
  for ET and exclusive delivery, removing up to two consecutive `select`
  calls. Terminal results, unknown protocols, unsupported provider snapshots,
  and priority qualification retain their existing paths.

## Remaining opportunities

| Area | Evidence from the implementation | Next experiment |
| --- | --- | --- |
| Global AFD target-key index | 256 fixed buckets, one SRW lock, and a chain search to unlink active requests | Measure large active sets across independent ports; consider adaptive buckets, constant-time unlinking, and lock partitioning while preserving duplicate-handle reservations |
| Per-port fd table | Raw modulo hashing of aligned SOCKET values; division during probing and cluster repair | Measure realistic handle distributions; test mixed hashing and power-of-two capacities with deletion/reuse regressions |
| Registration metadata | UDP ADD queries protocol information for classification and again for direct-AFD eligibility; valid exclusive ADD also repeats target validation | Profile connection churn and combine redundant metadata queries without changing error precedence or provider fallback |
| API and completion locks | Operations on an epfd acquire the global mutex for reference acquisition/release; each completion acquires the port mutex, and a ready drain holds it across the batch | Separate independent-port throughput from same-port contention; benchmark bounded completion batches and registry lock partitioning |
| UDP receive qualification | Per receive: duplicate/pin, file-mode check, identity query in hardened modes, private-event reset, direct AFD peek, and close | Profile the calls individually; investigate cached mode information and reduced pinning specifically under synchronized lifetime, preserving receive errors and application IOCP isolation |
| Fired oneshots / implicit ET | Fired sockets remain on the probe worklist; held ET registrations can be resampled every retry interval | Benchmark many dormant oneshots and permanently writable ET sockets; assess synchronized-mode probe elision and explicit class acknowledgement |
| Pipes and waitables | Pipe readiness uses timer polling; waitable rearming involves thread-pool wait registration/retirement | Measure timer/callback load independently; native overlapped I/O remains the appropriate comparison for large pipe workloads |

These are follow-up candidates, not measured speedups. In particular, removing
identity checks or broadening their cache lifetime would change the safety
contract and is not justified by this review. Pool locking, ready-node
initialization, and timestamp conversion are lower-priority CPU costs until
profiling shows they dominate the remaining native calls.

## Reproduction and validation

Configure a disposable Release build with `WEPOLL_EX_BUILD_BENCH=ON` and the
desired explicit `WEPOLL_EX_SOCKET_LIFETIME_MODE`. Run CTest before timing:

```sh
ctest --test-dir build-review --output-on-failure
./build-review/bench/bench_windows.exe --production
./build-review/bench/bench_rearm_batch.exe batch 2000 tcp
./build-review/bench/bench_rearm_batch.exe scalar 2000 tcp
```

The baseline TCP binary must also use the updated benchmark source, linked
against the baseline library. Keep compiler, flags, linkage, lifetime mode,
and benchmark parameters identical, alternate versions, and retain all raw
runs. The production benchmark measures armed-idle registration scaling;
its largest ready set is 512.

New regressions cover group allocation/open/submit rollback, pending-deletion
pinning, reuse of released group capacity, native close fallback across
groups, delayed reaping of multiple groups, and stale TCP read suppression
followed by a new acknowledged edge.
The qualification script includes the group-fault and TCP-snapshot cases in
its repeated Windows selection.

The full MinGW qualification used the Windows toolchain and Release flags
listed above, with `WEPOLL_EX_REPEAT=3` and `WEPOLL_EX_JOBS=4`:

| Linkage | Lifetime mode | Full suite passed | Expected skips | Selected cases, each repeated three times |
| --- | --- | ---: | ---: | ---: |
| Static + shared | best-effort | 226 | 0 | 113 |
| Static only | best-effort | 224 | 0 | 113 |
| Shared only | best-effort | 132 | 0 | 68 |
| Static + shared | strict | 226 | 0 | 113 |
| Shared only | strict | 132 | 0 | 68 |
| Static + shared | synchronized | 222 | 4 | 113 |
| Shared only | synchronized | 128 | 4 | 68 |

All seven configurations completed without failures: 1,290 full-suite passes,
eight expected native-reuse skips under synchronized lifetime, and 1,968
successful repeated executions. The strengthened multi-group reaper regression
also passed ten consecutive runs in each of the combined and static-only
best-effort Release builds. Scalar TCP and UDP benchmark smoke runs passed.

An additional best-effort Debug build used `-Wall -Wextra -Wpedantic -Werror`
and CMake's `-g`, with assertions enabled. The group-fault, TCP-snapshot, and
multi-group reaper cases each passed ten consecutive runs. Its deterministic
stress run completed 250,000 operations over 1,024 sockets, including 4,960
port rotations, with zero reported backpressure. Linux/WSL1 GCC 14.2.0 passed
strict Release CTest 8/8 with `-O3 -Wall -Wextra -Wpedantic -Werror`; the build
used a disposable directory under `/tmp` because this WSL1 mount rejected
CMake's generated-file permission operations on the Windows drive.
