#!/usr/bin/env python3
"""Run the deterministic dual-model workload on the local x86 host with the
dynamic_threads controller (baseline / dynamic / dynamic-phase)."""

import argparse
import concurrent.futures
import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_DIR = SCRIPT_DIR.parent.parent
SRC_DIR = REPO_DIR / "src" / "x86_dynamic_threads"
sys.path.insert(0, str(SCRIPT_DIR))
import config_env  # noqa: E402

DEFAULT_SERVER = config_env.path(
    "DT_X86_LLAMA_BIN", str(SRC_DIR / "llama" / "build-x86" / "bin" / "llama-server"))
DEFAULT_LIBDIR = config_env.path(
    "DT_X86_LIBDIR", str(SRC_DIR / "llama" / "build-x86" / "bin"))
DEFAULT_MODEL = config_env.path(
    "DT_X86_MODEL",
    str(REPO_DIR.parent / "riscv-linux" / "host-share" / "Llama-3.2-3B-Instruct-Q4_K_M.gguf"))
DEFAULT_CONTROLLER = SRC_DIR / "build" / "dt_controller"
SCHED_EXT_SYSFS = Path("/sys/kernel/sched_ext")


def http_json(url, payload=None, timeout=30):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"} if data is not None else {},
        method="POST" if data is not None else "GET",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.load(response)


def wait_health(url, process, timeout=180):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited with status {process.returncode}")
        try:
            status = http_json(f"{url}/health", timeout=5).get("status")
            if status in {"ok", "no slot available"}:
                return
        except (OSError, urllib.error.URLError, json.JSONDecodeError) as exc:
            last_error = exc
        time.sleep(0.5)
    raise RuntimeError(f"server health timeout: {last_error}")


def stop_process(process, sig=signal.SIGTERM, timeout=15):
    if process is None or process.poll() is not None:
        return
    process.send_signal(sig)
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def sched_ext_status():
    state = (SCHED_EXT_SYSFS / "state").read_text().strip()
    switch_all = (SCHED_EXT_SYSFS / "switch_all").read_text().strip()
    ops_paths = list(SCHED_EXT_SYSFS.glob("*/ops"))
    ops = ops_paths[0].read_text().strip() if ops_paths else ""
    return {"state": state, "switch_all": switch_all, "ops": ops}


def require_active_scheduler():
    status = sched_ext_status()
    if status != {"state": "enabled", "switch_all": "1", "ops": "dt_control"}:
        raise RuntimeError(f"sched_ext is not active for all normal tasks: {status}")
    return status


def parse_controller_stats(log_path):
    """dt_controller prints: BPF enqueue=.. placed=.. running=.. wrong_cpu=.."""
    text = log_path.read_text()
    matches = re.findall(
        r"BPF enqueue=(\d+) placed=(\d+) running=(\d+) wrong_cpu=(\d+) stale_identity=(\d+)",
        text,
    )
    if not matches:
        raise RuntimeError("controller did not print BPF counters")
    enqueue, placed, running, wrong, stale = (int(v) for v in matches[-1])
    if wrong:
        raise RuntimeError(f"wrong_cpu={wrong} (must be 0)")
    return {
        "enqueue": enqueue,
        "placed": placed,
        "running": running,
        "wrong_cpu": wrong,
        "stale_identity": stale,
    }


def start_server(args, model, port, parallel, log_path, dynamic_socket="", cpu_list=""):
    command = [
        str(args.server),
        "-m", str(args.model),
        "--host", "127.0.0.1",
        "--port", str(port),
        "-ngl", "0",
        "-t", str(args.threads),
        "-tb", str(args.threads),
        "-c", str(args.context),
        "-np", str(parallel),
        "--cache-ram", "0",
    ]
    if cpu_list:
        command = ["taskset", "--cpu-list", cpu_list, *command]
    log = log_path.open("w")
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(args.libdir) + ":" + env.get("LD_LIBRARY_PATH", "")
    if dynamic_socket:
        env["GGML_DYNAMIC_THREADS_SOCKET"] = dynamic_socket
    process = subprocess.Popen(
        command,
        stdout=log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
        env=env,
    )
    process.log_file = log
    process.model_name = model
    return process


def warmup(args, servers, parallel):
    payload = {
        "prompt": "dual model deterministic warmup",
        "n_predict": args.warmup_tokens,
        "temperature": 0,
        "top_k": 1,
        "top_p": 1.0,
        "seed": args.seed,
        "ignore_eos": True,
        "cache_prompt": False,
        "stream": False,
    }

    def one(url):
        response = http_json(f"{url}/completion", payload, args.server_start_timeout)
        if int(response.get("tokens_predicted", 0)) != args.warmup_tokens:
            raise RuntimeError(f"incomplete warmup at {url}")

    with concurrent.futures.ThreadPoolExecutor(max_workers=2 * parallel) as pool:
        futures = [
            pool.submit(one, url)
            for url, _ in servers
            for _slot in range(parallel)
        ]
        for future in futures:
            future.result()
    time.sleep(args.quiet_sec)


