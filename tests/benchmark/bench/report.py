#!/usr/bin/env python3
"""Generate per-run and suite reports for the v2 benchmark."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path

from .metrics import (
    add_sampler_metrics,
    calculate_run_metrics,
    cv_percent,
    mean,
    percentile,
    sample_sd,
)


KERNEL_FAILURE = re.compile(
    r"(sched_ext.*(stall|error|watchdog)|runnable task stall|out of memory|oom-kill)",
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
    vector = data.get("vector") or data.get("global") or {}
    futex = data.get("futex") or {}
    return {
        "v_save_candidates": vector.get("v_save_candidates"),
        "v_restore_candidates": vector.get("v_restore_candidates"),
        "futex_wait_completed_ops": futex.get("wait_completed_ops"),
        "futex_wait_blocked_ops": futex.get("wait_blocked_ops"),
        "futex_wait_blocked_total_ns": futex.get("wait_blocked_total_ns"),
        "futex_wait_blocked_max_ns": futex.get("wait_blocked_max_ns"),
    }


def add_trace_metrics(metrics):
    traces = [item for item in (metrics.get("rvv") or {}).values() if item]
    if not traces:
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


def analyze_run(run_dir):
    workload = json.loads((run_dir / "workload.json").read_text())
    metrics = calculate_run_metrics(workload)
    add_sampler_metrics(metrics, workload, load_jsonl(run_dir / "sampler.jsonl"))

    scheduler = metrics.get("scheduler", {})
    model_labels = sorted(metrics.get("models", {}))
    model_totals = {model: scheduler.get(model, {}).get("total", {}) for model in model_labels}
    llm_cpu_runtime_sec = sum(
        value.get("runtime_ns", 0) for value in model_totals.values()
    ) / 1e9
    makespan_sec = metrics.get("makespan_sec") or 0
    metrics["cpu_service_jain"] = scheduler.get("cpu_service_jain")
    metrics["sampler_wall_clock_aligned"] = scheduler.get("wall_clock_aligned", False)
    metrics["per_model_scheduler"] = scheduler
    metrics["llm_cpu_runtime_sec"] = llm_cpu_runtime_sec
    metrics["llm_cpu_utilization_percent"] = (
        llm_cpu_runtime_sec / (makespan_sec * 8) * 100.0 if makespan_sec > 0 else None
    )

    if metrics.get("scenario") == "asynchronous":
        incumbent = metrics.get("incumbent_model")
        injected = metrics.get("injected_model")
        metrics["incumbent_context_switches"] = _context_switches(scheduler, incumbent)
        metrics["injected_context_switches"] = _context_switches(scheduler, injected)
        metrics["context_switches_total"] = (
            metrics["incumbent_context_switches"] + metrics["injected_context_switches"]
        )
        metrics["incumbent_runqueue_wait_sec"] = _overlap_field(
            scheduler, incumbent, "runqueue_wait_ns"
        )
        metrics["injected_runqueue_wait_sec"] = _overlap_field(
            scheduler, injected, "runqueue_wait_ns"
        )
    else:
        metrics["context_switches_total"] = sum(
            _context_switches(scheduler, model) for model in model_labels
        )

    dmesg_path = run_dir / "dmesg_delta.log"
    dmesg_delta = dmesg_path.read_text(errors="replace") if dmesg_path.exists() else ""
    kernel_failures = [line for line in dmesg_delta.splitlines() if KERNEL_FAILURE.search(line)]
    metrics["kernel_valid"] = not kernel_failures
    metrics["kernel_failures"] = kernel_failures

    metadata_path = run_dir / "run.json"
    metadata = json.loads(metadata_path.read_text()) if metadata_path.exists() else {}
    platform_validation = metadata.get("platform_validation") or {}
    trace_validation = metadata.get("trace_validation") or {}
    control_validation = metadata.get("control_validation") or {}
    control = control_validation.get("control") or {}
    metrics["platform_valid"] = platform_validation.get("valid", True)
    metrics["trace_valid"] = trace_validation.get("valid", True)
    metrics["control_valid"] = control_validation.get("valid", True)
    metrics["control_profile"] = control_validation.get("profile")
    metrics["dynamic_grants"] = control.get("grants")
    metrics["dynamic_shrinks"] = control.get("shrinks")
    metrics["dynamic_grant_wait_p95_us"] = control.get("grant_wait_p95_us")
    metrics["dynamic_team_publish_p95_us"] = control.get("team_publish_p95_us")
    metrics["dynamic_decode_floor_targets"] = control.get("decode_floor_targets")
    metrics["dynamic_control"] = control
    metrics["scx_phase_hint_mode"] = control_validation.get("phase_hint_mode")
    for key, value in (control_validation.get("scx") or {}).items():
        metrics[f"scx_{key}"] = value
    metrics["rvv"] = {
        model: rvv_summary(run_dir / f"rvv_{str(model).lower()}.json")
        for model in model_labels
    }
    add_trace_metrics(metrics)
    metrics["valid"] = bool(
        metrics["valid"]
        and metrics["kernel_valid"]
        and metrics["platform_valid"]
        and metrics["trace_valid"]
        and metrics["control_valid"]
    )
    metrics.update(
        {
            "run_name": run_dir.name,
            "loop": metadata.get("loop"),
            "direction": metadata.get("direction"),
            "attempt": metadata.get("attempt"),
            "selected": metadata.get("selected", True),
        }
    )
    (run_dir / "run_metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    return metrics


def _context_switches(scheduler, model):
    entry = scheduler.get(model, {}).get("overlap") or scheduler.get(model, {}).get("total") or {}
    return entry.get("voluntary_ctxt", 0) + entry.get("nonvoluntary_ctxt", 0)


def _overlap_field(scheduler, model, field):
    entry = scheduler.get(model, {}).get("overlap") or {}
    return entry.get(field, 0) / 1e9


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
        (suite_dir / run["run_name"] / "run_metrics.json").write_text(
            json.dumps(run, indent=2) + "\n"
        )

    valid_runs = [run for run in selected if run["valid"]]
    scenario = next((run.get("scenario") for run in valid_runs if run.get("scenario")), None)
    if not scenario:
        scenario = next((run.get("scenario") for run in runs if run.get("scenario")), None)
    if not scenario and (suite_dir / "config.json").exists():
        scenario = json.loads((suite_dir / "config.json").read_text()).get("scenario")
    scenario = scenario or "asynchronous"
    keys = (
        "aggregate_tps",
        "ttft_mean_ms",
        "ttft_p95_ms",
        "ttft_max_ms",
        "model_e2e_tps_jain",
        "model_completion_skew_ms",
        "model_completion_skew_percent",
        "release_alignment_spread_ms",
        "overlap_fraction_percent",
        "llm_cpu_runtime_sec",
        "llm_cpu_utilization_percent",
        "context_switches_total",
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
        "rvv_save_candidates",
        "rvv_restore_candidates",
        "rvv_context_candidates",
        "futex_wait_blocked_ops",
        "futex_wait_blocked_total_ns",
        "dynamic_grants",
        "dynamic_shrinks",
        "dynamic_grant_wait_p95_us",
        "dynamic_team_publish_p95_us",
        "dynamic_decode_floor_targets",
        "scx_phase_valid",
        "scx_phase_prefill",
        "scx_phase_decode",
        "scx_lease_engagements",
        "scx_lease_preferred",
        "scx_lease_safety_forced",
        "scx_wakeup_preempts",
        "scx_vector_dsq_dispatches",
    )
    statistics = {key: summary_stats(valid_runs, key) for key in keys}
    pooled_ttfts = [value for run in valid_runs for value in run["injected_ttft_samples_ms"]]
    statistics["injected_ttft_pooled_p95_ms"] = percentile(pooled_ttfts, 0.95)
    pooled_all_ttfts = [value for run in valid_runs for value in run["ttft_samples_ms"]]
    statistics["ttft_pooled_p95_ms"] = percentile(pooled_all_ttfts, 0.95)

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

    _write_csv(suite_dir, runs)
    _write_markdown(suite_dir, valid_runs, selected, statistics, summary, hash_mismatches)
    return 0 if len(valid_runs) == len(selected) and selected else 2


def _write_csv(suite_dir, runs):
    fields = [
        "run_name",
        "loop",
        "direction",
        "attempt",
        "selected",
        "valid",
        "aggregate_tps",
        "ttft_mean_ms",
        "ttft_p95_ms",
        "ttft_max_ms",
        "model_e2e_tps_jain",
        "model_completion_skew_ms",
        "release_alignment_spread_ms",
        "overlap_fraction_percent",
        "llm_cpu_runtime_sec",
        "llm_cpu_utilization_percent",
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
        "futex_wait_blocked_ops",
        "dynamic_grants",
        "dynamic_shrinks",
        "dynamic_grant_wait_p95_us",
        "dynamic_team_publish_p95_us",
        "dynamic_decode_floor_targets",
        "scx_phase_hint_mode",
        "scx_phase_valid",
        "scx_phase_prefill",
        "scx_phase_decode",
        "scx_lease_engagements",
        "scx_lease_preferred",
        "scx_lease_safety_forced",
        "scx_wakeup_preempts",
        "scx_vector_dsq_dispatches",
        "control_valid",
    ]
    with (suite_dir / "summary.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for run in runs:
            writer.writerow({field: run.get(field) for field in fields})


def _write_markdown(suite_dir, valid_runs, selected, statistics, summary, hash_mismatches):
    scenario = summary["scenario"]
    config = {}
    if (suite_dir / "config.json").exists():
        config = json.loads((suite_dir / "config.json").read_text())

    title = {
        "asynchronous": "# Dual-Model Asynchronous Benchmark",
        "synchronized": "# Dual-Model Synchronized Benchmark",
        "multi": "# Multi-Model Benchmark",
    }.get(scenario, "# LLM Benchmark")

    lines = [
        title,
        "",
        "## Configuration",
        "",
        f"- Scenario: `{scenario}`",
        f"- Strategy: `{config.get('strategy', 'cfs')}`",
        f"- Loops: `{config.get('loops', 1)}`",
    ]
    if scenario == "asynchronous":
        lines.append(f"- Direction: `{config.get('direction', 'ab')}`")
        lines.append(f"- Injection milestone: `{config.get('injection_token')}` tokens")
    elif scenario == "synchronized":
        lines.append(f"- Startup order: `{config.get('direction', 'ab')}`")
        lines.append("- Release: all A/B requests share the same absolute release time")
    lines.extend(
        [
            f"- Total concurrency: `{config.get('concurrency', 'unknown')}`",
            f"- Output tokens/request: `{config.get('output_tokens')}`",
            "",
            "## Summary",
            "",
            f"Valid selected runs: **{len(valid_runs)}/{len(selected)}**",
        ]
    )

    labels = {
        "aggregate_tps": "Aggregate TPS",
        "ttft_mean_ms": "All-request mean TTFT (ms)",
        "ttft_p95_ms": "All-request p95 TTFT (ms)",
        "ttft_max_ms": "All-request max TTFT (ms)",
        "model_e2e_tps_jain": "Model E2E TPS Jain fairness",
        "model_completion_skew_ms": "Model completion skew (ms)",
        "model_completion_skew_percent": "Model completion skew / makespan (%)",
        "release_alignment_spread_ms": "Release alignment spread (ms)",
        "overlap_fraction_percent": "Both-model overlap / makespan (%)",
        "llm_cpu_runtime_sec": "Combined LLM CPU runtime (s)",
        "llm_cpu_utilization_percent": "LLM CPU utilization across 8 cores (%)",
        "context_switches_total": "Total context switches",
        "injected_ttft_mean_ms": "Injected mean TTFT (ms)",
        "injected_ttft_max_ms": "Injected max TTFT (ms)",
        "incumbent_pre_itl_p50_ms": "Incumbent pre-injection p50 ITL (ms)",
        "incumbent_overlap_itl_p50_ms": "Incumbent overlap p50 ITL (ms)",
        "incumbent_itl_degradation_percent": "Incumbent p50 ITL degradation (%)",
        "incumbent_overlap_tps": "Incumbent overlap TPS",
        "injected_overlap_tps": "Injected overlap TPS",
        "token_rate_jain": "Token-rate Jain fairness",
        "cpu_service_jain": "CPU-service Jain fairness",
        "incumbent_context_switches": "Incumbent context switches",
        "injected_context_switches": "Injected context switches",
        "rvv_save_candidates": "RVV save candidates",
        "rvv_restore_candidates": "RVV restore candidates",
        "rvv_context_candidates": "RVV save+restore candidates",
        "futex_wait_blocked_ops": "All futex blocked waits",
        "futex_wait_blocked_total_ns": "All futex blocked worker-time (ns)",
        "dynamic_grants": "Dynamic controller grants",
        "dynamic_shrinks": "Dynamic controller shrinks",
        "dynamic_grant_wait_p95_us": "Dynamic grant wait p95 (us)",
        "dynamic_team_publish_p95_us": "Dynamic team publish p95 (us)",
        "dynamic_decode_floor_targets": "Dynamic decode floor targets",
        "scx_phase_valid": "SCX phase hints valid",
        "scx_phase_prefill": "SCX phase prefill",
        "scx_phase_decode": "SCX phase decode",
        "scx_lease_engagements": "SCX phase-lease engagements",
        "scx_lease_preferred": "SCX phase-lease preferred consumes",
        "scx_lease_safety_forced": "SCX phase-lease safety slices",
        "scx_wakeup_preempts": "SCX targeted wakeup preempts",
        "scx_vector_dsq_dispatches": "SCX vector DSQ dispatches",
    }
    if scenario == "asynchronous":
        display_keys = (
            "aggregate_tps",
            "ttft_mean_ms",
            "ttft_p95_ms",
            "injected_ttft_mean_ms",
            "injected_ttft_max_ms",
            "incumbent_pre_itl_p50_ms",
            "incumbent_overlap_itl_p50_ms",
            "incumbent_itl_degradation_percent",
            "incumbent_overlap_tps",
            "injected_overlap_tps",
            "token_rate_jain",
            "cpu_service_jain",
            "llm_cpu_utilization_percent",
            "context_switches_total",
            "rvv_save_candidates",
            "rvv_restore_candidates",
            "futex_wait_blocked_ops",
            "dynamic_grants",
            "dynamic_shrinks",
            "dynamic_grant_wait_p95_us",
            "dynamic_team_publish_p95_us",
            "scx_phase_valid",
            "scx_phase_prefill",
            "scx_phase_decode",
            "scx_lease_engagements",
            "scx_lease_preferred",
            "scx_wakeup_preempts",
            "scx_vector_dsq_dispatches",
        )
    else:
        display_keys = (
            "aggregate_tps",
            "ttft_mean_ms",
            "ttft_p95_ms",
            "ttft_max_ms",
            "model_e2e_tps_jain",
            "model_completion_skew_ms",
            "release_alignment_spread_ms",
            "overlap_fraction_percent",
            "llm_cpu_utilization_percent",
            "cpu_service_jain",
            "context_switches_total",
            "rvv_save_candidates",
            "rvv_restore_candidates",
            "futex_wait_blocked_ops",
            "dynamic_grants",
            "dynamic_shrinks",
            "dynamic_grant_wait_p95_us",
            "dynamic_team_publish_p95_us",
            "scx_phase_valid",
            "scx_phase_prefill",
            "scx_phase_decode",
            "scx_lease_engagements",
            "scx_lease_preferred",
            "scx_wakeup_preempts",
            "scx_vector_dsq_dispatches",
        )
    lines.extend(
        [
            "",
            "| Metric | Mean | Sample SD | CV |",
            "| --- | ---: | ---: | ---: |",
        ]
    )
    for key in display_keys:
        item = statistics[key]
        lines.append(
            f"| {labels[key]} | {fmt(item['mean'])} | {fmt(item['sd'])} | "
            f"{fmt(item['cv_percent'], 2)}% |"
        )

    pooled_key = "injected_ttft_pooled_p95_ms" if scenario == "asynchronous" else "ttft_pooled_p95_ms"
    pooled_label = "Injected" if scenario == "asynchronous" else "All-request"
    lines.extend(
        [
            "",
            f"Pooled {pooled_label} p95 TTFT: **{fmt(statistics[pooled_key])} ms**",
            "",
            "## Runs",
            "",
            "| Run | Valid | Aggregate TPS | Mean TTFT | p95 TTFT | CPU util | Jain |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for run in selected:
        jain = run.get("token_rate_jain") if scenario == "asynchronous" else run.get("model_e2e_tps_jain")
        lines.append(
            f"| {run['run_name']} | {run['valid']} | {fmt(run.get('aggregate_tps'))} | "
            f"{fmt(run.get('ttft_mean_ms'))} | {fmt(run.get('ttft_p95_ms'))} | "
            f"{fmt(run.get('llm_cpu_utilization_percent'))}% | {fmt(jain)} |"
        )

    if summary["invalid_runs"]:
        lines.extend(
            ["", "Invalid runs: " + ", ".join(f"`{name}`" for name in summary["invalid_runs"])]
        )
    if hash_mismatches:
        lines.extend(
            [
                "",
                f"Deterministic output mismatches (diagnostic only): **{len(hash_mismatches)}**",
            ]
        )

    lines.extend(["", "## Per-Request Detail", ""])
    for run in selected:
        lines.extend(
            [
                f"### {run['run_name']}",
                "",
                "| Task | Role | Model | Prompt | Input | Output | E2E TPS | Decode TPS | TTFT (ms) |",
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("suite_dir", type=Path)
    args = parser.parse_args()
    raise SystemExit(generate_suite(args.suite_dir))


if __name__ == "__main__":
    main()
