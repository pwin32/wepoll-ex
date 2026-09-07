#!/usr/bin/env python3
"""Collect same-runner A/A and balanced A/B Windows benchmark evidence.

Intervals describe the median paired change, not individual operations. The
report deliberately leaves the regression decision to review: a noisy or
incomplete comparison must not silently authorize a merge.
"""

import argparse
import csv
import io
import json
import math
from pathlib import Path
import statistics
import subprocess


WINDOWS_KEYS = {
    ("registration_add", "sockets=1000"),
    ("registration_del", "sockets=1000"),
    *(("ready_batch", f"batch={size}") for size in (1, 16, 64, 512)),
    ("oneshot_roundtrip", "socket=1"),
    ("oneshot_rearm", "socket=1"),
    ("control_churn", "add+mod+del"),
}
CONTENTION_KEYS = {("ctl_add", ""), ("ctl_mod", ""), ("ctl_del", "")}
PROGRAMS = {
    "bench_windows": ["--max-sockets", "1000", "--iterations", "500"],
    "bench_mt_contention": ["256", "32", "3"],
}


def parse_result(program, output):
    lines = [line for line in output.splitlines() if not line.startswith("#")]
    reader = csv.DictReader(io.StringIO("\n".join(lines)))
    expected = WINDOWS_KEYS if program == "bench_windows" else CONTENTION_KEYS
    fields = ["p50_ns", "p95_ns", "p99_ns"]
    fields.append("operations_per_second" if program == "bench_windows" else "mean_ns")
    metrics = {}
    seen = set()
    for row in reader:
        key = (row.get("benchmark", row.get("operation")), row.get("parameter", ""))
        if key not in expected or key in seen or None in row:
            raise ValueError(f"unexpected or duplicate benchmark row: {row}")
        seen.add(key)
        if int(row["samples"]) <= 0:
            raise ValueError(f"empty benchmark row: {row}")
        for field in fields:
            value = float(row[field])
            if not math.isfinite(value) or value <= 0:
                raise ValueError(f"invalid metric: {row}")
            metrics["/".join((program, *key, field))] = value
    if seen != expected:
        raise ValueError(f"missing benchmark rows: {expected - seen}")
    return metrics


def paired_change(base, candidate, throughput=False):
    # Positive always means slower, including throughput metrics.
    ratio = base / candidate if throughput else candidate / base
    return (ratio - 1) * 100


def median_interval(values):
    """Exact distribution-free, at-least-95% interval for a paired median."""
    ordered = sorted(values)
    count = len(ordered)
    indices = [index for index in range(count // 2)
               if 2 * sum(math.comb(count, k) for k in range(index + 1))
               / 2 ** count <= 0.05]
    if not indices:
        raise ValueError("at least six pairs are required")
    index = max(indices)
    return [ordered[index], ordered[-index - 1]]


def summarize(calibration, comparison):
    if not calibration or not comparison:
        raise ValueError("both A/A and A/B phases are required")
    keys = set(comparison[0][0])
    for base, candidate in calibration + comparison:
        if set(base) != keys or set(candidate) != keys:
            raise ValueError("benchmark metric sets differ")
    summary = {}
    for key in sorted(keys):
        throughput = key.endswith("/operations_per_second")
        aa = [paired_change(base[key], candidate[key], throughput)
              for base, candidate in calibration]
        ab = [paired_change(base[key], candidate[key], throughput)
              for base, candidate in comparison]
        summary[key] = {
            "aa_median_percent": statistics.median(aa),
            "aa_interval_percent": median_interval(aa),
            "ab_median_percent": statistics.median(ab),
            "ab_interval_percent": median_interval(ab),
            "aa_paired_percent": aa,
            "ab_paired_percent": ab,
        }
    return summary


def run_one(directory, program, log_path):
    command = [str((directory / (program + ".exe")).resolve()), *PROGRAMS[program]]
    result = subprocess.run(command, capture_output=True, text=True, timeout=180)
    log_path.write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"benchmark exited {result.returncode}: {log_path}")
    return parse_result(program, result.stdout)


def collect_phase(label, count, base_dir, candidate_dir, output):
    pairs = []
    for pair in range(count):
        results = [{}, {}]
        order = (0, 1) if pair % 2 == 0 else (1, 0)
        for program in PROGRAMS:
            for side in order:
                directory = (base_dir, candidate_dir)[side]
                path = output / f"{label}-{pair:02d}-{side}-{program}.log"
                results[side].update(run_one(directory, program, path))
        pairs.append(results)
        print(f"{label}: pair {pair + 1}/{count} complete", flush=True)
    return pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-dir", type=Path, required=True)
    parser.add_argument("--candidate-dir", type=Path, required=True)
    parser.add_argument("--base-sha", required=True)
    parser.add_argument("--candidate-sha", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pairs", type=int, default=12)
    parser.add_argument("--calibration-pairs", type=int, default=6)
    args = parser.parse_args()
    if args.pairs < 6 or args.calibration_pairs < 6:
        parser.error("each phase needs at least six pairs")
    args.output.mkdir(parents=True, exist_ok=True)
    metadata = {"base_sha": args.base_sha, "candidate_sha": args.candidate_sha,
                "base_dir": str(args.base_dir), "candidate_dir": str(args.candidate_dir),
                "commands": PROGRAMS, "pairs": args.pairs,
                "calibration_pairs": args.calibration_pairs}
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    collect_phase("warmup", 1, args.base_dir, args.candidate_dir, args.output)
    aa = collect_phase("aa", args.calibration_pairs, args.base_dir, args.base_dir, args.output)
    ab = collect_phase("ab", args.pairs, args.base_dir, args.candidate_dir, args.output)
    summary = summarize(aa, ab)
    report = {"metadata": metadata, "summary": summary, "aa": aa, "ab": ab}
    (args.output / "comparison.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("metric,aa_median_percent,ab_median_percent,ab_95_low,ab_95_high")
    for key, row in summary.items():
        low, high = row["ab_interval_percent"]
        print(f"{key},{row['aa_median_percent']:.2f},{row['ab_median_percent']:.2f},"
              f"{low:.2f},{high:.2f}")


if __name__ == "__main__":
    main()
