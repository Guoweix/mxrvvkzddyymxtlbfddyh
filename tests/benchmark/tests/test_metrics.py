import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from bench.metrics import (  # noqa: E402
    calculate_run_metrics,
    cv_percent,
    jain_fairness,
    mean,
    percentile,
    request_metrics,
    sample_sd,
)


def make_request(request_id, role, model, prompt_id, send_ns, output_tokens=4):
    times = [send_ns + 1_000_000 * (index + 1) for index in range(output_tokens)]
    return {
        "request_id": request_id,
        "role": role,
        "model": model,
        "prompt_id": prompt_id,
        "input_tokens": 128,
        "output_tokens": output_tokens,
        "token_times_ns": times,
        "token_wall_times_ns": times,
        "send_ns": send_ns,
        "send_wall_ns": send_ns,
        "first_token_ns": times[0],
        "last_token_ns": times[-1],
        "first_token_wall_ns": times[0],
        "last_token_wall_ns": times[-1],
        "finish_ns": times[-1],
        "completed": True,
        "error": "",
        "timings": {},
        "token_hash": "deadbeef",
    }


class PercentileTest(unittest.TestCase):
    def test_nearest_rank(self):
        values = [1, 2, 3, 4, 5]
        self.assertEqual(percentile(values, 0.50), 3)
        self.assertEqual(percentile(values, 0.95), 5)
        self.assertIsNone(percentile([], 0.95))

    def test_stats(self):
        self.assertEqual(mean([2, 4]), 3)
        self.assertAlmostEqual(sample_sd([2, 4]), 1.4142135, places=6)
        self.assertAlmostEqual(cv_percent([10, 10]), 0.0, places=6)
        self.assertIsNone(cv_percent([]))


class JainTest(unittest.TestCase):
    def test_equal_values(self):
        self.assertAlmostEqual(jain_fairness([1, 1, 1, 1]), 1.0)

    def test_single_nonzero(self):
        self.assertAlmostEqual(jain_fairness([1, 0, 0, 0]), 0.25)


class RequestMetricsTest(unittest.TestCase):
    def test_basic_fields(self):
        row = request_metrics(make_request("incumbent-a", "incumbent", "A", "a", 0))
        self.assertEqual(row["output_tokens"], 4)
        self.assertEqual(row["ttft_ms"], 1.0)
        self.assertTrue(row["completed"])


class RunMetricsTest(unittest.TestCase):
    def test_asynchronous(self):
        workload = {
            "scenario": "asynchronous",
            "valid": True,
            "complete": True,
            "milestone_order_ok": True,
            "incumbent_model": "A",
            "injected_model": "B",
            "injection_ns": 3_000_000,
            "injection_wall_ns": 3_000_000,
            "requests": [
                make_request("incumbent-a", "incumbent", "A", "a", 0),
                make_request("incumbent-b", "incumbent", "A", "b", 0),
                make_request("injected-a", "injected", "B", "a", 3_000_000),
                make_request("injected-b", "injected", "B", "b", 3_000_000),
            ],
        }
        metrics = calculate_run_metrics(workload)
        self.assertGreater(metrics["aggregate_tps"], 0)
        self.assertIsNotNone(metrics["injected_ttft_mean_ms"])
        self.assertIn("A", metrics["models"])
        self.assertIn("B", metrics["models"])

    def test_synchronized(self):
        workload = {
            "scenario": "synchronized",
            "valid": True,
            "complete": True,
            "incumbent_model": "A",
            "injected_model": "B",
            "release_alignment_spread_ms": 0.5,
            "requests": [
                make_request("incumbent-a", "incumbent", "A", "a", 0),
                make_request("injected-a", "injected", "B", "a", 0),
            ],
        }
        metrics = calculate_run_metrics(workload)
        self.assertEqual(metrics["release_alignment_spread_ms"], 0.5)
        self.assertIsNotNone(metrics["model_e2e_tps_jain"])

    def test_multi(self):
        workload = {
            "scenario": "multi",
            "valid": True,
            "complete": True,
            "requests": [
                make_request("A-p0", "A", "A", "p0", 0),
                make_request("B-p0", "B", "B", "p0", 0),
                make_request("C-p0", "C", "C", "p0", 0),
            ],
        }
        metrics = calculate_run_metrics(workload)
        self.assertEqual(sorted(metrics["models"]), ["A", "B", "C"])
        self.assertIsNotNone(metrics["model_e2e_tps_jain"])


if __name__ == "__main__":
    unittest.main()
