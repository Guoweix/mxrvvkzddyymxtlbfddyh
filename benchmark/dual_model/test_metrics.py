#!/usr/bin/env python3

import unittest
import json
import tempfile
from pathlib import Path

from metrics import add_sampler_metrics, calculate_run_metrics, jain_fairness, percentile
from report import add_trace_metrics, rvv_summary
from scheduler_stats import detect_profile, validate_scheduler_log


def request(request_id, role, model, start, times):
    return {
        "request_id": request_id,
        "role": role,
        "model": model,
        "prompt_id": request_id.split("-")[-1],
        "send_ns": start,
        "first_token_ns": times[0],
        "last_token_ns": times[-1],
        "token_times_ns": times,
        "output_tokens": len(times),
        "completed": True,
        "token_hash": request_id,
        "error": "",
    }


class MetricsTest(unittest.TestCase):
    def test_scheduler_gang_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "scheduler.log"
            path.write_text(
                "scx_llm_pin attached successfully\n"
                "SCX_GANG_STATS rvv_team0=8 rvv_team1=8 hint_valid_workers=16 "
                "phase_valid=40 phase_prefill=10 phase_decode=20 gang_engagements=3 "
                "gang_quanta=8 gang_enqueues=12 gang_runs=100 gang_active_runs=95 "
                "dsq_dispatches=20 dsq_consumes=20 dsq_dispatches_team0=9 "
                "dsq_dispatches_team1=11 dsq_consumes_team0=9 dsq_consumes_team1=11\n"
                "SCX_GANG_SAMPLES interval_us=1000 samples=1000 engaged=100 "
                "full_gang=75 read_errors=0\n"
            )
            result = validate_scheduler_log(path, "gang-active")
            self.assertTrue(result["valid"])
            self.assertEqual(result["gang_run_match_percent"], 95.0)
            self.assertEqual(result["sample_full_gang_percent"], 75.0)

    def test_scheduler_profile_detection(self):
        command = "/tmp/scx_llm_pin --team-gang always --user-control active"
        self.assertEqual(detect_profile("external", command), "gang-active")
        lease_command = "/tmp/scx_llm_pin --team-gang prefill-lease --user-control active"
        self.assertEqual(detect_profile("external", lease_command), "phase-lease")
        self.assertEqual(detect_profile("cfs-shared", ""), "none")

    def test_scheduler_phase_lease_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "scheduler.log"
            path.write_text(
                "scx_llm_pin attached successfully\n"
                "SCX_GANG_STATS rvv_team0=8 rvv_team1=8 hint_valid_workers=16 "
                "phase_valid=40 phase_prefill=10 phase_decode=20 "
                "gang_quanta=0 gang_kicks=0 gang_enqueues=0 "
                "phase_lease_engagements=8 phase_lease_preferred=20 "
                "phase_lease_runs=100 phase_lease_preferred_runs=90 "
                "phase_lease_pending_max_ns=1200000000 phase_lease_pending_ge_4s=0 "
                "phase_lease_wakeup_candidates=30 phase_lease_wakeup_preempts=12 "
                "dsq_dispatches=20 dsq_consumes=20 dsq_dispatches_team0=9 "
                "dsq_dispatches_team1=11 dsq_consumes_team0=9 dsq_consumes_team1=11\n"
            )
            result = validate_scheduler_log(path, "phase-lease")
            self.assertTrue(result["valid"])
            self.assertEqual(result["phase_lease_run_match_percent"], 90.0)
            self.assertTrue(result["checks"]["wakeup_preemption_active"])

    def test_percentile_uses_nearest_rank(self):
        self.assertEqual(percentile([1, 2, 3, 4], 0.95), 4)
        self.assertEqual(percentile([1, 2, 3, 4], 0.50), 2)

    def test_jain_fairness(self):
        self.assertEqual(jain_fairness([2, 2]), 1.0)
        self.assertAlmostEqual(jain_fairness([1, 3]), 0.8)

    def test_run_metric_windows(self):
        second = 1_000_000_000
        workload = {
            "valid": True,
            "complete": True,
            "milestone_order_ok": True,
            "within_run_deterministic": True,
            "incumbent_model": "A",
            "injected_model": "B",
            "injection_ns": 3 * second,
            "requests": [
                request("incumbent-short", "incumbent", "A", 0, [second, 2 * second, 4 * second, 6 * second]),
                request("injected-short", "injected", "B", 3 * second, [5 * second, 6 * second]),
            ],
        }
        metrics = calculate_run_metrics(workload)
        self.assertAlmostEqual(metrics["aggregate_tps"], 1.0)
        self.assertAlmostEqual(metrics["request_metrics"][0]["e2e_tps"], 2.0 / 3.0)
        self.assertEqual(metrics["injected_ttft_mean_ms"], 2000.0)
        self.assertEqual(metrics["incumbent_pre_itl_p50_ms"], 1000.0)
        self.assertEqual(metrics["incumbent_overlap_itl_p50_ms"], 2000.0)
        self.assertEqual(metrics["incumbent_itl_degradation_percent"], 100.0)
        self.assertAlmostEqual(metrics["token_rate_jain"], 1.0)

    def test_sampler_aligns_with_wall_clock(self):
        second = 1_000_000_000
        workload = {
            "injection_wall_ns": 102 * second,
            "requests": [
                {
                    "send_wall_ns": 100 * second,
                    "last_token_wall_ns": 104 * second,
                }
            ],
        }
        run_metrics = {"overlap_end_wall_ns": 104 * second}
        samples = []
        for index, wall in enumerate((100, 102, 104)):
            samples.append(
                {
                    "monotonic_ns": (10 + index) * second,
                    "wall_time_ns": wall * second,
                    "tasks": [
                        {
                            "model": model,
                            "runtime_ns": index * runtime,
                            "runqueue_wait_ns": index * 10,
                            "timeslices": index,
                            "voluntary_ctxt": index,
                            "nonvoluntary_ctxt": index,
                            "migrations": index,
                        }
                        for model, runtime in (("A", 100), ("B", 200))
                    ],
                    "platform": {},
                }
            )
        add_sampler_metrics(run_metrics, workload, samples)
        self.assertTrue(run_metrics["scheduler"]["wall_clock_aligned"])
        self.assertEqual(run_metrics["scheduler"]["A"]["total"]["runtime_ns"], 200)
        self.assertEqual(run_metrics["scheduler"]["B"]["overlap"]["runtime_ns"], 200)

    def test_rvv_summary_reads_vector_schema(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rvv.json"
            path.write_text(
                json.dumps(
                    {
                        "vector": {"v_save_candidates": 11, "v_restore_candidates": 13},
                        "futex": {
                            "wait_completed_ops": 17,
                            "wait_blocked_ops": 7,
                            "wait_blocked_total_ns": 7000,
                            "wait_blocked_max_ns": 2000,
                        },
                        "futex_addrs": [
                            {
                                "uaddr": "0x10",
                                "wait_ops": 9,
                                "wait_blocked_ops": 5,
                                "wait_blocked_total_ns": 5000,
                            },
                            {
                                "uaddr": "0x20",
                                "wait_ops": 3,
                                "wait_blocked_ops": 2,
                                "wait_blocked_total_ns": 2000,
                            },
                        ],
                    }
                )
            )
            summary = rvv_summary(path)
            self.assertEqual(summary["v_save_candidates"], 11)
            self.assertEqual(summary["v_restore_candidates"], 13)
            self.assertEqual(summary["futex_wait_blocked_ops"], 7)
            self.assertEqual(summary["top_futex"]["uaddr"], "0x10")

    def test_add_trace_metrics_combines_both_models(self):
        metrics = {
            "total_tokens": 8,
            "rvv": {
                model: {
                    "v_save_candidates": 10,
                    "v_restore_candidates": 11,
                    "futex_wait_blocked_ops": 4,
                    "futex_wait_blocked_total_ns": 8000,
                    "top_futex": {
                        "wait_ops": 5,
                        "wait_blocked_ops": 3,
                        "wait_blocked_total_ns": 6000,
                    },
                }
                for model in ("A", "B")
            },
        }
        add_trace_metrics(metrics)
        self.assertEqual(metrics["rvv_context_candidates"], 42)
        self.assertEqual(metrics["barrier_wait_ops"], 10)
        self.assertEqual(metrics["barrier_blocked_ops"], 6)
        self.assertEqual(metrics["barrier_blocked_avg_us"], 2.0)
        self.assertEqual(metrics["barrier_blocked_ms_per_token"], 0.0015)


if __name__ == "__main__":
    unittest.main()
