#!/usr/bin/env python3
"""Aggregate the four-strategy c=2/4/8 campaign from selected run files."""

from __future__ import annotations

import csv
import json
import math
import statistics
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
CAMPAIGN_DIR = SCRIPT_DIR / "results" / "four_strategy_c2_c4_c8_20260725"
STRATEGIES = ("cfs", "observe", "phase-aware", "cfs-4plus4")
CONCURRENCIES = (2, 4, 8)
LOOPS = (1, 2, 3)


def nearest_rank_p95(values: list[float]) -> float:
    ordered = sorted(values)
    return ordered[math.ceil(0.95 * len(ordered)) - 1]


def mean_sd(values: list[float]) -> dict[str, float]:
    return {
        "mean": statistics.mean(values),
        "sd": statistics.stdev(values),
    }


def load_cell(concurrency: int, loop: int, strategy: str) -> dict:
    cell = CAMPAIGN_DIR / f"c{concurrency}" / f"loop{loop}" / strategy
    report = Path((cell / ".done").read_text(encoding="utf-8").strip())
    suite = report.parent
    summary = json.loads((suite / "summary.json").read_text(encoding="utf-8"))
    if summary["selected_runs"] != 1 or summary["valid_runs"] != 1:
        raise RuntimeError(f"cell does not contain exactly one selected valid run: {cell}")

    run_dirs = sorted(path for path in suite.glob("run*") if path.is_dir())
    selected = []
    for run_dir in run_dirs:
        run = json.loads((run_dir / "run.json").read_text(encoding="utf-8"))
        if run.get("selected"):
            selected.append((run_dir, run))
    if len(selected) != 1:
        raise RuntimeError(f"expected one selected run directory: {suite}")

    run_dir, run = selected[0]
    metrics = json.loads((run_dir / "run_metrics.json").read_text(encoding="utf-8"))
    expected_tokens = concurrency * 256
    if not run["valid"] or not metrics["valid"] or not metrics["complete"]:
        raise RuntimeError(f"invalid selected run: {run_dir}")
    if metrics["total_tokens"] != expected_tokens:
        raise RuntimeError(f"wrong token count in {run_dir}: {metrics['total_tokens']}")
    if metrics["kernel_failures"] or not metrics["kernel_valid"] or not metrics["platform_valid"]:
        raise RuntimeError(f"kernel or platform validation failed: {run_dir}")
    if strategy in {"observe", "phase-aware"}:
        scheduler = run["scheduler_validation"]
        if not scheduler["valid"] or not scheduler["alive_at_workload_end"]:
            raise RuntimeError(f"scheduler validation failed: {run_dir}")

    return {
        "concurrency": concurrency,
        "loop": loop,
        "strategy": strategy,
        "suite": str(suite),
        "aggregate_tps": metrics["aggregate_tps"],
        "injected_ttft_mean_ms": metrics["injected_ttft_mean_ms"],
        "injected_ttft_samples_ms": metrics["injected_ttft_samples_ms"],
        "context_switches_total": metrics["context_switches_total"],
        "token_rate_jain": metrics["token_rate_jain"],
        "runqueue_wait_sec": (
            metrics["incumbent_runqueue_wait_sec"] + metrics["injected_runqueue_wait_sec"]
        ),
        "temperature_max_millic": metrics["platform"]["temperature_max_millic"],
        "frequency_min_khz": metrics["platform"]["frequency_min_khz"],
        "memory_available_min_kib": metrics["platform"]["memory_available_min_kib"],
        "phase_lease_run_match_percent": metrics["phase_lease_run_match_percent"],
        "phase_lease_wakeup_preempts": metrics["phase_lease_wakeup_preempts"],
        "phase_lease_pending_max_ns": metrics["phase_lease_pending_max_ns"],
        "phase_lease_pending_ge_4s": metrics["phase_lease_pending_ge_4s"],
    }


