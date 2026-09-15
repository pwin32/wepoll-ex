# Windows AFD index follow-up — September 15, 2026

The follow-up found that the active AFD target index was still expensive with
large registration sets. Expanding the index from 256 to 4,096 buckets,
giving each bucket its own lock, and unlinking completed requests directly
improved large-set ADD and active work on independent epoll instances.
Numeric target uniqueness and the socket lifetime contracts are preserved.
The optimization is opt-in: CMake's `WEPOLL_EX_LARGE_AFD_INDEX` defaults to
`OFF`. The measurements below compare the compact baseline with the larger
index enabled; they do not describe a change to the default memory footprint.
The [registration/API follow-up](WINDOWS_API_PERFORMANCE_REVIEW.md) examines
duplicate protocol queries and descriptor-table locking after this change.

## Environment and method

The baseline is `6749abb708c7cae230cbbfcbf8fb6acce809cf8c`, including the
earlier bounded AFD handle groups and TCP readiness reuse. Measurements used
Windows 10 Enterprise LTSC 10.0.17763.316, x86-64, an Intel Core i7-4700HQ
(4 cores / 8 logical processors), MSYS2 MinGW GCC 16.2.0, and CMake 4.4.2.
Both versions used static linkage and best-effort socket lifetime.

The Release libraries used `-O2 -Wall -Wextra -Wpedantic -Werror`, followed by
CMake's `-O3 -DNDEBUG -std=c11`; the effective optimization was `-O3`.
Standalone probes used the same effective C11/Release flags and static
winpthread linkage. Timing ran separately from compilation and correctness
suites. This remains a shared workstation, so the results describe these
workloads on this machine, without portable latency guarantees.

The updated rearm benchmark source was compiled against both libraries. Its
optional background count keeps idle UDP registrations on another epoll
instance. Timed sockets use explicit class acknowledgement and verify exact,
unique readiness delivery; background setup and final draining are outside
the per-cycle samples. The first two background runs used the baseline,
followed by two candidate runs and an adjacent baseline/candidate pair.
Production and independent-port comparisons used A/B/B/A orderings.

## Diagnosis

A disposable instrumented baseline counted chain visits and timed the index
searches using `QueryPerformanceCounter`. At 50,000 registrations, ADD made
4,857,889 chain visits and spent 238.3 ms in lookup, about 42% of that run's
ADD time. At 1,000 registrations the corresponding lookup cost was 0.1 ms.

The production benchmark deletes newest registrations first. Its cancellation
completion order therefore happened to remove bucket heads, hiding the
linear unlink cost. A separate forward-deletion probe made 4,857,891 visits
while unlinking the 50,000 completions, consuming another 127.2 ms. These
instrumented timings establish where work occurs; the comparisons below use
uninstrumented libraries.

## Measurements

Values shown as ranges are ranges of run-level percentiles, not pooled
percentiles. Two production runs per version used 50,000 sockets and 1,000
readiness iterations.

| Registration operation | Baseline | Candidate |
| --- | ---: | ---: |
| ADD, 1,000 sockets, p50 | 5.4–5.5 µs | 5.5–5.8 µs |
| ADD, 10,000 sockets, p50 | 6.3–6.6 µs | 5.9 µs |
| ADD, 50,000 sockets, p50 | 10.1–12.2 µs | 8.0–8.1 µs |
| ADD, 50,000 sockets, p95 | 20.6–21.8 µs | 11.1–11.3 µs |
| ADD, 50,000 sockets, operations/sec | 73,567–82,035 | 117,031–118,406 |
| DEL, 50,000 sockets, p50 | 3.7 µs | 3.7 µs |

Three rearm runs per version used 1,000 measured iterations per batch size,
with 50,000 idle sockets on the separate background epoll instance:

| UDP explicit acknowledgement plus delivery, p50 | Baseline | Candidate |
| --- | ---: | ---: |
| 1 active socket | 8.2–8.5 µs | 6.5–7.1 µs |
| 16 active sockets | 148.0–152.5 µs | 102.6–105.0 µs |
| 64 active sockets | 622.6–627.3 µs | 434.2–441.1 µs |
| 256 active sockets | 9.570–10.469 ms | 3.680–3.973 ms |

