# Windows registration and API cost review — September 15, 2026

This follow-up removes one duplicate Winsock metadata query from UDP
registration. It also evaluates a native descriptor-table lock, but retains
the existing lock because the prototype increased scalar rearm cost. The
larger AFD index remains independently opt-in, with its compact default and
memory tradeoff documented in the [index review](WINDOWS_INDEX_PERFORMANCE_REVIEW.md).

## Environment and comparison

The baseline is `d9cdb0da12722542c45f051c368dd93b303b6b1b`, including the
compile-time index option. Measurements used Windows 10 Enterprise LTSC
10.0.17763.316 x86-64, an Intel Core i7-4700HQ with 4 cores / 8 logical
processors, MSYS2 MinGW GCC 16.2.0, and CMake 4.4.2. Both versions used
static linkage, best-effort socket lifetime, and
`WEPOLL_EX_LARGE_AFD_INDEX=ON` to reduce unrelated index traversal costs.

Release flags were `-O2 -Wall -Wextra -Wpedantic -Werror`, followed by CMake's
`-O3 -DNDEBUG -std=c11`; the effective optimization was `-O3`. Standalone
probes used the same effective flags and static winpthread linkage. Timing
comparisons ran after correctness checks and separately from builds and
tests. The two prototypes were measured independently against the baseline.
All results are local to this shared workstation.

## One protocol snapshot per registration

Registration previously queried `SO_PROTOCOL_INFOW` to classify the socket,
then queried it again for a UDP socket's provider-chain eligibility. A
disposable counter build observed 1,000 queries in each helper for 1,000 UDP
registrations. The registration path now obtains both classifications from
one query and stores them in the existing fields.

Direct AFD receive remains restricted to an exact IPv4/IPv6 UDP match with a
base-provider chain. Unknown, incomplete, failed, or layered metadata retains
the conservative qualification behavior. Endpoint identity and asynchronous
I/O capability checks still run independently. The helper preserves the
thread's error information and Winsock error state. No public API or
persistent allocation is added.

Initial A/B/B/A comparisons used the production benchmark and standalone
registration points. Those cold-process samples varied considerably, even
for unchanged DEL work. For example, production ADD p50 at 1,000 sockets was
5.6–5.9 microseconds in the baseline and 7.7–8.1 in the candidate, while DEL
shifted from 3.5–4.0 to 4.9–5.2 microseconds. These runs do not establish an
end-to-end improvement and are retained with the other measurements.

A focused probe reuses the checked-in registration workload. It creates the
sockets once, performs one complete ADD/DEL/close warmup, then measures fresh
epoll instances against that same socket set. Setup, reporting, and final
completion draining remain outside the per-operation samples. A/B/B/A order
gave two processes per version: ten measured rounds each at 1,000 and 10,000
sockets, and three rounds each at 50,000 sockets.

The table reports the median of the round-level ADD p50 values, with the
range of those p50 values in parentheses. These are not pooled percentiles.

| Registrations | Baseline, microseconds | One-query version, microseconds |
| ---: | ---: | ---: |
| 1,000 | 5.05 (4.6–7.0) | 4.85 (4.6–7.4) |
| 10,000 | 5.7 (5.5–6.2) | 5.5 (5.2–5.8) |
| 50,000 | 7.4 (7.3–7.7) | 7.3 (7.1–7.4) |

The warmed results suggest a modest 0.1–0.2 microsecond ADD saving. Median
DEL p50 was unchanged at 3.5 and 3.6 microseconds for the first two sizes;
at 50,000 it was 3.6 versus 3.7. The optimization removes a redundant query
without a new memory cost, but the small timing differences and overlapping
ranges do not justify a general readiness or throughput claim.

A final A/B/B/A check used the compact default
(`WEPOLL_EX_LARGE_AFD_INDEX=OFF`), with the same warmup and twenty measured
rounds per version at each size. The median of ADD p50s was 5.10 to 4.75
microseconds at 1,000 sockets and 5.85 to 5.70 at 10,000. DEL also shifted
from 3.60 to 3.20 at 1,000 sockets, so the full small-set difference cannot
be attributed to the removed query. All raw rounds are retained.

## Descriptor-lock experiment

A separate prototype replaced only the process-wide epfd table's pthread
mutex with an exclusive SRW lock. Alias, reference, close, and teardown
ordering stayed the same; per-port locks were unchanged. Its static
best-effort correctness suite passed all 225 cases.

A disposable probe gives each worker a separate empty epoll instance and
checks 500,000 `epoll_fd_count()` calls per worker. A common event starts the
workers, and aggregate elapsed time includes their start/finish scheduling.
Two runs per version in A/B/B/A order produced these calls/second ranges:

