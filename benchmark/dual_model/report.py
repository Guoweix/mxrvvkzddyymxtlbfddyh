#!/usr/bin/env python3
"""Generate per-run and suite reports for the dual-model benchmark."""

import argparse
import csv
import json
import re
from pathlib import Path

from metrics import (
    add_sampler_metrics,
    calculate_run_metrics,
    cv_percent,
    mean,
    percentile,
    sample_sd,
)


KERNEL_FAILURE = re.compile(
    r"(sched_ext.*(stall|error|disabled|watchdog)|runnable task stall|out of memory|oom-kill)",
    re.IGNORECASE,
)


def load_jsonl(path):
    if not path.exists():
        return []
    rows = []
    for line in path.read_text(errors="replace").splitlines():
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return rows


def rvv_summary(path):
    if not path.exists():
        return None
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return None
    vector_stats = data.get("vector") or data.get("global") or {}
    futex_stats = data.get("futex") or {}
    futex_addrs = [
        item for item in data.get("futex_addrs", []) if int(item.get("wait_ops", 0)) > 0
    ]
    top_futex = max(
        futex_addrs,
        key=lambda item: (int(item.get("wait_ops", 0)), int(item.get("wait_blocked_ops", 0))),
        default=None,
    )
    return {
        "v_save_candidates": vector_stats.get("v_save_candidates"),
        "v_restore_candidates": vector_stats.get("v_restore_candidates"),
        "futex_wait_completed_ops": futex_stats.get("wait_completed_ops"),
        "futex_wait_blocked_ops": futex_stats.get("wait_blocked_ops"),
        "futex_wait_blocked_total_ns": futex_stats.get("wait_blocked_total_ns"),
        "futex_wait_blocked_max_ns": futex_stats.get("wait_blocked_max_ns"),
        "top_futex": top_futex,
    }


def add_trace_metrics(metrics):
    traces = [item for item in metrics.get("rvv", {}).values() if item]
    if len(traces) != 2:
        return

    def total(field):
        values = [trace.get(field) for trace in traces]
        return sum(values) if all(isinstance(value, int) for value in values) else None

    metrics["rvv_save_candidates"] = total("v_save_candidates")
    metrics["rvv_restore_candidates"] = total("v_restore_candidates")
    if metrics["rvv_save_candidates"] is not None and metrics["rvv_restore_candidates"] is not None:
        metrics["rvv_context_candidates"] = (
            metrics["rvv_save_candidates"] + metrics["rvv_restore_candidates"]
        )

    metrics["futex_wait_blocked_ops"] = total("futex_wait_blocked_ops")
    metrics["futex_wait_blocked_total_ns"] = total("futex_wait_blocked_total_ns")
    top_futexes = [trace.get("top_futex") for trace in traces]
    if all(top_futexes):
        metrics["barrier_wait_ops"] = sum(
            int(item.get("wait_ops", 0)) for item in top_futexes
        )
        metrics["barrier_blocked_ops"] = sum(
            int(item.get("wait_blocked_ops", 0)) for item in top_futexes
        )
        metrics["barrier_blocked_total_ns"] = sum(
            int(item.get("wait_blocked_total_ns", 0)) for item in top_futexes
        )
        metrics["barrier_blocked_avg_us"] = (
            metrics["barrier_blocked_total_ns"]
            / metrics["barrier_blocked_ops"]
            / 1_000.0
            if metrics["barrier_blocked_ops"]
            else None
        )
        metrics["barrier_blocked_ms_per_token"] = (
            metrics["barrier_blocked_total_ns"] / metrics["total_tokens"] / 1_000_000.0
            if metrics.get("total_tokens")
            else None
        )


