#!/usr/bin/env python3
"""Metric calculations shared by the v2 report generation and unit tests."""

from __future__ import annotations

import math
import statistics


def percentile(values, quantile):
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, math.ceil(quantile * len(ordered)) - 1)
    return ordered[index]


def mean(values):
    return statistics.fmean(values) if values else None


def sample_sd(values):
    return statistics.stdev(values) if len(values) > 1 else 0.0 if values else None


def cv_percent(values):
    avg = mean(values)
    sd = sample_sd(values)
    if avg in (None, 0) or sd is None:
        return None
    return sd / avg * 100.0


def jain_fairness(values):
    values = [max(0.0, float(value)) for value in values if value is not None]
    if not values or not any(values):
        return None
    return sum(values) ** 2 / (len(values) * sum(value * value for value in values))


def request_metrics(request):
    token_times = request.get("token_times_ns") or []
    send_ns = request.get("send_ns")
    first_ns = request.get("first_token_ns")
    last_ns = request.get("last_token_ns")
    itls_ms = [
        (right - left) / 1_000_000.0
        for left, right in zip(token_times, token_times[1:])
    ]
    decode_duration_sec = (
        (last_ns - first_ns) / 1_000_000_000.0
        if first_ns is not None and last_ns is not None and last_ns > first_ns
        else 0.0
    )
    timings = request.get("timings") or {}
    return {
        "request_id": request["request_id"],
        "role": request["role"],
        "model": request["model"],
        "prompt_id": request["prompt_id"],
        "input_tokens": request.get("input_tokens", 0),
        "output_tokens": request.get("output_tokens", 0),
        "ttft_ms": (first_ns - send_ns) / 1_000_000.0 if first_ns is not None else None,
        "e2e_ms": (last_ns - send_ns) / 1_000_000.0 if last_ns is not None else None,
        "e2e_tps": (
            len(token_times) / ((last_ns - send_ns) / 1_000_000_000.0)
            if last_ns is not None and last_ns > send_ns
            else None
        ),
        "decode_tps": (
            max(0, len(token_times) - 1) / decode_duration_sec
            if decode_duration_sec > 0
            else None
        ),
        "server_prompt_tokens": timings.get("prompt_n"),
        "server_prompt_ms": timings.get("prompt_ms"),
        "server_prompt_tps": timings.get("prompt_per_second"),
        "server_predicted_tokens": timings.get("predicted_n"),
        "server_predicted_ms": timings.get("predicted_ms"),
        "server_decode_tps": timings.get("predicted_per_second"),
        "itl_p50_ms": percentile(itls_ms, 0.50),
        "itl_p95_ms": percentile(itls_ms, 0.95),
        "itl_max_ms": max(itls_ms) if itls_ms else None,
        "itls_ms": itls_ms,
        "token_hash": request.get("token_hash", ""),
        "completed": bool(request.get("completed")),
        "error": request.get("error", ""),
    }


def count_tokens_in_window(requests, start_ns, end_ns):
    return sum(
        1
        for request in requests
        for timestamp in request.get("token_times_ns", [])
        if start_ns <= timestamp <= end_ns
    )


def group_metrics(requests):
    starts = [request["send_ns"] for request in requests]
    firsts = [request["first_token_ns"] for request in requests if request.get("first_token_ns")]
    lasts = [request["last_token_ns"] for request in requests if request.get("last_token_ns")]
    total_tokens = sum(request.get("output_tokens", 0) for request in requests)
    e2e_sec = (max(lasts) - min(starts)) / 1e9 if starts and lasts else 0.0
    decode_sec = (max(lasts) - min(firsts)) / 1e9 if firsts and lasts else 0.0
    return {
        "requests": len(requests),
        "completed_requests": sum(bool(request.get("completed")) for request in requests),
        "output_tokens": total_tokens,
        "e2e_sec": e2e_sec,
        "e2e_tps": total_tokens / e2e_sec if e2e_sec > 0 else None,
        "decode_sec": decode_sec,
        "decode_tps": (
            max(0, total_tokens - len(firsts)) / decode_sec if decode_sec > 0 else None
        ),
    }


