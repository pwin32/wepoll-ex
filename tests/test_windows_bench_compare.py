import importlib.util
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location(
    "compare", Path(__file__).resolve().parents[1] / "scripts/compare-windows-bench.py")
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


class ComparisonTests(unittest.TestCase):
    def test_parse_complete(self):
        output = "# metadata\noperation,samples,p50_ns,p95_ns,p99_ns,mean_ns\n"
        for operation in ("ctl_add", "ctl_mod", "ctl_del"):
            output += f"{operation},10,100,200,300,150\n"
        self.assertEqual(len(compare.parse_result("bench_mt_contention", output)), 12)

    def test_reject_missing_empty_duplicate_and_nan(self):
        header = "operation,samples,p50_ns,p95_ns,p99_ns,mean_ns\n"
        valid = [f"{op},10,100,200,300,150\n" for op in ("ctl_add", "ctl_mod", "ctl_del")]
        for body in ("", valid[0], "".join(valid + valid[:1]),
                     "".join(valid).replace(",10,", ",0,"),
                     "".join(valid).replace(",100,", ",nan,")):
            with self.subTest(body=body), self.assertRaises(ValueError):
                compare.parse_result("bench_mt_contention", header + body)

    def test_rearm_requires_all_sizes_and_both_timings(self):
        header = "benchmark,parameter,samples,p50_ns,p95_ns,p99_ns,operations_per_second\n"
        rows = [f"{name},{size},500,100,200,300,100000\n"
                for name, size in sorted(compare.REARM_KEYS)]
        self.assertEqual(len(compare.parse_result("bench_rearm_batch", header + "".join(rows))), 32)
        for body in ("".join(rows[1:]), "".join(rows + rows[:1]),
                     "".join(rows).replace(",100000\n", ",0\n")):
            with self.subTest(body=body), self.assertRaises(ValueError):
                compare.parse_result("bench_rearm_batch", header + body)

    def test_paired_direction_and_interval(self):
        self.assertAlmostEqual(compare.paired_change(100, 110), 10)
        self.assertAlmostEqual(compare.paired_change(110, 100, throughput=True), 10)
        self.assertEqual(compare.median_interval(list(range(12))), [2, 9])
        with self.assertRaises(ValueError):
            compare.median_interval([0] * 5)

    def test_summary_keeps_pairs_and_noise_separate(self):
        aa = [[{"latency": 100}, {"latency": value}] for value in (90, 110) * 3]
        ab = [[{"latency": 100}, {"latency": 120}] for _ in range(12)]
        row = compare.summarize(aa, ab)["latency"]
        self.assertAlmostEqual(row["aa_median_percent"], 0)
        self.assertAlmostEqual(row["ab_median_percent"], 20)
        self.assertEqual(len(row["ab_paired_percent"]), 12)


if __name__ == "__main__":
    unittest.main()