def main() -> None:
    cells = [
        load_cell(concurrency, loop, strategy)
        for concurrency in CONCURRENCIES
        for loop in LOOPS
        for strategy in STRATEGIES
    ]
    aggregates = []
    for concurrency in CONCURRENCIES:
        for strategy in STRATEGIES:
            selected = [
                cell
                for cell in cells
                if cell["concurrency"] == concurrency and cell["strategy"] == strategy
            ]
            pooled_ttft = [
                sample
                for cell in selected
                for sample in cell["injected_ttft_samples_ms"]
            ]
            row = {
                "concurrency": concurrency,
                "strategy": strategy,
                "runs": len(selected),
                "injected_requests": len(pooled_ttft),
                "aggregate_tps": mean_sd([cell["aggregate_tps"] for cell in selected]),
                "injected_ttft_mean_ms": mean_sd(
                    [cell["injected_ttft_mean_ms"] for cell in selected]
                ),
                "injected_ttft_pooled_p95_ms": nearest_rank_p95(pooled_ttft),
                "context_switches_total": mean_sd(
                    [cell["context_switches_total"] for cell in selected]
                ),
                "token_rate_jain": mean_sd([cell["token_rate_jain"] for cell in selected]),
                "runqueue_wait_sec": mean_sd(
                    [cell["runqueue_wait_sec"] for cell in selected]
                ),
                "temperature_max_millic": max(
                    cell["temperature_max_millic"] for cell in selected
                ),
                "frequency_min_khz": min(cell["frequency_min_khz"] for cell in selected),
                "memory_available_min_kib": min(
                    cell["memory_available_min_kib"] for cell in selected
                ),
            }
            if strategy == "phase-aware":
                row["phase_lease_run_match_percent"] = mean_sd(
                    [cell["phase_lease_run_match_percent"] for cell in selected]
                )
                row["phase_lease_wakeup_preempts"] = mean_sd(
                    [cell["phase_lease_wakeup_preempts"] for cell in selected]
                )
                row["phase_lease_pending_max_ns"] = max(
                    cell["phase_lease_pending_max_ns"] for cell in selected
                )
                row["phase_lease_pending_ge_4s"] = sum(
                    cell["phase_lease_pending_ge_4s"] for cell in selected
                )
            aggregates.append(row)

    output = {"campaign": str(CAMPAIGN_DIR), "cells": cells, "aggregates": aggregates}
    (CAMPAIGN_DIR / "aggregate_summary.json").write_text(
        json.dumps(output, indent=2) + "\n", encoding="utf-8"
    )
    with (CAMPAIGN_DIR / "aggregate_summary.csv").open(
        "w", encoding="utf-8", newline=""
    ) as handle:
        writer = csv.writer(handle)
        writer.writerow(
            (
                "concurrency",
                "strategy",
                "runs",
                "aggregate_tps_mean",
                "aggregate_tps_sd",
                "injected_ttft_mean_ms",
                "injected_ttft_run_sd_ms",
                "injected_ttft_pooled_p95_ms",
                "injected_requests",
                "context_switches_mean",
                "context_switches_sd",
                "token_rate_jain_mean",
                "runqueue_wait_sec_mean",
            )
        )
        for row in aggregates:
            writer.writerow(
                (
                    row["concurrency"],
                    row["strategy"],
                    row["runs"],
                    row["aggregate_tps"]["mean"],
                    row["aggregate_tps"]["sd"],
                    row["injected_ttft_mean_ms"]["mean"],
                    row["injected_ttft_mean_ms"]["sd"],
                    row["injected_ttft_pooled_p95_ms"],
                    row["injected_requests"],
                    row["context_switches_total"]["mean"],
                    row["context_switches_total"]["sd"],
                    row["token_rate_jain"]["mean"],
                    row["runqueue_wait_sec"]["mean"],
                )
            )


if __name__ == "__main__":
    main()