def calculate_run_metrics(workload):
    scenario = workload.get("scenario", "asynchronous")
    requests = workload.get("requests") or []
    request_rows = [request_metrics(request) for request in requests]
    injection_ns = workload.get("injection_ns")
    incumbent = [request for request in requests if request.get("role") == "incumbent"]
    injected = [request for request in requests if request.get("role") == "injected"]

    starts = [request["send_ns"] for request in requests]
    lasts = [request["last_token_ns"] for request in requests if request.get("last_token_ns")]
    makespan_sec = (max(lasts) - min(starts)) / 1_000_000_000.0 if starts and lasts else 0.0
    total_tokens = sum(request.get("output_tokens", 0) for request in requests)

    pre_itls = []
    overlap_itls = []
    for request in incumbent:
        times = request.get("token_times_ns") or []
        for left, right in zip(times, times[1:]):
            interval_ms = (right - left) / 1_000_000.0
            if injection_ns is not None and right <= injection_ns:
                pre_itls.append(interval_ms)
            elif injection_ns is not None and left >= injection_ns:
                overlap_itls.append(interval_ms)

    incumbent_end = max(
        (request["last_token_ns"] for request in incumbent if request.get("last_token_ns")),
        default=None,
    )
    injected_end = max(
        (request["last_token_ns"] for request in injected if request.get("last_token_ns")),
        default=None,
    )
    overlap_end = (
        min(incumbent_end, injected_end)
        if incumbent_end is not None and injected_end is not None
        else None
    )
    overlap_end_wall_ns = None
    if overlap_end is not None:
        for request in requests:
            if request.get("last_token_ns") == overlap_end:
                overlap_end_wall_ns = request.get("last_token_wall_ns")
                break
    overlap_sec = (
        (overlap_end - injection_ns) / 1_000_000_000.0
        if injection_ns is not None and overlap_end is not None and overlap_end > injection_ns
        else 0.0
    )
    incumbent_overlap_tokens = (
        count_tokens_in_window(incumbent, injection_ns, overlap_end) if overlap_sec > 0 else 0
    )
    injected_overlap_tokens = (
        count_tokens_in_window(injected, injection_ns, overlap_end) if overlap_sec > 0 else 0
    )
    overlap_rates = [
        incumbent_overlap_tokens / overlap_sec if overlap_sec > 0 else 0.0,
        injected_overlap_tokens / overlap_sec if overlap_sec > 0 else 0.0,
    ]

    model_labels = sorted({request.get("model") for request in requests})
    model_groups = {
        model: group_metrics([request for request in requests if request.get("model") == model])
        for model in model_labels
    }
    model_ttft_means = {
        model: mean(
            [
                row["ttft_ms"]
                for row in request_rows
                if row["model"] == model and row["ttft_ms"] is not None
            ]
        )
        for model in model_labels
    }
    model_ends = {
        model: max(
            (
                request["last_token_ns"]
                for request in requests
                if request.get("model") == model
                and request.get("last_token_ns") is not None
            ),
            default=None,
        )
        for model in model_labels
    }
    if len(model_ends) >= 2:
        ends = [value for value in model_ends.values() if value is not None]
        completion_skew_ms = (max(ends) - min(ends)) / 1_000_000.0 if len(ends) >= 2 else None
    else:
        completion_skew_ms = None
    overlap_fraction_percent = (
        overlap_sec / makespan_sec * 100.0 if makespan_sec > 0 else None
    )
    completion_skew_percent = (
        completion_skew_ms / 1000.0 / makespan_sec * 100.0
        if completion_skew_ms is not None and makespan_sec > 0
        else None
    )
    model_e2e_rates = [model_groups[model]["e2e_tps"] for model in model_labels]

    injected_ttfts = [
        row["ttft_ms"]
        for row in request_rows
        if row["role"] == "injected" and row["ttft_ms"] is not None
    ]
    all_ttfts = [row["ttft_ms"] for row in request_rows if row["ttft_ms"] is not None]
    pre_p50 = percentile(pre_itls, 0.50)
    overlap_p50 = percentile(overlap_itls, 0.50)
    degradation = (
        (overlap_p50 / pre_p50 - 1.0) * 100.0
        if pre_p50 not in (None, 0) and overlap_p50 is not None
        else None
    )

    return {
        "scenario": scenario,
        "valid": bool(workload.get("valid")),
        "complete": bool(workload.get("complete")),
        "milestone_order_ok": workload.get("milestone_order_ok"),
        "within_run_deterministic": bool(workload.get("within_run_deterministic", False)),
        "incumbent_model": workload.get("incumbent_model"),
        "injected_model": workload.get("injected_model"),
        "total_tokens": total_tokens,
        "makespan_sec": makespan_sec,
        "aggregate_tps": total_tokens / makespan_sec if makespan_sec > 0 else None,
        "ttft_mean_ms": mean(all_ttfts),
        "ttft_max_ms": max(all_ttfts) if all_ttfts else None,
        "ttft_p95_ms": percentile(all_ttfts, 0.95),
        "injected_ttft_mean_ms": mean(injected_ttfts),
        "injected_ttft_max_ms": max(injected_ttfts) if injected_ttfts else None,
        "incumbent_pre_itl_p50_ms": pre_p50,
        "incumbent_pre_itl_p95_ms": percentile(pre_itls, 0.95),
        "incumbent_overlap_itl_p50_ms": overlap_p50,
        "incumbent_overlap_itl_p95_ms": percentile(overlap_itls, 0.95),
        "incumbent_itl_degradation_percent": degradation,
        "overlap_sec": overlap_sec,
        "overlap_end_ns": overlap_end,
        "overlap_end_wall_ns": overlap_end_wall_ns,
        "incumbent_overlap_tps": overlap_rates[0],
        "injected_overlap_tps": overlap_rates[1],
        "token_rate_jain": jain_fairness(overlap_rates),
        "models": model_groups,
        "model_e2e_tps_jain": (
            jain_fairness(model_e2e_rates)
            if all(value is not None for value in model_e2e_rates)
            else None
        ),
        "model_ttft_means_ms": model_ttft_means,
        "model_completion_skew_ms": completion_skew_ms,
        "model_completion_skew_percent": completion_skew_percent,
        "release_alignment_spread_ms": workload.get("release_alignment_spread_ms"),
        "overlap_fraction_percent": overlap_fraction_percent,
        "symmetric_input_ok": workload.get("symmetric_input_ok"),
        "roles": {
            "incumbent": group_metrics(incumbent),
            "injected": group_metrics(injected),
        },
        "request_metrics": request_rows,
        "ttft_samples_ms": all_ttfts,
        "injected_ttft_samples_ms": injected_ttfts,
    }