A disposable independent-port probe runs the same 16-socket UDP rearm
workload concurrently in 1, 2, or 4 threads. Each thread owns its epoll
instance and sockets; a separate idle instance supplies the background set.
A common start event releases the workers, each performs 5,000 measured
cycles, and the probe checks every worker's result. Aggregate elapsed time
also includes the workers' small setup, warmup, reporting, and cleanup costs.
Two runs per version and configuration gave:

| Idle registrations | Worker ports | Baseline acknowledgements/sec | Candidate acknowledgements/sec |
| ---: | ---: | ---: | ---: |
| 0 | 1 | 154,708–163,902 | 152,726–165,127 |
| 0 | 2 | 259,498–260,148 | 258,868–261,841 |
| 0 | 4 | 408,359–408,826 | 407,294–414,114 |
| 50,000 | 1 | 100,002–102,377 | 144,030–146,343 |
| 50,000 | 2 | 132,324–136,917 | 246,883–250,373 |
| 50,000 | 4 | 117,813–122,215 | 388,816–389,461 |

The larger index reduces traversal, while separate locks keep searches in
different buckets independent. Without the background set, the
independent-port results show no material change. TCP rearm without background
load also remained similar: the 256-socket p50 was 3.1605–3.1618 ms before and
3.1534–3.1600 ms after. One-socket baseline TCP timings varied from 6.9 to
10.8 µs, illustrating the noise in small measurements. No additional general
TCP or ordinary UDP receive speedup is claimed.

## Implementation and tradeoffs

Every numeric AFD target still has at most one active index owner. The first
request can use its provider base; overlapping registrations obtain distinct
duplicate target values and retain the existing optional reservation behavior.
Each numeric value always maps to one bucket. Claim, insertion, reservation
publication, and removal use that bucket's SRW lock; duplicate retries never
hold two bucket locks together. Port-lock-before-bucket-lock ordering remains
consistent with the existing lifetime rules.

Each registration gains an intrusive previous pointer so removal no longer
searches for the registration. Neighbor links change under the same bucket
lock. Buckets have static storage, adding no allocation, resizing, or new
failure path. On x64 the bucket array occupies 64 KiB instead of roughly
2 KiB, and each registration stores one extra pointer. Lookup still uses
separate chaining; this is a larger fixed index, not an unbounded constant-time
lookup guarantee.

The compact default retains 256 pointer-only buckets, one global SRW lock,
and singly linked removal. Both the per-bucket locks and the previous pointer
are compiled out. CMake applies the selected numeric macro to both libraries
and all tests that use internal structures. Direct-source builds default to
`WEPOLL_EX_LARGE_AFD_INDEX=0`; opt in with
`-DWEPOLL_EX_LARGE_AFD_INDEX=1` consistently across backend sources and other
internal-header consumers, including source-embedded nginx builds. No public
structure, symbol, or package ABI changes.

## Reproduction

Build Release with benchmarks enabled, the larger index selected, and an
explicit socket lifetime mode, then pass CTest before timing:

```sh
cmake -S . -B build-review -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DWEPOLL_EX_BUILD_BENCH=ON \
  -DWEPOLL_EX_SOCKET_LIFETIME_MODE=best-effort \
  -DWEPOLL_EX_LARGE_AFD_INDEX=ON \
  -DCMAKE_C_FLAGS="-O3 -Wall -Wextra -Wpedantic -Werror"
cmake --build build-review --parallel
ctest --test-dir build-review --output-on-failure
./build-review/bench/bench_windows.exe --production
./build-review/bench/bench_rearm_batch.exe batch 1000 udp 50000
./build-review/bench/bench_rearm_batch.exe batch 1000 tcp 0
```

Use the updated benchmark source against the baseline static library too.
Keep flags and parameters identical, alternate versions, and retain every
run. Raw CSVs, logs, and the disposable index/independent-port probe sources
are under `build/windows-perf-followup-20260915/` in the review workspace;
they are generated research artifacts, not distributed source.

## Correctness coverage