| Workers | Existing mutex | Native-lock prototype |
| ---: | ---: | ---: |
| 1 | 20,068,232–21,945,899 | 19,874,710–21,704,215 |
| 2 | 8,639,234–8,855,529 | 20,354,867–20,775,767 |
| 4 | 9,965,946–10,391,014 | 19,832,259–20,647,016 |
| 8 | 5,847,610–5,920,894 | 18,481,017–18,518,030 |

This is a clear gain in heavily contended reference bookkeeping. The
independent-port UDP rearm probe, however, showed no clear comparable gain.
Scalar rearm comparisons first used 1,000 cycles, then repeated A/B/B/A with
5,000 cycles per batch size and protocol. In those longer runs, acknowledging
64 UDP sockets took a p50 of 104.0–106.1 microseconds with the existing mutex
and 107.2–107.4 with the prototype. TCP acknowledgement at the same size was
105.8–106.8 versus 110.2–110.3 microseconds. Total readiness-cycle results
were mixed, particularly for 256 sockets.

The prototype is not included: its synthetic metadata-throughput benefit
did not carry through to a clear readiness gain, and the scalar acknowledgement
cost increased consistently. The backend retains its existing descriptor
table mutex. Further work on this bottleneck should examine lock partitioning
or fewer reference acquisitions in real workloads.

## Reproduction and artifacts

Use separate Release builds of the baseline and candidate with the same
explicit lifetime and index options. Pass correctness tests before timing:

```sh
cmake -S . -B build-api-review -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DWEPOLL_EX_BUILD_BENCH=ON \
  -DWEPOLL_EX_SOCKET_LIFETIME_MODE=best-effort \
  -DWEPOLL_EX_LARGE_AFD_INDEX=ON \
  -DCMAKE_C_FLAGS="-O3 -Wall -Wextra -Wpedantic -Werror"
cmake --build build-api-review --parallel
ctest --test-dir build-api-review --output-on-failure
./build-api-review/bench/bench_windows.exe --production
./build-api-review/bench/bench_rearm_batch.exe scalar 5000 udp 0
./build-api-review/bench/bench_rearm_batch.exe scalar 5000 tcp 0
```

The raw CSVs, counter source, warm-registration probe, and ordering scripts
are under `build/windows-metadata-review-20260915/`. The native-lock prototype,
descriptor-count and independent-port probes, and their raw comparisons are
under `build/windows-api-lock-review-20260915/`. These are disposable research
artifacts in the review workspace. They are not installed or distributed.

## Final correctness checks

The final source passed the full MinGW qualification with the compact
default, `WEPOLL_EX_REPEAT=3`, `WEPOLL_EX_JOBS=4`, and the Release flags above:

| Linkage | Lifetime mode | Full suite passed | Expected skips | Repeated cases, three runs each |
| --- | --- | ---: | ---: | ---: |
| Static + shared | best-effort | 227 | 0 | 114 |
| Static only | best-effort | 225 | 0 | 114 |
| Shared only | best-effort | 132 | 0 | 68 |
| Static + shared | strict | 227 | 0 | 114 |
| Shared only | strict | 132 | 0 | 68 |
| Static + shared | synchronized | 223 | 4 | 114 |
| Shared only | synchronized | 128 | 4 | 68 |

That is 1,294 full-suite passes, eight expected native-reuse skips, and 1,980
successful repeated executions. A separate combined best-effort build with
the larger index enabled passed 227 full-suite cases and 342 repeated
executions. Shared-export and installed package-consumer checks passed.
The final enabled Release AFD and port objects have byte-identical `.text`
sections to the measured metadata prototype.

The mapping regression now queries live TCP and UDP sockets over IPv4/IPv6,
an invalid socket, and a valid non-socket HANDLE. It checks protocol and
eligibility results, clears stale eligibility on failure, and checks exact
error-state preservation. Existing synthetic metadata cases retain coverage
of short, missing, malformed, and layered-provider responses.

A best-effort Debug build with the larger index, `-Wall -Wextra -Wpedantic
-Werror`, and CMake's `-g` passed the mapping and three UDP qualification
regressions ten times each. Its 1,024-socket stress run passed 246,386
operations before reaching the 120-second bound while other builds ran.
After builds and suites finished, a baseline/candidate check with a 20-second
bound completed all 250,000 operations in both binaries, each in 4.016
seconds, with 4,960 port rotations and zero backpressure. Both used seed
`0x5eedc0de12345678`. This resolved the earlier stress timing concern.

Linux/WSL1 GCC 14.2.0 passed strict Release CTest 8/8 with
`-O3 -Wall -Wextra -Wpedantic -Werror`. The qualification, Debug, Linux,
and quiet stress logs are retained alongside the metadata comparison CSVs.
