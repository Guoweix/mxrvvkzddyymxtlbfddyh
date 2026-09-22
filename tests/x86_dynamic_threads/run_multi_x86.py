#!/usr/bin/env python3
"""Compare CFS and the dynamic controller with N t4 llama-server teams on x86.

N is 3 or 4 (12/16 workers on 8 SMT logical CPUs -> scheduling-induced
oversubscription). This is the scenario where x86 SMT vs scheduler matters.
"""

import argparse
import concurrent.futures
import json
import os
import signal
import statistics
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from types import SimpleNamespace

from run_dual_model_x86 import (
    DEFAULT_CONTROLLER,
    DEFAULT_LIBDIR,
    DEFAULT_MODEL,
    DEFAULT_SERVER,
    SCRIPT_DIR,
    require_active_scheduler,
    start_server,
    stop_process,
    wait_health,
)

sys.path.insert(0, str(SCRIPT_DIR))
from metrics import percentile, request_metrics  # noqa: E402
from workload import Workload, build_equal_prompt_tokens, tokenize  # noqa: E402


def wait_active_scheduler(process, timeout=30):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("dt_controller exited during attach")
        try:
            return require_active_scheduler()
        except RuntimeError as exc:
            last_error = exc
        time.sleep(0.25)
    raise RuntimeError(f"controller attach timeout: {last_error}")


