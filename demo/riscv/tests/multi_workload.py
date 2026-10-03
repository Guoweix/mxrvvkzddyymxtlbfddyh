#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""One request per independent llama server, with paired two-wave arrivals."""

import argparse
import json
from pathlib import Path
import statistics
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from workload import (
    Workload,
    tokenize,
    build_equal_prompt_tokens,
    validate_prompt_manifest,
)
from metrics import percentile, jain_fairness, request_metrics


def calculate_metrics(requests):
    if not requests or any(not r.get("last_token_ns") for r in requests):
        return {}
    start = max(r["send_ns"] for r in requests)
    end = min(r["last_token_ns"] for r in requests)
    duration = max(0, end - start) / 1e9
    makespan = (
        max(r["last_token_ns"] for r in requests) - min(r["send_ns"] for r in requests)
    ) / 1e9
    per_process, gaps, overlap_gaps, rates = [], [], [], []
    for r in requests:
        row = request_metrics(r)
        times = r["token_times_ns"]
        local = [
            (b - a) / 1e6 for a, b in zip(times, times[1:]) if start <= a <= b <= end
        ]
        rate = sum(start <= t <= end for t in times) / duration if duration else None
        row.update(
            {
                "process_id": r["model"],
                "overlap_tps": rate,
                "overlap_itl_p95_ms": percentile(local, 0.95),
                "overlap_itl_count": len(local),
            }
        )
        row.pop("itls_ms", None)
        per_process.append(row)
        gaps.extend((b - a) / 1e6 for a, b in zip(times, times[1:]))
        overlap_gaps.extend(local)
        if rate is not None:
            rates.append(rate)
    injected = [r["ttft_ms"] for r in per_process if r["role"] == "injected"]
    return {
        "aggregate_tps": sum(r["output_tokens"] for r in requests) / makespan,
        "makespan_sec": makespan,
        "ttft_mean_ms": statistics.fmean(r["ttft_ms"] for r in per_process),
        "injected_ttft_mean_ms": statistics.fmean(injected) if injected else None,
        "itl_p95_ms": percentile(gaps, 0.95),
        "itl_p99_ms": percentile(gaps, 0.99),
        "itl_max_ms": max(gaps, default=None),
        "overlap_start_ns": start,
        "overlap_end_ns": end,
        "overlap_sec": duration,
        "overlap_itl_p95_ms": percentile(overlap_gaps, 0.95),
        "overlap_itl_count": len(overlap_gaps),
        "process_token_rate_jain": jain_fairness(rates) if duration else None,
        "processes": per_process,
        "roles": {
            role: {
                "ttft_mean_ms": (
                    statistics.fmean(
                        r["ttft_ms"] for r in per_process if r["role"] == role
                    )
                    if any(r["role"] == role for r in per_process)
                    else None
                ),
                "overlap_tps": sum(
                    r["overlap_tps"] for r in per_process if r["role"] == role
                )
                if duration
                else None,
            }
            for role in ("incumbent", "injected")
        },
    }


def run(args):
    servers = json.loads(Path(args.servers).read_text())
    n = len(servers)
    if n not in (2, 4, 8) or len({s["process_id"] for s in servers}) != n:
        raise ValueError("expected 2/4/8 distinct servers")
    if len({s["url"] for s in servers}) != n:
        raise ValueError("each process requires a distinct endpoint")
    prompts = json.loads(Path(args.prompts).read_text())[: n // 2]
    validate_prompt_manifest(prompts)
    if len(prompts) != n // 2:
        raise ValueError("one prompt per process pair required")
    source = {
        s["process_id"]: {p["id"]: tokenize(s["url"], p["prompt"], 30) for p in prompts}
        for s in servers
    }
    tokens = build_equal_prompt_tokens(prompts, source)
    for i in range(n // 2):
        p = prompts[i]["id"]
        if (
            tokens[servers[i]["process_id"]][p]
            != tokens[servers[i + n // 2]["process_id"]][p]
        ):
            raise ValueError("paired servers tokenize differently")
    work = Workload(args, prompts, tokens)
    threads = []

    def wave(indices, role):
        release = time.monotonic_ns() + 100_000_000
        for i in indices:
            s, p = servers[i], prompts[i % (n // 2)]
            t = threading.Thread(
                target=work.run_request,
                args=(role, s["process_id"], s["url"], p, i, release),
                daemon=True,
            )
            t.start()
            threads.append(t)
        return release

    wave(range(n // 2), "incumbent")
    deadline = time.monotonic() + args.milestone_timeout
    with work.condition:
        while not work.incumbent_milestone_ready() and not work.incumbent_failed():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            work.condition.wait(min(remaining, 1))
    milestone = work.incumbent_milestone_ready()
    injection = wave(range(n // 2, n), "injected") if milestone else None
    deadline = time.monotonic() + args.request_timeout + 10
    for t in threads:
        t.join(max(0, deadline - time.monotonic()))
    requests = sorted(work.results.values(), key=lambda r: r["model"])
    complete = (
        len(requests) == n
        and not any(t.is_alive() for t in threads)
        and all(
            r["completed"]
            and not r["error"]
            and r["output_tokens"] == args.output_tokens
            for r in requests
        )
    )
    by_role = {(r["role"], r["prompt_id"]): r for r in requests}
    deterministic = complete and all(
        by_role["incumbent", p["id"]]["token_hash"]
        == by_role["injected", p["id"]]["token_hash"]
        for p in prompts
    )
    result = {
        "valid": bool(complete and milestone),
        "complete": complete,
        "process_count": n,
        "requests_per_process": 1,
        "total_requests": n,
        "scenario": "asynchronous",
        "injection_ns": injection,
        "injection_token": args.injection_token,
        "milestone_ok": milestone,
        "within_run_deterministic": deterministic,
        "servers": servers,
        "requests": requests,
        "metrics": calculate_metrics(requests),
    }
    Path(args.output).write_text(json.dumps(result, indent=2) + "\n")
    if not any(t.is_alive() for t in threads):
        work.events.close()
    return result["valid"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--servers", required=True)
    parser.add_argument("--prompts", required=True)
    parser.add_argument("--events", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--output-tokens", type=int, default=32)
    parser.add_argument("--injection-token", type=int, default=4)
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--request-timeout", type=float, default=1800)
    parser.add_argument("--milestone-timeout", type=float, default=600)
    args = parser.parse_args()
    if not 1 <= args.injection_token < args.output_tokens <= 768:
        parser.error(
            "require 1 <= injection-token < output-tokens <= 768 (context=1024)"
        )
    return 0 if run(args) else 1


if __name__ == "__main__":
    sys.exit(main())
