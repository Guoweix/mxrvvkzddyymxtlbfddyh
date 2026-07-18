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
        }
    )
    metrics["context_switches_total"] = (
        metrics["incumbent_context_switches"] + metrics["injected_context_switches"]
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
    metrics["platform_valid"] = platform_validation.get("valid", True)
    metrics["valid"] = bool(metrics["valid"] and metrics["platform_valid"])
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
    keys = (
        "aggregate_tps",
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
    )
    statistics = {key: summary_stats(valid_runs, key) for key in keys}
    pooled_ttfts = [value for run in valid_runs for value in run["injected_ttft_samples_ms"]]
    statistics["injected_ttft_pooled_p95_ms"] = percentile(pooled_ttfts, 0.95)

    summary = {
        "suite": suite_dir.name,
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
        ]
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for run in runs:
            writer.writerow({field: run.get(field) for field in fields})

    config = {}
    if (suite_dir / "config.json").exists():
        config = json.loads((suite_dir / "config.json").read_text())
    lines = [
        "# Dual-Model Asynchronous Benchmark",
        "",
        "## Configuration",
        "",
        f"- Strategy: `{config.get('strategy', 'unknown')}`",
        f"- Loops: `{config.get('loops', 'unknown')}`",
        f"- Directions: `{config.get('directions', 'ab')}`",
        f"- Output tokens/request: `{config.get('output_tokens', 'unknown')}`",
        f"- Injection milestone: `{config.get('injection_token', 'unknown')}` tokens from every incumbent request",
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
    labels = {
        "aggregate_tps": "Aggregate TPS",
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
    }
    for key in keys:
        item = statistics[key]
        lines.append(
            f"| {labels[key]} | {fmt(item['mean'])} | {fmt(item['sd'])} | {fmt(item['cv_percent'], 2)}% |"
        )
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
