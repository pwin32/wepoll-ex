#!/usr/bin/env python3
"""Run a balanced paired h2load comparison against level and edge nginx."""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import random
import re
import statistics
import subprocess
import sys
import time
from typing import Dict, List, Optional, Sequence


DEFAULT_SEED = 0xE9E5A11CE
FINISHED_DETAILED_RE = re.compile(
    r"finished in ([^,]+),\s+([0-9.,]+[kKmMgG]?) requests,\s+"
    r"([0-9.,]+[kKmMgG]?) errors,\s+([0-9.]+[kKmMgG]?) req/s"
)
FINISHED_RATE_RE = re.compile(
    r"finished in ([^,]+),\s+([0-9.]+[kKmMgG]?) req/s"
)
REQUESTS_RE = re.compile(
    r"requests:\s+(\d+) total,\s+(\d+) started,\s+(\d+) done,\s+"
    r"(\d+) succeeded,\s+(\d+) failed,\s+(\d+) errored,\s+(\d+) timeout"
)
STATUS_RE = re.compile(
    r"status codes:\s+(\d+) 2xx,\s+(\d+) 3xx,\s+(\d+) 4xx,\s+(\d+) 5xx"
)


@dataclasses.dataclass(frozen=True)
class Result:
    variant: str
    pair: int
    order: int
    requests: int
    requests_per_second: float
    errors: int
    failed: int
    errored: int
    timeouts: int
    status_2xx: int


class ComparisonError(RuntimeError):
    pass


def positive_int(text: str) -> int:
    value = int(text, 0)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def nonnegative_int(text: str) -> int:
    value = int(text, 0)
    if value < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return value


def positive_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("must be finite and positive")
    return value


def nonnegative_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value) or value < 0:
        raise argparse.ArgumentTypeError("must be finite and nonnegative")
    return value


def scaled_number(text: str) -> float:
    normalized = text.replace(",", "")
    multiplier = 1.0
    if normalized[-1:] in "kK":
        multiplier = 1_000.0
        normalized = normalized[:-1]
    elif normalized[-1:] in "mM":
        multiplier = 1_000_000.0
        normalized = normalized[:-1]
    elif normalized[-1:] in "gG":
        multiplier = 1_000_000_000.0
        normalized = normalized[:-1]
    return float(normalized) * multiplier


def parse_h2load_output(output: str, variant: str, pair: int,
                        order: int) -> Result:
    finished_detailed = FINISHED_DETAILED_RE.search(output)
    finished_rate = FINISHED_RATE_RE.search(output)
    requests = REQUESTS_RE.search(output)
    statuses = STATUS_RE.search(output)
    if ((finished_detailed is None and finished_rate is None) or
            requests is None or statuses is None):
        raise ComparisonError("could not parse the h2load summary")

    total = int(requests.group(1))
    done = int(requests.group(3))
    succeeded = int(requests.group(4))
    failed = int(requests.group(5))
    errored = int(requests.group(6))
    timeouts = int(requests.group(7))
    errors = (
        int(scaled_number(finished_detailed.group(3)))
        if finished_detailed is not None else 0
    )
    requests_per_second = (
        scaled_number(finished_detailed.group(4))
        if finished_detailed is not None
        else scaled_number(finished_rate.group(2))
    )
    status_2xx = int(statuses.group(1))
    if total == 0 or done != total or succeeded != total or status_2xx != total:
        raise ComparisonError(
            "incomplete h2load run: "
            f"total={total} done={done} succeeded={succeeded} 2xx={status_2xx}"
        )
    if errors != 0 or failed != 0 or errored != 0 or timeouts != 0:
        raise ComparisonError(
            "h2load reported failures: "
            f"errors={errors} failed={failed} errored={errored} "
            f"timeouts={timeouts}"
        )

    return Result(
        variant=variant,
        pair=pair,
        order=order,
        requests=total,
        requests_per_second=requests_per_second,
        errors=errors,
        failed=failed,
        errored=errored,
        timeouts=timeouts,
        status_2xx=status_2xx,
    )


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Compare two already-running nginx endpoints with alternating, "
            "balanced HTTP/1.1 h2load runs."
        )
    )
    parser.add_argument("level_url", nargs="?")
    parser.add_argument("edge_url", nargs="?")
    parser.add_argument("--h2load", default="h2load")
    parser.add_argument("--pairs", type=positive_int, default=8)
    parser.add_argument("--duration", type=positive_float, default=6.0)
    parser.add_argument("--warmup-duration", type=nonnegative_float,
                        default=2.0)
    parser.add_argument("--connections", type=positive_int, default=32)
    parser.add_argument("--threads", type=positive_int, default=2)
    parser.add_argument("--max-concurrent-streams", type=positive_int,
                        default=1)
    parser.add_argument("--pause-ms", type=nonnegative_int, default=500)
    parser.add_argument("--seed", type=lambda text: int(text, 0),
                        default=DEFAULT_SEED)
    parser.add_argument("--header", action="append", default=[])
    parser.add_argument("--self-test", action="store_true")
    return parser


