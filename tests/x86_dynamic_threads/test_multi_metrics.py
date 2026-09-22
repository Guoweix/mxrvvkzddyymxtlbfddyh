import unittest

from multi_workload import calculate_metrics


def request(i, role, start, times):
    return {"request_id": f"r{i}", "role": role, "model": f"P{i}", "prompt_id": "p",
            "send_ns": start * 10**9, "first_token_ns": times[0] * 10**9,
            "last_token_ns": times[-1] * 10**9, "token_times_ns": [t * 10**9 for t in times],
            "output_tokens": len(times), "completed": True}


class MultiMetrics(unittest.TestCase):
    def test_common_window_excludes_both_crossing_edges(self):
        rows = [request(0, "incumbent", 0, [1, 5, 9, 20]),
                request(1, "injected", 3, [4, 6, 8, 12]),
                request(2, "injected", 6, [7, 8, 9, 10])]
        m = calculate_metrics(rows)
        self.assertEqual((m["overlap_start_ns"], m["overlap_end_ns"]), (6 * 10**9, 10 * 10**9))
        self.assertEqual(m["overlap_itl_count"], 4)
        self.assertEqual(m["overlap_itl_p95_ms"], 2000)
        self.assertEqual(m["itl_max_ms"], 11000)
        self.assertEqual([r["overlap_tps"] for r in m["processes"]], [.25, .5, 1])

    def test_no_common_overlap_has_no_fairness_estimate(self):
        rows = [request(0, "incumbent", 0, [1, 2]), request(1, "injected", 3, [4, 5])]
        m = calculate_metrics(rows)
        self.assertEqual(m["overlap_sec"], 0)
        self.assertIsNone(m["process_token_rate_jain"])
        self.assertIsNone(m["overlap_itl_p95_ms"])
        self.assertEqual(m["itl_max_ms"], 1000)