def aggregate_sample(sample, model):
    fields = (
        "runtime_ns",
        "runqueue_wait_ns",
        "timeslices",
        "voluntary_ctxt",
        "nonvoluntary_ctxt",
        "migrations",
    )
    result = {field: 0 for field in fields}
    for task in sample.get("tasks", []):
        if task.get("model") != model:
            continue
        for field in fields:
            value = task.get(field)
            if value is not None:
                result[field] += value
    return result


def nearest_sample(samples, timestamp_ns, field="monotonic_ns"):
    if not samples:
        return None
    return min(samples, key=lambda sample: abs(sample[field] - timestamp_ns))


def sample_delta(before, after, model):
    if before is None or after is None:
        return {}
    first = aggregate_sample(before, model)
    last = aggregate_sample(after, model)
    return {field: max(0, last[field] - first[field]) for field in first}


def add_sampler_metrics(run_metrics, workload, samples):
    if not samples:
        run_metrics["scheduler"] = {}
        run_metrics["platform"] = {}
        return run_metrics

    starts = [
        request["send_wall_ns"]
        for request in workload.get("requests", [])
        if request.get("send_wall_ns")
    ]
    lasts = [
        request["last_token_wall_ns"]
        for request in workload.get("requests", [])
        if request.get("last_token_wall_ns")
    ]
    injection_ns = workload.get("injection_wall_ns")
    aligned = bool(starts and lasts)
    if aligned:
        start_sample = nearest_sample(samples, min(starts), "wall_time_ns")
        end_sample = nearest_sample(samples, max(lasts), "wall_time_ns")
        injection_sample = (
            nearest_sample(samples, injection_ns, "wall_time_ns") if injection_ns else None
        )
        overlap_end_ns = run_metrics.get("overlap_end_wall_ns")
        overlap_end_sample = (
            nearest_sample(samples, overlap_end_ns, "wall_time_ns")
            if overlap_end_ns is not None
            else end_sample
        )
    else:
        start_sample = samples[0]
        end_sample = samples[-1]
        injection_sample = None
        overlap_end_sample = end_sample

    models = sorted(
        {
            task.get("model")
            for sample in samples
            for task in sample.get("tasks", [])
        }
    )
    scheduler = {}
    overlap_runtime = []
    for model in models:
        total = sample_delta(start_sample, end_sample, model)
        overlap = (
            sample_delta(injection_sample, overlap_end_sample, model)
            if injection_sample
            else {}
        )
        scheduler[model] = {"total": total, "overlap": overlap}
        overlap_runtime.append(overlap.get("runtime_ns", 0))
    scheduler["cpu_service_jain"] = jain_fairness(overlap_runtime)
    scheduler["wall_clock_aligned"] = aligned
    run_metrics["scheduler"] = scheduler

    temperatures = []
    frequencies = []
    available_memory = []
    states = set()
    for sample in samples:
        platform = sample.get("platform", {})
        temperatures.extend(
            value
            for value in platform.get("temperatures_millic", {}).values()
            if value is not None
        )
        frequencies.extend(
            value
            for value in platform.get("frequencies_khz", {}).values()
            if value is not None
        )
        memory = platform.get("memory_kib", {}).get("MemAvailable")
        if memory is not None:
            available_memory.append(memory)
        states.add(platform.get("sched_ext_state", ""))
    run_metrics["platform"] = {
        "temperature_max_millic": max(temperatures) if temperatures else None,
        "frequency_min_khz": min(frequencies) if frequencies else None,
        "frequency_max_khz": max(frequencies) if frequencies else None,
        "memory_available_min_kib": min(available_memory) if available_memory else None,
        "sched_ext_states": sorted(states),
    }
    return run_metrics