The new `afd-key-order` regression retains 8,192 socket requests and 64
same-socket aliases, removes and resubmits alternating sets in permuted order,
injects submission failure, and probes each original socket again to verify
that no retained target value can be reused. Existing multi-port readiness,
duplicate-reservation fallback, close/reaper, and native-reuse tests exercise
the surrounding contracts. The qualification script repeats the new case.

The full MinGW qualification used the toolchain and Release flags above with
`WEPOLL_EX_REPEAT=3` and `WEPOLL_EX_JOBS=4`. These initial results used the
larger index before the compile-time option was introduced:

| Linkage | Lifetime mode | Full suite passed | Expected skips | Repeated cases, three runs each |
| --- | --- | ---: | ---: | ---: |
| Static + shared | best-effort | 227 | 0 | 114 |
| Static only | best-effort | 225 | 0 | 114 |
| Shared only | best-effort | 132 | 0 | 68 |
| Static + shared | strict | 227 | 0 | 114 |
| Shared only | strict | 132 | 0 | 68 |
| Static + shared | synchronized | 223 | 4 | 114 |
| Shared only | synchronized | 128 | 4 | 68 |

All seven configurations completed without failures: 1,294 full-suite passes,
eight expected native-reuse skips under synchronized lifetime, and 1,980
successful repeated executions. The suites include shared-export and package
consumer checks where applicable.

An additional best-effort Debug build enabled assertions with
`-Wall -Wextra -Wpedantic -Werror` and CMake's `-g`. The new index-order test,
duplicate-key fallback, AFD-group fault test, and two multi-port readiness
cases each passed ten consecutive runs. Debug stress passed 250,000
operations over 1,024 sockets with seed `0x5eedc0de12345678`, 4,960 port
rotations, and zero reported backpressure. Linux/WSL1 GCC 14.2.0 passed
strict Release CTest 8/8 with `-O3 -Wall -Wextra -Wpedantic -Werror`.
Functional benchmark checks also passed for scalar TCP, scalar UDP, and
shared-library batched UDP with 5,000 background registrations. These checks
ran during qualification and are not used for timing comparisons.

## Compile-time option qualification

After introducing the option, the full seven-configuration qualification was
repeated separately with `WEPOLL_EX_LARGE_AFD_INDEX=OFF` and `ON`, using the
same toolchain, Release flags, three repetitions, and four build jobs. Each
setting produced the same table above: 1,294 full-suite passes, eight expected
synchronized-lifetime skips, and 1,980 successful repeated executions. Across
both settings that is 2,588 full-suite passes and 3,960 repeated executions.
Package consumers and shared-export checks passed in their applicable builds.

Both settings also passed the five Debug index/group/multi-port cases ten
times each, plus 250,000 stress operations over 128 sockets with seed
`0x5eedc0de12345678`, 4,960 port rotations, and zero reported backpressure.
Linux strict Release CTest passed 8/8 again; the 200,000-iteration latency and
1,024-registration wait-scaling benchmark checks completed successfully.

Direct-source compilation without a macro selected the compact index, and
numeric value `2` was rejected. The best-effort x64 registration layout is
336 bytes with the option disabled and 344 bytes enabled. The enabled Release
AFD and port objects have byte-identical `.text` sections to the previously
measured unconditional optimization. The option therefore preserves that
enabled implementation while compiling out its added memory by default.

The option qualification logs and layout checks are under
`build/windows-afd-index-option-20260915/`. Reproduce the two matrices from
the MinGW shell with separate build roots:

```sh
WEPOLL_EX_LARGE_AFD_INDEX=OFF ./scripts/qualify-mingw.sh build/qualify-index-off
WEPOLL_EX_LARGE_AFD_INDEX=ON ./scripts/qualify-mingw.sh build/qualify-index-on
```

## Remaining work

Large-set lookup is cheaper, but the per-port fd table still uses raw modulo
hashing and cluster repair. Registration still repeats some protocol metadata
queries. Independent ports still share the public epfd reference mutex;
completion and ready-drain work share each port's mutex. UDP receive
qualification and implicit-ET/oneshot probing also remain candidates from the
initial review. Their next changes need separate profiles; the results here
do not justify weakening identity or completion-ownership checks.