def run_requests(args, run_dir, servers, prompt):
    source = {}
    for model, url, _process in servers:
        source[model] = {"fixed-1": tokenize(url, prompt["prompt"], 60)}
    prompt_tokens = build_equal_prompt_tokens([prompt], source)
    workload_args = SimpleNamespace(
        events=str(run_dir / "events.jsonl"),
        output_tokens=args.output_tokens,
        seed=args.seed,
        request_timeout=args.request_timeout,
    )
    workload = Workload(workload_args, [], prompt_tokens)
    try:
        release_ns = time.monotonic_ns() + 1_000_000_000
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(servers)) as pool:
            futures = [
                pool.submit(
                    workload.run_request,
                    model, model, url, {"id": "fixed-1"}, index, release_ns,
                )
                for index, (model, url, _process) in enumerate(servers)
            ]
            for future in futures:
                future.result()
    finally:
        workload.events.close()
    requests = sorted(workload.results.values(), key=lambda row: row["model"])
    if len(requests) != len(servers) or any(
        not row["completed"] or row["error"] or
        row["output_tokens"] != args.output_tokens or row["input_tokens"] != 128
        for row in requests
    ):
        raise RuntimeError("incomplete or invalid multi-team workload")
    (run_dir / "workload.json").write_text(json.dumps(requests, indent=2) + "\n")
    rows = [request_metrics(row) for row in requests]
    span_sec = (
        max(row["last_token_ns"] for row in requests) -
        min(row["send_ns"] for row in requests)
    ) / 1e9
    metrics = {
        "aggregate_tps": sum(row["output_tokens"] for row in requests) / span_sec,
        "makespan_sec": span_sec,
        "ttft_mean_ms": statistics.fmean(row["ttft_ms"] for row in rows),
        "ttft_p95_ms": percentile([row["ttft_ms"] for row in rows], 0.95),
        "request_metrics": rows,
    }
    (run_dir / "run_metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    return metrics


def run_one(args, suite_dir, mode, loop, prompt):
    run_dir = suite_dir / f"{mode}_run{loop:02d}"
    run_dir.mkdir()
    servers = []
    controller = None
    controller_log = None
    socket_path = f"/tmp/dt-x86-multi-{os.getpid()}-{mode}-{loop}.sock"
    dynamic = mode in {"dynamic", "pair-gang"}
    try:
        if dynamic:
            controller_log = (run_dir / "controller.log").open("w")
            command = [
                str(args.controller),
                "--socket", socket_path,
                "--mode", "dynamic",
                "--scx",
                "--grant-lifetime", "persistent",
                "--allocation-policy", "equal",
                "--events", str(run_dir / "controller-events.csv"),
            ]
            if mode == "pair-gang":
                command += ["--team-gang", "pair-gang",
                            "--gang-quantum-us", str(args.gang_quantum_ms * 1000)]
            controller = subprocess.Popen(
                command, stdout=controller_log, stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            time.sleep(1)
            wait_active_scheduler(controller)

        dynamic_socket = socket_path if dynamic else ""
        for index, port in enumerate(args.ports):
            model = chr(ord("A") + index)
            process = start_server(
                args, model, port, 1, run_dir / f"server_{model}.log",
                dynamic_socket, "",
            )
            url = f"http://127.0.0.1:{port}"
            wait_health(url, process, args.server_start_timeout)
            servers.append((model, url, process))

        # warmup each server with one tiny request
        warm = {
            "prompt": "multi team warmup", "n_predict": args.warmup_tokens,
            "temperature": 0, "top_k": 1, "top_p": 1.0, "seed": args.seed,
            "ignore_eos": True, "cache_prompt": False, "stream": False,
        }
        import urllib.error
        import urllib.request
        for _model, url, _process in servers:
            last_error = None
            for _attempt in range(10):
                request = urllib.request.Request(
                    f"{url}/completion", data=json.dumps(warm).encode(),
                    headers={"Content-Type": "application/json"}, method="POST",
                )
                try:
                    with urllib.request.urlopen(request, timeout=args.server_start_timeout) as r:
                        json.load(r)
                    last_error = None
                    break
                except (urllib.error.HTTPError, urllib.error.URLError, OSError) as exc:
                    last_error = exc
                    time.sleep(1)
            if last_error is not None:
                raise RuntimeError(f"warmup failed at {url}: {last_error}")
        time.sleep(args.quiet_sec)

        metrics = run_requests(args, run_dir, servers, prompt)
        if controller is not None:
            if controller.poll() is not None:
                raise RuntimeError("dt_controller exited during workload")
            require_active_scheduler()
            stop_process(controller, signal.SIGINT, timeout=15)
            controller_log.flush()
            metrics["controller"] = _controller_stats(run_dir / "controller.log")
            (run_dir / "run_metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
        print(
            f"[RESULT] {mode} n={len(servers)}: TPS={metrics['aggregate_tps']:.4f}, "
            f"TTFT mean/p95={metrics['ttft_mean_ms']/1000:.3f}/"
            f"{metrics['ttft_p95_ms']/1000:.3f}s, makespan={metrics['makespan_sec']:.3f}s",
            flush=True,
        )
        return metrics
    finally:
        stop_process(controller, signal.SIGINT, timeout=15)
        for _model, _url, process in reversed(servers):
            stop_process(process)
        if controller_log is not None:
            controller_log.close()
        for _model, _url, process in servers:
            process.log_file.close()


def _controller_stats(path):
    import re
    matches = re.findall(
        r"BPF enqueue=(\d+) placed=(\d+) running=(\d+) wrong_cpu=(\d+) stale_identity=(\d+)",
        path.read_text(),
    )
    if not matches:
        raise RuntimeError("controller did not print BPF counters")
    enqueue, placed, running, wrong, stale = (int(v) for v in matches[-1])
    if wrong:
        raise RuntimeError(f"wrong_cpu={wrong} (must be 0)")
    return {"enqueue": enqueue, "placed": placed, "running": running,
            "wrong_cpu": wrong, "stale_identity": stale}


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, default=DEFAULT_SERVER)
    parser.add_argument("--libdir", type=Path, default=DEFAULT_LIBDIR)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--controller", type=Path, default=DEFAULT_CONTROLLER)
    parser.add_argument("--prompts", type=Path, default=SCRIPT_DIR / "prompts.json")
    parser.add_argument("--results-dir", type=Path, default=SCRIPT_DIR / "results")
    parser.add_argument("--mode", choices=("baseline", "dynamic", "pair-gang"), action="append")
    parser.add_argument("--loops", type=int, default=1)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--gang-quantum-ms", type=int, default=100)
    parser.add_argument("--context", type=int, default=4096)
    parser.add_argument("--output-tokens", type=int, default=64)
    parser.add_argument("--warmup-tokens", type=int, default=2)
    parser.add_argument("--quiet-sec", type=float, default=5.0)
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--request-timeout", type=float, default=1800.0)
    parser.add_argument("--server-start-timeout", type=float, default=180.0)
    parser.add_argument("--ports", type=int, nargs="+", default=(18080, 18081, 18082, 18083))
    args = parser.parse_args()
    args.mode = args.mode or ["baseline", "dynamic", "pair-gang"]
    if args.threads != 4:
        parser.error("this runner requires --threads 4")
    if len(args.ports) not in (3, 4):
        parser.error("use 3 or 4 ports (3 or 4 llama-server teams)")
    if "pair-gang" in args.mode and len(args.ports) % 2:
        parser.error("pair-gang needs an even number of teams")
    if args.loops < 1 or args.output_tokens < 2 or not 1 <= args.gang_quantum_ms <= 500:
        parser.error("invalid loops, output-tokens or gang-quantum-ms")
    for path in (args.server, args.model, args.controller, args.prompts):
        if not path.exists():
            parser.error(f"missing required path: {path}")
    if any(mode != "baseline" for mode in args.mode) and os.geteuid() != 0:
        parser.error("dynamic/pair-gang modes must run as root (use sudo -n)")
    return args


def main():
    args = parse_args()
    prompts = json.loads(args.prompts.read_text())
    prompt = next(item for item in prompts if item["id"] == "fixed-1")
    suite_dir = args.results_dir / (
        datetime.now().strftime("%Y%m%d_%H%M%S") + "_x86_multi"
    )
    suite_dir.mkdir(parents=True)
    config = {
        key: str(value) if isinstance(value, Path) else value
        for key, value in vars(args).items()
    }
    (suite_dir / "config.json").write_text(json.dumps(config, indent=2) + "\n")
    summary = []
    for loop in range(1, args.loops + 1):
        for mode in args.mode:
            metrics = run_one(args, suite_dir, mode, loop, prompt)
            summary.append({
                "mode": mode,
                "teams": len(args.ports),
                "loop": loop,
                "aggregate_tps": metrics["aggregate_tps"],
                "ttft_mean_ms": metrics["ttft_mean_ms"],
                "ttft_p95_ms": metrics["ttft_p95_ms"],
                "makespan_sec": metrics["makespan_sec"],
            })
            (suite_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"[INFO] results: {suite_dir}")


if __name__ == "__main__":
    main()