def analyze_run(run_dir):
    workload = json.loads((run_dir / "workload.json").read_text())
    metrics = calculate_run_metrics(workload)
    add_sampler_metrics(metrics, workload, load_jsonl(run_dir / "sampler.jsonl"))
    scheduler = metrics.get("scheduler", {})
    incumbent_scheduler = scheduler.get(metrics.get("incumbent_model"), {}).get("overlap", {})
    injected_scheduler = scheduler.get(metrics.get("injected_model"), {}).get("overlap", {})
    model_a_total = scheduler.get("A", {}).get("total", {})
    model_b_total = scheduler.get("B", {}).get("total", {})
    llm_cpu_runtime_sec = (
        model_a_total.get("runtime_ns", 0) + model_b_total.get("runtime_ns", 0)
    ) / 1e9
    makespan_sec = metrics.get("makespan_sec") or 0
    metrics.update(
        {
            "cpu_service_jain": scheduler.get("cpu_service_jain_post_injection"),
            "sampler_wall_clock_aligned": scheduler.get("wall_clock_aligned", False),
            "incumbent_cpu_runtime_sec": incumbent_scheduler.get("runtime_ns", 0) / 1e9,
            "injected_cpu_runtime_sec": injected_scheduler.get("runtime_ns", 0) / 1e9,
            "incumbent_runqueue_wait_sec": incumbent_scheduler.get("runqueue_wait_ns", 0) / 1e9,
            "injected_runqueue_wait_sec": injected_scheduler.get("runqueue_wait_ns", 0) / 1e9,
            "incumbent_context_switches": incumbent_scheduler.get("voluntary_ctxt", 0)
            + incumbent_scheduler.get("nonvoluntary_ctxt", 0),
            "injected_context_switches": injected_scheduler.get("voluntary_ctxt", 0)
            + injected_scheduler.get("nonvoluntary_ctxt", 0),
            "model_a_cpu_runtime_sec": model_a_total.get("runtime_ns", 0) / 1e9,
            "model_b_cpu_runtime_sec": model_b_total.get("runtime_ns", 0) / 1e9,
            "model_a_runqueue_wait_sec": model_a_total.get("runqueue_wait_ns", 0) / 1e9,
            "model_b_runqueue_wait_sec": model_b_total.get("runqueue_wait_ns", 0) / 1e9,
            "model_a_context_switches": model_a_total.get("voluntary_ctxt", 0)
            + model_a_total.get("nonvoluntary_ctxt", 0),
            "model_b_context_switches": model_b_total.get("voluntary_ctxt", 0)
            + model_b_total.get("nonvoluntary_ctxt", 0),
            "llm_cpu_runtime_sec": llm_cpu_runtime_sec,
            "llm_cpu_utilization_percent": (
                llm_cpu_runtime_sec / (makespan_sec * 8) * 100.0
                if makespan_sec > 0
                else None
            ),
        }
    )
    metrics["context_switches_total"] = (
        metrics["model_a_context_switches"] + metrics["model_b_context_switches"]
        if metrics.get("scenario") == "symmetric"
        else metrics["incumbent_context_switches"] + metrics["injected_context_switches"]
    )

    dmesg_delta = (run_dir / "dmesg_delta.log").read_text(errors="replace") if (run_dir / "dmesg_delta.log").exists() else ""
    kernel_failures = [line for line in dmesg_delta.splitlines() if KERNEL_FAILURE.search(line)]
    metrics["kernel_valid"] = not kernel_failures
    metrics["kernel_failures"] = kernel_failures
    metrics["rvv"] = {
        "A": rvv_summary(run_dir / "rvv_a.json"),
        "B": rvv_summary(run_dir / "rvv_b.json"),
    }
    add_trace_metrics(metrics)
    metrics["valid"] = bool(metrics["valid"] and metrics["kernel_valid"])

    metadata_path = run_dir / "run.json"
    metadata = json.loads(metadata_path.read_text()) if metadata_path.exists() else {}
    platform_validation = metadata.get("platform_validation") or {}
    trace_validation = metadata.get("trace_validation") or {}
    scheduler_validation = metadata.get("scheduler_validation") or {}
    gang_stats = scheduler_validation.get("stats") or {}
    gang_samples = scheduler_validation.get("samples") or {}
    metrics["platform_valid"] = platform_validation.get("valid", True)
    metrics["trace_valid"] = trace_validation.get("valid", True)
    metrics["scheduler_control_valid"] = scheduler_validation.get("valid", True)
    metrics["valid"] = bool(
        metrics["valid"]
        and metrics["platform_valid"]
        and metrics["trace_valid"]
        and metrics["scheduler_control_valid"]
    )
    metrics.update(
        {
            "run_name": run_dir.name,
            "loop": metadata.get("loop"),
            "direction": metadata.get("direction"),
            "attempt": metadata.get("attempt"),
            "selected": metadata.get("selected", True),
            "phase_hint_prefill": gang_stats.get("phase_prefill"),
            "phase_hint_decode": gang_stats.get("phase_decode"),
            "gang_engagements": gang_stats.get("gang_engagements"),
            "gang_quanta": gang_stats.get("gang_quanta"),
            "gang_wrong_runs": gang_stats.get("gang_wrong_runs"),
            "gang_run_match_percent": scheduler_validation.get("gang_run_match_percent"),
            "gang_sample_engaged": gang_samples.get("engaged"),
            "gang_sample_full_percent": scheduler_validation.get("sample_full_gang_percent"),
            "phase_lease_engagements": gang_stats.get("phase_lease_engagements"),
            "phase_lease_preferred_consumes": gang_stats.get("phase_lease_preferred"),
            "phase_lease_empty_fallbacks": gang_stats.get("phase_lease_empty"),
            "phase_lease_fallback_consumes": gang_stats.get("phase_lease_fallback"),
            "phase_lease_safety_forced": gang_stats.get("phase_lease_safety_forced"),
            "phase_lease_pending_max_ns": gang_stats.get("phase_lease_pending_max_ns"),
            "phase_lease_pending_ge_4s": gang_stats.get("phase_lease_pending_ge_4s"),
            "phase_lease_wakeup_candidates": gang_stats.get(
                "phase_lease_wakeup_candidates"
            ),
            "phase_lease_wakeup_preempts": gang_stats.get(
                "phase_lease_wakeup_preempts"
            ),
            "phase_lease_run_match_percent": scheduler_validation.get(
                "phase_lease_run_match_percent"
            ),
        }
    )
    (run_dir / "run_metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    return metrics


def fmt(value, digits=3):
    return "N/A" if value is None else f"{value:.{digits}f}"


def summary_stats(runs, key):
    values = [run[key] for run in runs if run.get(key) is not None]
    return {
        "n": len(values),
        "mean": mean(values),
        "sd": sample_sd(values),
        "cv_percent": cv_percent(values),
    }


def generate_suite(suite_dir):
    run_dirs = sorted(path for path in suite_dir.glob("run*") if (path / "workload.json").exists())
    runs = [analyze_run(path) for path in run_dirs]
    selected = [run for run in runs if run.get("selected")]

    reference_hashes = {}
    hash_mismatches = []
    for run in selected:
        for request in run["request_metrics"]:
            prompt_id = request["prompt_id"]
            token_hash = request["token_hash"]
            reference_hashes.setdefault(prompt_id, token_hash)
            if reference_hashes[prompt_id] != token_hash:
                hash_mismatches.append(
                    {
                        "run": run["run_name"],
                        "request": request["request_id"],
                        "prompt_id": prompt_id,
                        "expected": reference_hashes[prompt_id],
                        "actual": token_hash,
                    }
                )

    mismatched_runs = {item["run"] for item in hash_mismatches}
    for run in selected:
        run["cross_run_deterministic"] = run["run_name"] not in mismatched_runs
        run_path = suite_dir / run["run_name"] / "run_metrics.json"
        run_path.write_text(json.dumps(run, indent=2) + "\n")

    valid_runs = [run for run in selected if run["valid"]]
    scenario = (
        valid_runs[0].get("scenario", "asynchronous")
        if valid_runs
        else "asynchronous"
    )
    keys = (
        "aggregate_tps",
        "model_a_e2e_tps",
        "model_b_e2e_tps",
        "model_e2e_tps_jain",
        "model_a_ttft_mean_ms",
        "model_b_ttft_mean_ms",
        "model_completion_skew_ms",
        "model_completion_skew_percent",
        "pair_start_skew_max_ms",
        "release_alignment_spread_ms",
        "pair_completion_skew_max_ms",
        "overlap_fraction_percent",
        "llm_cpu_runtime_sec",
        "llm_cpu_utilization_percent",
        "model_a_runqueue_wait_sec",
        "model_b_runqueue_wait_sec",
        "model_a_context_switches",
        "model_b_context_switches",
        "injected_ttft_mean_ms",
        "injected_ttft_max_ms",
        "incumbent_pre_itl_p50_ms",
        "incumbent_overlap_itl_p50_ms",
        "incumbent_itl_degradation_percent",
        "incumbent_overlap_tps",
        "injected_overlap_tps",
        "token_rate_jain",
        "cpu_service_jain",
        "incumbent_runqueue_wait_sec",
        "injected_runqueue_wait_sec",
        "incumbent_context_switches",
        "injected_context_switches",
        "context_switches_total",
        "rvv_save_candidates",
        "rvv_restore_candidates",
        "rvv_context_candidates",
        "barrier_wait_ops",
        "barrier_blocked_ops",
        "barrier_blocked_total_ns",
        "barrier_blocked_avg_us",
        "barrier_blocked_ms_per_token",
        "futex_wait_blocked_ops",
        "futex_wait_blocked_total_ns",
        "gang_run_match_percent",
        "gang_sample_full_percent",
        "phase_lease_run_match_percent",
        "phase_lease_safety_forced",
        "phase_lease_pending_max_ns",
        "phase_lease_pending_ge_4s",
        "phase_lease_wakeup_candidates",
        "phase_lease_wakeup_preempts",
    )
    statistics = {key: summary_stats(valid_runs, key) for key in keys}
    pooled_ttfts = [value for run in valid_runs for value in run["injected_ttft_samples_ms"]]
    statistics["injected_ttft_pooled_p95_ms"] = percentile(pooled_ttfts, 0.95)

    summary = {
        "suite": suite_dir.name,
        "scenario": scenario,
        "selected_runs": len(selected),
        "valid_runs": len(valid_runs),
        "invalid_runs": [run["run_name"] for run in selected if not run["valid"]],
        "reference_token_hashes": reference_hashes,
        "hash_mismatches": hash_mismatches,
        "statistics": statistics,
        "runs": runs,
    }
    (suite_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    with (suite_dir / "summary.csv").open("w", newline="") as output:
        fields = [
            "run_name",
            "loop",
            "direction",
            "attempt",
            "selected",
            "valid",
            "aggregate_tps",
            "model_a_e2e_tps",
            "model_b_e2e_tps",
            "model_e2e_tps_jain",
            "model_a_ttft_mean_ms",
            "model_b_ttft_mean_ms",
            "model_completion_skew_ms",
            "model_completion_skew_percent",
            "pair_start_skew_max_ms",
            "release_alignment_spread_ms",
            "pair_completion_skew_max_ms",
            "overlap_fraction_percent",
            "llm_cpu_runtime_sec",
            "llm_cpu_utilization_percent",
            "model_a_runqueue_wait_sec",
            "model_b_runqueue_wait_sec",
            "model_a_context_switches",
            "model_b_context_switches",
            "injected_ttft_mean_ms",
            "injected_ttft_max_ms",
            "incumbent_pre_itl_p50_ms",
            "incumbent_overlap_itl_p50_ms",
            "incumbent_itl_degradation_percent",
            "incumbent_overlap_tps",
            "injected_overlap_tps",
            "token_rate_jain",
            "cpu_service_jain",
            "incumbent_context_switches",
            "injected_context_switches",
            "context_switches_total",
            "rvv_save_candidates",
            "rvv_restore_candidates",
            "rvv_context_candidates",
            "barrier_wait_ops",
            "barrier_blocked_ops",
            "barrier_blocked_total_ns",
            "barrier_blocked_avg_us",
            "barrier_blocked_ms_per_token",
            "futex_wait_blocked_ops",
            "futex_wait_blocked_total_ns",
            "phase_hint_prefill",
            "phase_hint_decode",
            "gang_engagements",
            "gang_quanta",
            "gang_wrong_runs",
            "gang_run_match_percent",
            "gang_sample_engaged",
            "gang_sample_full_percent",
            "phase_lease_engagements",
            "phase_lease_preferred_consumes",
            "phase_lease_empty_fallbacks",
            "phase_lease_fallback_consumes",
            "phase_lease_safety_forced",
            "phase_lease_pending_max_ns",
            "phase_lease_pending_ge_4s",
            "phase_lease_wakeup_candidates",
            "phase_lease_wakeup_preempts",
            "phase_lease_run_match_percent",
        ]
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for run in runs:
            writer.writerow({field: run.get(field) for field in fields})

    config = {}
    if (suite_dir / "config.json").exists():
        config = json.loads((suite_dir / "config.json").read_text())
    scenario = config.get("scenario", scenario)
    symmetric = scenario == "symmetric"
    configuration_lines = [
        "# Dual-Model Symmetric Benchmark" if symmetric else "# Dual-Model Asynchronous Benchmark",
        "",
        "## Configuration",
        "",
        f"- Scenario: `{scenario}`",
        f"- Strategy: `{config.get('strategy', 'unknown')}`",
        f"- Loops: `{config.get('loops', 'unknown')}`",
        f"- Startup order: `{config.get('directions', 'ab')}`"
        if symmetric
        else f"- Directions: `{config.get('directions', 'ab')}`",
        f"- Total concurrency: `{config.get('concurrency', 'unknown')}` "
        f"({config.get('requests_per_model', 'unknown')} requests/server)",
        f"- Output tokens/request: `{config.get('output_tokens', 'unknown')}`",
    ]
    if symmetric:
        configuration_lines.extend(
            [
                "- Paired release: matching A/B requests use the same absolute release time",
                f"- Maximum start skew: `{config.get('max_start_skew_ms', 'unknown')}ms`",
            ]
        )
    else:
        configuration_lines.append(
            f"- Injection milestone: `{config.get('injection_token', 'unknown')}` "
            "tokens from every incumbent request"
        )
    configuration_lines.extend(
        [
            f"- Arrival gap: `{config.get('arrival_gap_sec', 'unknown')}s`",
            f"- Metrics profile: `{config.get('metrics', 'unknown')}`",
            "",
            "## Summary",
            "",
            f"Valid selected runs: **{len(valid_runs)}/{len(selected)}**",
            "",
            "| Metric | Mean | Sample SD | CV |",
            "| --- | ---: | ---: | ---: |",
        ]
    )
    lines = configuration_lines
    labels = {
        "aggregate_tps": "Aggregate TPS",
        "model_a_e2e_tps": "Model A E2E TPS",
        "model_b_e2e_tps": "Model B E2E TPS",
        "model_e2e_tps_jain": "Model E2E TPS Jain fairness",
        "model_a_ttft_mean_ms": "Model A mean TTFT (ms)",
        "model_b_ttft_mean_ms": "Model B mean TTFT (ms)",
        "model_completion_skew_ms": "A/B completion skew (ms)",
        "model_completion_skew_percent": "A/B completion skew / makespan (%)",
        "pair_start_skew_max_ms": "Maximum paired start skew (ms)",
        "release_alignment_spread_ms": "Release alignment spread (ms)",
        "pair_completion_skew_max_ms": "Maximum paired completion skew (ms)",
        "overlap_fraction_percent": "Both-model overlap / makespan (%)",
        "llm_cpu_runtime_sec": "Combined LLM CPU runtime (s)",
        "llm_cpu_utilization_percent": "LLM CPU utilization across 8 cores (%)",
        "model_a_runqueue_wait_sec": "Model A total runqueue wait (s)",
        "model_b_runqueue_wait_sec": "Model B total runqueue wait (s)",
        "model_a_context_switches": "Model A context switches",
        "model_b_context_switches": "Model B context switches",
        "injected_ttft_mean_ms": "Injected mean TTFT (ms)",
        "injected_ttft_max_ms": "Injected max TTFT (ms)",
        "incumbent_pre_itl_p50_ms": "Incumbent pre-injection p50 ITL (ms)",
        "incumbent_overlap_itl_p50_ms": "Incumbent overlap p50 ITL (ms)",
        "incumbent_itl_degradation_percent": "Incumbent p50 ITL degradation (%)",
        "incumbent_overlap_tps": "Incumbent overlap TPS",
        "injected_overlap_tps": "Injected overlap TPS",
        "token_rate_jain": "Token-rate Jain fairness",
        "cpu_service_jain": "CPU-service Jain fairness",
        "incumbent_runqueue_wait_sec": "Incumbent overlap runqueue wait (s)",
        "injected_runqueue_wait_sec": "Injected overlap runqueue wait (s)",
        "incumbent_context_switches": "Incumbent context switches",
        "injected_context_switches": "Injected context switches",
        "context_switches_total": "Both-model context switches",
        "rvv_save_candidates": "RVV save candidates",
        "rvv_restore_candidates": "RVV restore candidates",
        "rvv_context_candidates": "RVV save+restore candidates",
        "barrier_wait_ops": "Top-futex barrier wait operations",
        "barrier_blocked_ops": "Top-futex barrier blocked waits",
        "barrier_blocked_total_ns": "Top-futex barrier blocked worker-time (ns)",
        "barrier_blocked_avg_us": "Top-futex barrier blocked average (us)",
        "barrier_blocked_ms_per_token": "Top-futex barrier blocked time/token (ms)",
        "futex_wait_blocked_ops": "All futex blocked waits",
        "futex_wait_blocked_total_ns": "All futex blocked worker-time (ns)",
        "gang_run_match_percent": "Gang active-team run match (%)",
        "gang_sample_full_percent": "Fully synchronized gang samples (%)",
        "phase_lease_run_match_percent": "Phase-lease preferred-team run match (%)",
        "phase_lease_safety_forced": "Phase-lease forced safety slices",
        "phase_lease_pending_max_ns": "Phase-lease maximum V DSQ wait (ns)",
        "phase_lease_pending_ge_4s": "Phase-lease V DSQ waits >=4s",
        "phase_lease_wakeup_candidates": "Phase-lease preferred wakeup candidates",
        "phase_lease_wakeup_preempts": "Phase-lease targeted wakeup preempts",
    }
    symmetric_display_keys = (
        "aggregate_tps",
        "model_a_e2e_tps",
        "model_b_e2e_tps",
        "model_e2e_tps_jain",
        "model_a_ttft_mean_ms",
        "model_b_ttft_mean_ms",
        "model_completion_skew_ms",
        "model_completion_skew_percent",
        "pair_start_skew_max_ms",
        "release_alignment_spread_ms",
        "pair_completion_skew_max_ms",
        "overlap_fraction_percent",
        "llm_cpu_utilization_percent",
        "cpu_service_jain",
        "model_a_runqueue_wait_sec",
        "model_b_runqueue_wait_sec",
        "model_a_context_switches",
        "model_b_context_switches",
        "context_switches_total",
        "phase_lease_run_match_percent",
        "phase_lease_pending_max_ns",
        "phase_lease_pending_ge_4s",
        "phase_lease_wakeup_preempts",
    )
    asynchronous_display_keys = ("aggregate_tps",) + keys[
        keys.index("injected_ttft_mean_ms") :
    ]
    display_keys = symmetric_display_keys if symmetric else asynchronous_display_keys
    for key in display_keys:
        item = statistics[key]
        lines.append(
            f"| {labels[key]} | {fmt(item['mean'])} | {fmt(item['sd'])} | {fmt(item['cv_percent'], 2)}% |"
        )
    if symmetric:
        lines.extend(
            [
                "",
                "## Runs",
                "",
                "| Run | Startup order | Valid | Aggregate TPS | A TPS | B TPS | Completion skew | Overlap | CPU util | Jain |",
                "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for run in selected:
            lines.append(
                f"| {run['run_name']} | {run.get('direction')} | {run['valid']} | "
                f"{fmt(run.get('aggregate_tps'))} | {fmt(run.get('model_a_e2e_tps'))} | "
                f"{fmt(run.get('model_b_e2e_tps'))} | "
                f"{fmt(run.get('model_completion_skew_ms'))} ms | "
                f"{fmt(run.get('overlap_fraction_percent'))}% | "
                f"{fmt(run.get('llm_cpu_utilization_percent'))}% | "
                f"{fmt(run.get('model_e2e_tps_jain'))} |"
            )
    else:
        lines.extend(
            [
                "",
                f"Pooled injected-request p95 TTFT: **{fmt(statistics['injected_ttft_pooled_p95_ms'])} ms**",
                "",
                "## Runs",
                "",
                "| Run | Direction | Valid | Aggregate TPS | Injected TTFT | Incumbent ITL change | Jain |",
                "| --- | --- | --- | ---: | ---: | ---: | ---: |",
            ]
        )
        for run in selected:
            lines.append(
                f"| {run['run_name']} | {run.get('direction')} | {run['valid']} | "
                f"{fmt(run.get('aggregate_tps'))} | {fmt(run.get('injected_ttft_mean_ms'))} ms | "
                f"{fmt(run.get('incumbent_itl_degradation_percent'))}% | {fmt(run.get('token_rate_jain'))} |"
            )
    if summary["invalid_runs"]:
        lines.extend(["", "Invalid runs: " + ", ".join(f"`{name}`" for name in summary["invalid_runs"])])
    if hash_mismatches:
        lines.extend(
            [
                "",
                f"Deterministic output mismatches (diagnostic only): **{len(hash_mismatches)}**",
            ]
        )
    lines.extend(["", "## Per-Task TTFT", ""])
    for run in selected:
        lines.extend(
            [
                f"### {run['run_name']}",
                "",
                "| Task | Role | Model | Prompt | Input tokens | Output tokens | E2E TPS | Decode TPS | TTFT (ms) |",
                "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for request in run["request_metrics"]:
            lines.append(
                f"| {request['request_id']} | {request['role']} | {request['model']} | "
                f"{request['prompt_id']} | {request['input_tokens']} | "
                f"{request['output_tokens']} | {fmt(request.get('e2e_tps'))} | "
                f"{fmt(request.get('decode_tps'))} | {fmt(request.get('ttft_ms'))} |"
            )
        lines.append("")
    (suite_dir / "report.md").write_text("\n".join(lines) + "\n")
    return 0 if len(valid_runs) == len(selected) and selected else 2


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("suite_dir", type=Path)
    args = parser.parse_args()
    raise SystemExit(generate_suite(args.suite_dir))


if __name__ == "__main__":
    main()