def h2load_command(arguments: argparse.Namespace, url: str,
                   duration: float) -> List[str]:
    command = [
        arguments.h2load,
        "--h1",
        "-D", f"{duration:g}s",
        "-c", str(arguments.connections),
        "-t", str(arguments.threads),
        "-m", str(arguments.max_concurrent_streams),
    ]
    for header in arguments.header:
        command.extend(("-H", header))
    command.append(url)
    return command


def invoke(arguments: argparse.Namespace, variant: str, url: str, pair: int,
           order: int, duration: float) -> Result:
    command = h2load_command(arguments, url, duration)
    timeout = max(15.0, duration * 3.0 + 10.0)
    try:
        completed = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ComparisonError(f"cannot run h2load: {error}") from error
    if completed.returncode != 0:
        raise ComparisonError(
            f"h2load exited {completed.returncode}: {completed.stdout.strip()}"
        )
    return parse_h2load_output(completed.stdout, variant, pair, order)


def result_record(result: Result) -> Dict[str, object]:
    return dataclasses.asdict(result)


def percentile(values: Sequence[float], fraction: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def summarize(results: Sequence[Result], seed: int) -> Dict[str, object]:
    by_variant: Dict[str, List[Result]] = {"level": [], "edge": []}
    by_pair: Dict[int, Dict[str, Result]] = {}
    for result in results:
        by_variant[result.variant].append(result)
        by_pair.setdefault(result.pair, {})[result.variant] = result

    deltas: List[float] = []
    for pair in sorted(by_pair):
        variants = by_pair[pair]
        if set(variants) != {"level", "edge"}:
            raise ComparisonError(f"pair {pair} is incomplete")
        level_rate = variants["level"].requests_per_second
        edge_rate = variants["edge"].requests_per_second
        deltas.append((edge_rate - level_rate) * 100.0 / level_rate)

    variants_summary: Dict[str, object] = {}
    for variant, variant_results in by_variant.items():
        rates = [result.requests_per_second for result in variant_results]
        variants_summary[variant] = {
            "runs": len(rates),
            "requests": sum(result.requests for result in variant_results),
            "median_requests_per_second": statistics.median(rates),
            "mean_requests_per_second": statistics.fmean(rates),
            "min_requests_per_second": min(rates),
            "max_requests_per_second": max(rates),
        }

    return {
        "result": "pass",
        "seed": f"0x{seed:x}",
        "variants": variants_summary,
        "paired_edge_delta_percent": {
            "median": statistics.median(deltas),
            "mean": statistics.fmean(deltas),
            "p10": percentile(deltas, 0.10),
            "p90": percentile(deltas, 0.90),
            "min": min(deltas),
            "max": max(deltas),
        },
    }


def self_test() -> int:
    sample = """
finished in 6.00s, 480.00k requests, 0 errors, 80.00k req/s, 9.00MB/s
requests: 480000 total, 480000 started, 480000 done, 480000 succeeded, 0 failed, 0 errored, 0 timeout
status codes: 480000 2xx, 0 3xx, 0 4xx, 0 5xx
"""
    parsed = parse_h2load_output(sample, "level", 1, 1)
    if parsed.requests != 480000 or parsed.requests_per_second != 80000.0:
        raise ComparisonError("parser self-test produced the wrong values")
    summary = summarize(
        (
            parsed,
            dataclasses.replace(parsed, variant="edge", order=2,
                                requests_per_second=84000.0),
        ),
        DEFAULT_SEED,
    )
    delta = summary["paired_edge_delta_percent"]
    if not isinstance(delta, dict) or delta["median"] != 5.0:
        raise ComparisonError("summary self-test produced the wrong delta")
    print("PASS nginx h2load comparison self-test")
    return 0


def run(arguments: argparse.Namespace) -> int:
    if arguments.self_test:
        return self_test()
    if arguments.level_url is None or arguments.edge_url is None:
        raise ComparisonError("level_url and edge_url are required")

    variants: Dict[str, str] = {
        "level": arguments.level_url,
        "edge": arguments.edge_url,
    }
    if arguments.warmup_duration:
        warmup_order = ["level", "edge"]
        random.Random(arguments.seed).shuffle(warmup_order)
        for order, variant in enumerate(warmup_order, 1):
            result = invoke(
                arguments,
                variant,
                variants[variant],
                0,
                order,
                arguments.warmup_duration,
            )
            print("WARMUP " + json.dumps(result_record(result), sort_keys=True),
                  flush=True)

    results: List[Result] = []
    first_order = ["level", "edge"]
    random.Random(arguments.seed ^ 0xA17E).shuffle(first_order)
    for pair in range(1, arguments.pairs + 1):
        order_variants = first_order if pair % 2 else list(reversed(first_order))
        for order, variant in enumerate(order_variants, 1):
            result = invoke(
                arguments,
                variant,
                variants[variant],
                pair,
                order,
                arguments.duration,
            )
            results.append(result)
            print("RUN " + json.dumps(result_record(result), sort_keys=True),
                  flush=True)
            if arguments.pause_ms:
                time.sleep(arguments.pause_ms / 1000.0)

    print("SUMMARY " + json.dumps(summarize(results, arguments.seed),
                                  sort_keys=True), flush=True)
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = make_parser()
    arguments = parser.parse_args(argv)
    try:
        return run(arguments)
    except (ComparisonError, ValueError) as error:
        print(f"FAIL nginx h2load comparison: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