def workload_command(args, run_dir, parallel):
    return [
        sys.executable,
        str(SCRIPT_DIR / "workload.py"),
        "--scenario", "asynchronous",
        "--incumbent-url", f"http://127.0.0.1:{args.port_a}",
        "--injected-url", f"http://127.0.0.1:{args.port_b}",
        "--incumbent-model", "A",
        "--injected-model", "B",
        "--prompts", str(args.prompts),
        "--request-count", str(parallel),
        "--events", str(run_dir / "events.jsonl"),
        "--output", str(run_dir / "workload.json"),
        "--output-tokens", str(args.output_tokens),
        "--injection-token", str(args.injection_token),
        "--arrival-gap-sec", str(args.arrival_gap_sec),
        "--lead-sec", str(args.lead_sec),
        "--seed", str(args.seed),
        "--request-timeout", str(args.request_timeout),
        "--milestone-timeout", str(args.milestone_timeout),
    ]


def calculate_metrics(workload):
    sys.path.insert(0, str(SCRIPT_DIR))
    try:
        from metrics import calculate_run_metrics
        return calculate_run_metrics(workload)
    finally:
        sys.path.pop(0)


def run_one(args, suite_dir, concurrency, mode, loop):
    parallel = concurrency // 2
    run_dir = suite_dir / f"c{concurrency}_{mode}_run{loop:02d}"
    run_dir.mkdir()
    server_a = server_b = controller = None
    controller_stats = None
    started_ns = time.time_ns()
    print(f"[INFO] starting c={concurrency} mode={mode} loop={loop}", flush=True)
    dynamic = mode in {"dynamic", "dynamic-phase"}
    # AF_UNIX sun_path 上限 108，结果目录较长，socket 放 /tmp 短路径。
    socket_path = f"/tmp/dt-x86-{os.getpid()}-c{concurrency}-{mode}-{loop}.sock"
    try:
        if dynamic:
            controller_log = (run_dir / "controller.log").open("w")
            controller = subprocess.Popen(
                [
                    str(args.controller),
                    "--socket", socket_path,
                    "--mode", "dynamic",
                    "--scx",
                    "--grant-lifetime", "persistent",
                    "--allocation-policy", "prefill-first" if mode == "dynamic-phase" else "equal",
                    "--decode-wait-ms", "0",
                    *(["--decode-floor-on-prefill"] if mode == "dynamic-phase" else []),
                    "--events", str(run_dir / "controller-events.csv"),
                ],
                stdout=controller_log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            controller.log_file = controller_log
            time.sleep(1)
            if controller.poll() is not None:
                raise RuntimeError("dt_controller failed to start")
            require_active_scheduler()

        dynamic_socket = socket_path if dynamic else ""
        server_a = start_server(
            args, "A", args.port_a, parallel, run_dir / "server_a.log",
            dynamic_socket, args.cpu_list if args.overload else "",
        )
        wait_health(f"http://127.0.0.1:{args.port_a}", server_a, args.server_start_timeout)
        server_b = start_server(
            args, "B", args.port_b, parallel, run_dir / "server_b.log",
            dynamic_socket, args.cpu_list if args.overload else "",
        )
        wait_health(f"http://127.0.0.1:{args.port_b}", server_b, args.server_start_timeout)
        servers = [
            (f"http://127.0.0.1:{args.port_a}", server_a),
            (f"http://127.0.0.1:{args.port_b}", server_b),
        ]

        warmup(args, servers, parallel)

        completed = subprocess.run(workload_command(args, run_dir, parallel))
        if completed.returncode:
            raise RuntimeError(f"workload exited with status {completed.returncode}")
        if controller is not None and controller.poll() is not None:
            raise RuntimeError("dt_controller exited during workload")
        if controller is not None:
            require_active_scheduler()
            stop_process(controller, signal.SIGINT, timeout=15)
            controller.log_file.flush()
            controller_stats = parse_controller_stats(run_dir / "controller.log")
        workload = json.loads((run_dir / "workload.json").read_text())
        metrics = calculate_metrics(workload)
        (run_dir / "run_metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
        metadata = {
            "valid": bool(workload.get("valid")),
            "mode": mode,
            "concurrency": concurrency,
            "requests_per_model": parallel,
            "threads_per_server": args.threads,
            "server_pids": {"A": server_a.pid, "B": server_b.pid},
            "started_wall_ns": started_ns,
            "finished_wall_ns": time.time_ns(),
        }
        if controller_stats is not None:
            metadata["sched_ext"] = {"validated": True, "stats": controller_stats}
        (run_dir / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
        if not metadata["valid"]:
            raise RuntimeError("workload validity checks failed")
        print(
            f"[RESULT] c={concurrency} {mode}: "
            f"TPS={metrics['aggregate_tps']:.4f}, "
            f"injected TTFT={metrics['injected_ttft_mean_ms']/1000:.3f}s",
            flush=True,
        )
        return metrics
    finally:
        stop_process(controller, signal.SIGINT, timeout=15)
        stop_process(server_b)
        stop_process(server_a)
        for process in (controller, server_b, server_a):
            if process is not None and hasattr(process, "log_file"):
                process.log_file.close()


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, default=DEFAULT_SERVER)
    parser.add_argument("--libdir", type=Path, default=DEFAULT_LIBDIR)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--controller", type=Path, default=DEFAULT_CONTROLLER)
    parser.add_argument("--prompts", type=Path, default=SCRIPT_DIR / "prompts.json")
    parser.add_argument("--results-dir", type=Path, default=SCRIPT_DIR / "results")
    parser.add_argument("--concurrency", type=int, choices=(2, 4, 8), action="append")
    parser.add_argument(
        "--mode",
        choices=("baseline", "dynamic", "dynamic-phase"),
        action="append",
    )
    parser.add_argument("--loops", type=int, default=1)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--cpu-list", default="0-3")
    parser.add_argument("--overload", action="store_true",
                        help="pin both servers to --cpu-list (oversubscription scenario)")
    parser.add_argument("--context", type=int, default=4096)
    parser.add_argument("--output-tokens", type=int, default=256)
    parser.add_argument("--injection-token", type=int, default=4)
    parser.add_argument("--arrival-gap-sec", type=float, default=1.0)
    parser.add_argument("--lead-sec", type=float, default=1.0)
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--warmup-tokens", type=int, default=2)
    parser.add_argument("--quiet-sec", type=float, default=5.0)
    parser.add_argument("--request-timeout", type=float, default=1800.0)
    parser.add_argument("--milestone-timeout", type=float, default=600.0)
    parser.add_argument("--server-start-timeout", type=float, default=180.0)
    parser.add_argument("--port-a", type=int, default=18080)
    parser.add_argument("--port-b", type=int, default=18081)
    args = parser.parse_args()
    args.concurrency = args.concurrency or [2, 4, 8]
    args.mode = args.mode or ["baseline", "dynamic-phase"]
    for path in (args.server, args.model, args.controller, args.prompts):
        if not path.exists():
            parser.error(f"missing required path: {path}")
    if args.loops < 1 or args.threads < 1:
        parser.error("loops and threads must be positive")
    if args.output_tokens <= args.injection_token:
        parser.error("output-tokens must be greater than injection-token")
    if any(mode in {"dynamic", "dynamic-phase"} for mode in args.mode) and os.geteuid() != 0:
        parser.error("dynamic modes must run as root (use sudo -n)")
    return args


def main():
    args = parse_args()
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    suite_dir = args.results_dir / f"{timestamp}_x86_dynamic_dual"
    suite_dir.mkdir(parents=True)
    config = {
        key: str(value) if isinstance(value, Path) else value
        for key, value in vars(args).items()
    }
    (suite_dir / "config.json").write_text(json.dumps(config, indent=2) + "\n")
    rows = []
    for concurrency in args.concurrency:
        for loop in range(1, args.loops + 1):
            for mode in args.mode:
                metrics = run_one(args, suite_dir, concurrency, mode, loop)
                rows.append(
                    {
                        "concurrency": concurrency,
                        "loop": loop,
                        "mode": mode,
                        "aggregate_tps": metrics["aggregate_tps"],
                        "injected_ttft_mean_ms": metrics["injected_ttft_mean_ms"],
                        "injected_ttft_max_ms": metrics["injected_ttft_max_ms"],
                        "ttft_p95_ms": metrics.get("ttft_p95_ms"),
                        "itl_p95_ms": metrics.get("itl_p95_ms"),
                        "token_rate_jain": metrics["token_rate_jain"],
                        "makespan_sec": metrics["makespan_sec"],
                    }
                )
                (suite_dir / "summary.json").write_text(json.dumps(rows, indent=2) + "\n")
    print(f"[INFO] results: {suite_dir}")


if __name__ == "__main__":
    main()
