#!/usr/bin/env python3
"""Run the dynamic-thread ablation matrix with the patched llama runtime."""

import argparse
import concurrent.futures
import csv
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import random
import re
import shlex
import statistics
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import uuid

from control_evidence import validate_cpu_ownership

HERE = Path(__file__).resolve().parent
SRC = HERE.parent.parent / "src" / "dynamic_threads"
BUILD = SRC / "build"
LLAMA_SRC = SRC / "llama"
LLAMA_BIN = Path(
    os.environ.get("DT_LLAMA_BIN", str(SRC / "llama" / "build-dynamic-riscv" / "bin"))
)
BENCH = HERE.parent / "benchmark" / "dual_model"
WORKLOAD = BENCH / "workload.py"
PROMPTS = BENCH / "prompts.json"
sys.path.insert(0, str(WORKLOAD.parent))
from metrics import calculate_run_metrics  # noqa: E402
from scheduler_stats import validate_scheduler_log  # noqa: E402

DEFAULT_CASES = (
    "cfs_baseline",
    "scx_minimal",
    "scx_fixed8",
    "scx_fixed4",
    "scx_split4",
    "scx_dynamic",
    "cfs_dynamic",
)
ALL_CASES = DEFAULT_CASES + (
    "scx_dynamic_persistent",
    "cfs_dynamic_persistent",
    "scx_phase_dirty_lease",
    "scx_dynamic_persistent_phase",
    "scx_dynamic_persistent_phase_guard",
    "scx_dynamic_persistent_phase_reserve1",
    "scx_dynamic_persistent_phase_reserve1_full_release",
)
PHASE_CASES = {
    "scx_dynamic_persistent_phase",
    "scx_dynamic_persistent_phase_guard",
    "scx_dynamic_persistent_phase_reserve1",
    "scx_dynamic_persistent_phase_reserve1_full_release",
}
CASE_CONFIG = {
    "cfs_baseline": (None, False, False),
    "scx_minimal": ("fixed8", True, False),
    "scx_fixed8": ("fixed8", True, True),
    "scx_fixed4": ("fixed4", True, True),
    "scx_split4": ("split4", True, True),
    "scx_dynamic": ("dynamic", True, True),
    "cfs_dynamic": ("dynamic", False, True),
    "scx_dynamic_persistent": ("dynamic", True, True),
    "cfs_dynamic_persistent": ("dynamic", False, True),
    "scx_phase_dirty_lease": (None, False, False),
    "scx_dynamic_persistent_phase": ("dynamic", True, True),
    "scx_dynamic_persistent_phase_guard": ("dynamic", True, True),
    "scx_dynamic_persistent_phase_reserve1": ("dynamic", True, True),
    "scx_dynamic_persistent_phase_reserve1_full_release": ("dynamic", True, True),
}


def percentile(values, fraction):
    if not values:
        return None
    values = sorted(values)
    return values[min(len(values) - 1, math.ceil(fraction * len(values)) - 1)]


def event_metrics(rows):
    requests = {
        (row["slot"], row["epoch"]): int(row["time_ns"])
        for row in rows
        if row["event"] == "REQUEST"
    }
    grants = {
        (row["slot"], row["epoch"]): int(row["time_ns"])
        for row in rows
        if row["event"] == "GRANT"
    }
    members = {}
    for row in rows:
        if row["event"] == "MEMBERS":
            members.setdefault((row["slot"], row["epoch"]), int(row["time_ns"]))
    # Partial returns keep the epoch. Match each revoke in event order, not
    # once per epoch (which would discard repeated shrink handoffs).
    revoked, handoffs = {}, []
    for row in rows:
        key = row["slot"], row["epoch"]
        if row["event"] == "REVOKE":
            revoked[key] = int(row["time_ns"])
        elif row["event"] in ("RELEASE", "SHRINK") and key in revoked:
            handoffs.append((int(row["time_ns"]) - revoked.pop(key)) / 1000)
        elif row["event"] == "EXIT":
            revoked.pop(key, None)
    waits = [
        (grants[key] - value) / 1000 for key, value in requests.items() if key in grants
    ]
    startups = [
        (members[key] - value) / 1000 for key, value in grants.items() if key in members
    ]
    grant_rows = [row for row in rows if row["event"] == "GRANT"]
    allocation_rows = [row for row in rows if row["event"] in ("GRANT", "SHRINK")]
    decode_waits = [
        int(row.get("decode_wait_ns", 0)) / 1e6
        for row in rows
        if row["event"] == "DECODE_RESUME"
    ]
    observed_waits = [
        int(row["decode_wait_ns"]) / 1e6 for row in rows if "decode_wait_ns" in row
    ]
    recoveries = [
        int(row.get("decode_recovery_ns", 0)) / 1e6
        for row in rows
        if row["event"] == "DECODE_RESUME" and int(row.get("decode_recovery_ns", 0))
    ]
    return {
        "grants": len(grant_rows),
        "shrinks": len(allocation_rows) - len(grant_rows),
        "workers": sorted({int(row["workers"]) for row in allocation_rows}),
        "protocol_messages": sum(
            row["event"] in ("WIRE_RX", "WIRE_TX") for row in rows
        ),
        "member_updates": sum(row["event"] == "MEMBERS" for row in rows),
        "revocations": sum(row["event"] == "REVOKE" for row in rows),
        "phase_updates": sum(row["event"] == "PHASE" for row in rows),
        "decode_protections": sum(row["event"] == "DECODE_PROTECT" for row in rows),
        "decode_wait_p95_ms": percentile(decode_waits, 0.95),
        "decode_wait_max_ms": max(observed_waits, default=None),
        "decode_over_budget_max_ms": max(
            (
                int(row["decode_over_budget_ns"]) / 1e6
                for row in rows
                if "decode_over_budget_ns" in row
            ),
            default=None,
        ),
        "decode_recovery_p95_ms": percentile(recoveries, 0.95),
        "decode_recovery_max_ms": max(recoveries, default=None),
        "decode_pauses": sum(
            row["event"] == "TARGET"
            and row.get("phase") == "2"
            and row.get("target_workers") == "0"
            for row in rows
        ),
        "decode_floor_targets": sum(
            row["event"] == "TARGET"
            and row.get("phase") == "2"
            and row.get("target_workers") == "1"
            for row in rows
        ),
        "handoff_mean_us": statistics.fmean(handoffs) if handoffs else None,
        "handoff_p95_us": percentile(handoffs, 0.95),
        "cpu_masks": sorted({int(row["cpu_mask"]) for row in allocation_rows}),
        "grant_wait_mean_us": statistics.fmean(waits) if waits else None,
        "grant_wait_p95_us": percentile(waits, 0.95),
        "team_publish_mean_us": statistics.fmean(startups) if startups else None,
        "team_publish_p95_us": percentile(startups, 0.95),
    }


def calculate_metrics(workload):
    metrics = calculate_run_metrics(workload)
    ttft = []
    itl = []
    incumbent_itl = []
    for request in workload.get("requests", []):
        if request.get("first_token_ns") is not None:
            ttft.append((request["first_token_ns"] - request["send_ns"]) / 1e6)
        times = request.get("token_times_ns", [])
        gaps = [(right - left) / 1e6 for left, right in zip(times, times[1:])]
        itl.extend(gaps)
        if request.get("role") == "incumbent":
            incumbent_itl.extend(gaps)
    metrics.update(
        {
            "ttft_p95_ms": percentile(ttft, 0.95),
            "itl_mean_ms": statistics.fmean(itl) if itl else None,
            "itl_p95_ms": percentile(itl, 0.95),
            "itl_p99_ms": percentile(itl, 0.99),
            "itl_max_ms": max(itl, default=None),
            "incumbent_itl_p99_ms": percentile(incumbent_itl, 0.99),
            "incumbent_itl_max_ms": max(incumbent_itl, default=None),
        }
    )
    return metrics


def aggregate_runs(runs):
    groups = {}
    metric_names = (
        "aggregate_tps",
        "ttft_mean_ms",
        "ttft_p95_ms",
        "ttft_max_ms",
        "itl_mean_ms",
        "itl_p95_ms",
        "itl_p99_ms",
        "itl_max_ms",
        "incumbent_itl_p99_ms",
        "incumbent_itl_max_ms",
        "injected_ttft_mean_ms",
        "incumbent_overlap_itl_p95_ms",
        "token_rate_jain",
        "model_e2e_tps_jain",
    )
    control_names = (
        "protocol_messages",
        "member_updates",
        "grants",
        "shrinks",
        "revocations",
        "phase_updates",
        "decode_pauses",
        "decode_floor_targets",
        "decode_protections",
        "decode_wait_p95_ms",
        "decode_wait_max_ms",
        "decode_over_budget_max_ms",
        "decode_recovery_p95_ms",
        "decode_recovery_max_ms",
        "handoff_mean_us",
        "handoff_p95_us",
        "grant_wait_mean_us",
        "grant_wait_p95_us",
        "team_publish_mean_us",
        "team_publish_p95_us",
    )
    for run in runs:
        key = (run["case"], run["scenario"], run["concurrency"])
        groups.setdefault(key, []).append(run)
    result = []
    for (case, scenario, concurrency), selected in sorted(groups.items()):
        item = {
            "case": case,
            "scenario": scenario,
            "concurrency": concurrency,
            "valid_runs": sum(bool(run["valid"]) for run in selected),
            "total_runs": len(selected),
            "metrics": {},
            "control": {},
        }
        for name in metric_names:
            values = [run["metrics"].get(name) for run in selected]
            values = [float(value) for value in values if value is not None]
            if values:
                item["metrics"][name] = {
                    "mean": statistics.fmean(values),
                    "sd": statistics.stdev(values) if len(values) > 1 else 0.0,
                    "min": min(values),
                    "max": max(values),
                }
        for name in control_names:
            values = [run.get("control", {}).get(name) for run in selected]
            values = [float(value) for value in values if value is not None]
            if values:
                item["control"][name] = {
                    "mean": statistics.fmean(values),
                    "sd": statistics.stdev(values) if len(values) > 1 else 0.0,
                    "min": min(values),
                    "max": max(values),
                }
        result.append(item)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("REMOTE_IP", "192.168.3.67"))
    parser.add_argument("--user", default=os.environ.get("REMOTE_USER", "root"))
    parser.add_argument("--password", default=os.environ.get("REMOTE_PASS", "bianbu"))
    parser.add_argument("--llm-path", default="/mnt/llm/llama")
    parser.add_argument(
        "--model", default="/mnt/llm/llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf"
    )
    parser.add_argument("--port-a", type=int, default=18080)
    parser.add_argument("--port-b", type=int, default=18081)
    parser.add_argument("--cases", default=",".join(DEFAULT_CASES))
    parser.add_argument(
        "--interleave",
        action="store_true",
        help="alternate case order each repeat; restart identical runtimes each block",
    )
    parser.add_argument("--scenarios", default="simultaneous,asynchronous")
    parser.add_argument("--concurrencies", default="2,4,8")
    parser.add_argument("--loops", type=int, default=3)
    parser.add_argument("--output-tokens", type=int, default=256)
    parser.add_argument("--injection-token", type=int, default=4)
    parser.add_argument("--seed", type=int, default=20260917)
    parser.add_argument("--server-timeout", type=float, default=180)
    parser.add_argument("--phase-scheduler", default="/tmp/scx-nteam/scx_llm_pin")
    parser.add_argument(
        "--decode-wait-ms",
        type=int,
        default=2000,
        help="guard case wait budget; strict phase case always disables the floor",
    )
    parser.add_argument(
        "--smoke",
        action="store_true",
        help="force loops=1, output-tokens=16, concurrency=2",
    )
    args = parser.parse_args()
    if not 1 <= args.decode_wait_ms <= 3600000:
        parser.error(
            "decode-wait-ms must be 1..3600000; use the strict phase case to disable"
        )
    cases = tuple(item for item in args.cases.split(",") if item)
    scenarios = tuple(item for item in args.scenarios.split(",") if item)
    try:
        concurrencies = tuple(
            int(item) for item in args.concurrencies.split(",") if item
        )
    except ValueError:
        parser.error("invalid concurrency list")
    if not cases or any(item not in ALL_CASES for item in cases):
        parser.error(f"cases must be drawn from {','.join(ALL_CASES)}")
    if not scenarios or any(
        item not in ("simultaneous", "asynchronous") for item in scenarios
    ):
        parser.error("scenarios must be simultaneous and/or asynchronous")
    if any(item not in (2, 4, 8) for item in concurrencies):
        parser.error("concurrencies must be drawn from 2,4,8")
    if args.smoke:
        args.loops, args.output_tokens, concurrencies = 1, 16, (2,)
    if args.loops < 1 or args.output_tokens <= args.injection_token:
        parser.error("invalid loops/output/injection-token")

    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    campaign_id = f"benchmark-{stamp}-{uuid.uuid4().hex[:8]}"
    output_dir = HERE / "results" / campaign_id
    output_dir.mkdir(parents=True)
    remote_dir = f"/tmp/{campaign_id}"
    target = f"{args.user}@{args.host}"
    env = os.environ.copy()
    env["SSHPASS"] = args.password
    options = [
        "-o",
        "PubkeyAuthentication=no",
        "-o",
        "ConnectTimeout=8",
        "-o",
        "StrictHostKeyChecking=accept-new",
    ]
    ssh = ["sshpass", "-e", "ssh", *options, target]
    scp = ["sshpass", "-e", "scp", "-O", *options]
    campaign = {
        "campaign_id": campaign_id,
        "complete": False,
        "host": args.host,
        "threads": 8,
        "threads_batch": 8,
        "cases": list(cases),
        "scenarios": list(scenarios),
        "concurrencies": list(concurrencies),
        "loops": args.loops,
        "output_tokens": args.output_tokens,
        "runs": [],
    }
    campaign["decode_wait_ms"] = {
        case: args.decode_wait_ms if case.endswith("_phase_guard") else 0
        for case in cases
    }
    campaign["decode_floor_on_prefill"] = {
        case: "_phase_reserve1" in case for case in cases
    }
    campaign["retain_shrink"] = {
        case: not case.endswith("_full_release") for case in cases
    }

    def execute(command, timeout=180, check=True):
        completed = subprocess.run(
            command,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
        )
        if check and completed.returncode:
            raise RuntimeError(completed.stdout.decode(errors="replace"))
        return completed

    def remote(command, timeout=180, check=True):
        return execute(ssh + [command], timeout, check).stdout.decode(errors="replace")

    def health(port):
        deadline = time.monotonic() + args.server_timeout
        while time.monotonic() < deadline:
            try:
                with urllib.request.urlopen(
                    f"http://{args.host}:{port}/health", timeout=2
                ) as response:
                    if response.status == 200:
                        return
            except Exception:
                pass
            time.sleep(0.5)
        raise RuntimeError(f"server {port} health timeout")

    def completion(port, slot):
        payload = {
            "prompt": f"benchmark warmup slot {slot}",
            "n_predict": 2,
            "temperature": 0,
            "top_k": 1,
            "top_p": 1.0,
            "seed": 1234,
            "ignore_eos": True,
            "cache_prompt": False,
            "stream": False,
        }
        request = urllib.request.Request(
            f"http://{args.host}:{port}/completion",
            data=json.dumps(payload).encode(),
            headers={"content-type": "application/json"},
        )
        with urllib.request.urlopen(request, timeout=300) as response:
            result = json.load(response)
        if result.get("tokens_predicted") != 2:
            raise RuntimeError("warmup incomplete")

    try:
        if (
            not (LLAMA_BIN / "llama-server").is_file()
            or not (BUILD / "dt_controller").is_file()
        ):
            raise RuntimeError("missing RISC-V binaries")
        require_phase = "scx_phase_dirty_lease" in cases
        phase_probe = (
            f"; test -x {shlex.quote(args.phase_scheduler)}" if require_phase else ""
        )
        preflight = remote(
            "cat /sys/kernel/debug/sched/ext; "
            f"test -f {shlex.quote(args.model)}{phase_probe}"
        )
        if not re.search(r"^enabled\s*:\s*0\s*$", preflight, re.M):
            raise RuntimeError("sched_ext is already active")
        campaign["dmesg_before"] = remote("dmesg")
        remote(f"mkdir -p {remote_dir}/bin")
        with tempfile.NamedTemporaryFile(prefix="dt-bench-", suffix=".tar") as archive:
            with tarfile.open(fileobj=archive, mode="w", dereference=False) as tar:
                for path in LLAMA_BIN.iterdir():
                    tar.add(path, arcname=path.name, recursive=False)
            archive.flush()
            execute(scp + [archive.name, f"{target}:{remote_dir}/runtime.tar"])
        execute(scp + [str(BUILD / "dt_controller"), f"{target}:{remote_dir}/"])
        remote(
            f"tar -C {remote_dir}/bin -xf {remote_dir}/runtime.tar; rm {remote_dir}/runtime.tar"
        )
        campaign["sha256"] = {
            path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (
                LLAMA_BIN / "llama-server",
                BUILD / "dt_controller",
                LLAMA_BIN / "libggml-cpu.so.0.13.0",
            )
        }
        order = list(cases)
        random.Random(args.seed).shuffle(order)
        schedule = (
            [
                (case, loop)
                for loop in range(args.loops)
                for case in (order if loop % 2 == 0 else order[::-1])
            ]
            if args.interleave
            else [(case, None) for case in order]
        )
        campaign["case_order"] = schedule
        campaign["interleave"] = args.interleave
        max_slots = max(concurrencies) // 2
        loader = f"{args.llm_path}/bin/ld-linux-riscv64-lp64d.so.1"
        libpath = f"{remote_dir}/bin:{args.llm_path}/bin"
        for case_index, (case, only_loop) in enumerate(schedule):
            mode, use_scx, interface = CASE_CONFIG[case]
            case_dir = output_dir / f"{case_index:02d}-{case}"
            case_dir.mkdir()
            prefix = f"{remote_dir}/{case_index:02d}-{case}"
            sock = f"{prefix}.sock"
            controller_pid = None
            phase_scheduler_pid = None
            server_pids = []
            try:
                if mode:
                    scx_arg = " --scx" if use_scx else ""
                    lifetime = "persistent" if "_persistent" in case else "graph"
                    allocation = "prefill-first" if case in PHASE_CASES else "equal"
                    command = (
                        f"{remote_dir}/dt_controller --socket {sock} --mode {mode}{scx_arg} "
                        f"--grant-lifetime {lifetime} "
                        f"--allocation-policy {allocation} "
                        f"--decode-wait-ms {campaign['decode_wait_ms'][case]} "
                        f"{'--decode-floor-on-prefill ' if campaign['decode_floor_on_prefill'][case] else ''}"
                        f"--events {prefix}-events.csv >{prefix}-controller.log 2>&1 </dev/null & echo $!"
                    )
                    controller_pid = int(remote(command).strip())
                    for _ in range(200):
                        if remote(f"test -S {sock}", check=False) == "":
                            probe = execute(ssh + [f"test -S {sock}"], check=False)
                            if probe.returncode == 0:
                                break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError("controller socket timeout")
                dynamic_env = (
                    f"GGML_DYNAMIC_THREADS_SOCKET={sock} " if interface else ""
                )
                if interface and not campaign["retain_shrink"][case]:
                    dynamic_env += "GGML_DYNAMIC_THREADS_RETAIN_SHRINK=0 "
                if case == "scx_phase_dirty_lease":
                    dynamic_env += "GGML_SCX_LEASE_HINT_PATH=/sys/fs/bpf/scx_rvv_dirty_lease/workers "
                common = (
                    f"env OMP_DYNAMIC=FALSE {dynamic_env}{loader} --library-path {libpath} "
                    f"{remote_dir}/bin/llama-server -m {shlex.quote(args.model)} --host 0.0.0.0 "
                    f"-ngl 0 -t 8 -tb 8 -c {max(2048, max_slots * 1024)} -np {max_slots} --cache-ram 0"
                )
                for model, port in (("a", args.port_a), ("b", args.port_b)):
                    pid = int(
                        remote(
                            f"{common} --port {port} >{prefix}-server-{model}.log 2>&1 "
                            f"</dev/null & echo $!"
                        ).strip()
                    )
                    server_pids.append(pid)
                    health(port)
                if case == "scx_phase_dirty_lease":
                    command = (
                        f"{shlex.quote(args.phase_scheduler)} "
                        f"--team {server_pids[0]}:8:8:0xff --team {server_pids[1]}:8:8:0xff "
                        "--strategy rvv_dirty --vector-slice-ms 100 --vector-local-window-ms 500 "
                        "--dirty-threshold 2 --user-control active --user-cpu-map auto "
                        "--team-gang prefill-lease --prefill-starvation-ms 1000 "
                        "--vector-local-wakeup off --phase-wakeup-preempt on "
                        f">{prefix}-controller.log 2>&1 </dev/null & echo $!"
                    )
                    phase_scheduler_pid = int(remote(command).strip())
                    for _ in range(200):
                        state = remote("cat /sys/kernel/debug/sched/ext", check=False)
                        if re.search(r"^enabled\s*:\s*1\s*$", state, re.M):
                            break
                        if remote(f"kill -0 {phase_scheduler_pid}", check=False) != "":
                            raise RuntimeError(
                                "phase dirty lease scheduler exited during attach"
                            )
                        time.sleep(0.05)
                    else:
                        raise RuntimeError("phase dirty lease scheduler attach timeout")
                # Exercise every configured slot before measurements.
                for port in (args.port_a, args.port_b):
                    with concurrent.futures.ThreadPoolExecutor(
                        max_workers=max_slots
                    ) as pool:
                        list(
                            pool.map(
                                lambda slot: completion(port, slot), range(max_slots)
                            )
                        )
                time.sleep(2)
                run_specs = [
                    (scenario, concurrency, loop)
                    for scenario in scenarios
                    for concurrency in concurrencies
                    for loop in (
                        [only_loop] if only_loop is not None else range(args.loops)
                    )
                ]
                random.Random(args.seed + case_index).shuffle(run_specs)
                for run_index, (scenario, concurrency, loop) in enumerate(run_specs):
                    run_dir = (
                        case_dir / f"{run_index:02d}-{scenario}-c{concurrency}-r{loop}"
                    )
                    run_dir.mkdir()
                    before_lines = (
                        int(remote(f"wc -l < {prefix}-events.csv").strip())
                        if mode
                        else 0
                    )
                    command = [
                        sys.executable,
                        str(WORKLOAD),
                        "--scenario",
                        scenario,
                        "--incumbent-url",
                        f"http://{args.host}:{args.port_a}",
                        "--injected-url",
                        f"http://{args.host}:{args.port_b}",
                        "--incumbent-model",
                        "A",
                        "--injected-model",
                        "B",
                        "--prompts",
                        str(PROMPTS),
                        "--request-count",
                        str(concurrency // 2),
                        "--events",
                        str(run_dir / "events.jsonl"),
                        "--output",
                        str(run_dir / "workload.json"),
                        "--output-tokens",
                        str(args.output_tokens),
                        "--injection-token",
                        str(args.injection_token),
                        "--request-timeout",
                        "1800",
                        "--milestone-timeout",
                        "600",
                    ]
                    completed = execute(command, timeout=2000, check=False)
                    (run_dir / "workload.log").write_bytes(completed.stdout)
                    after_lines = (
                        int(remote(f"wc -l < {prefix}-events.csv").strip())
                        if mode
                        else 0
                    )
                    workload = json.loads((run_dir / "workload.json").read_text())
                    item = {
                        "case": case,
                        "case_index": case_index,
                        "scenario": scenario,
                        "concurrency": concurrency,
                        "loop": loop,
                        "valid": completed.returncode == 0
                        and workload.get("valid") is True,
                        "decode_wait_ms": campaign["decode_wait_ms"][case],
                        "event_line_start": before_lines,
                        "event_line_end": after_lines,
                        "metrics": calculate_metrics(workload),
                        "path": str(run_dir.relative_to(output_dir)),
                    }
                    (run_dir / "summary.json").write_text(
                        json.dumps(item, indent=2) + "\n"
                    )
                    campaign["runs"].append(item)
                    (output_dir / "campaign.json").write_text(
                        json.dumps(campaign, indent=2) + "\n"
                    )
                    if not item["valid"]:
                        raise RuntimeError(
                            f"invalid workload: {case}/{scenario}/c{concurrency}/r{loop}"
                        )
                    time.sleep(2)
            finally:
                # The dirty-lease scheduler's final machine-readable line must be
                # captured while both registered teams still exist.  Stopping the
                # servers first makes that last line report rvv_team0/1=0 even
                # though the measured workload ran with 8+8 workers.
                if phase_scheduler_pid:
                    remote(
                        f"kill -TERM {phase_scheduler_pid} 2>/dev/null || true; n=0; "
                        f"while kill -0 {phase_scheduler_pid} 2>/dev/null; do n=$((n+1)); "
                        f"[ $n -lt 100 ] || {{ kill -KILL {phase_scheduler_pid}; break; }}; sleep .1; done",
                        30,
                        False,
                    )
                for pid in server_pids:
                    remote(f"kill -TERM {pid} 2>/dev/null || true", check=False)
                for pid in server_pids:
                    remote(
                        f"n=0; while kill -0 {pid} 2>/dev/null; do n=$((n+1)); "
                        f"[ $n -lt 100 ] || {{ kill -KILL {pid}; break; }}; sleep .1; done",
                        30,
                        False,
                    )
                if controller_pid:
                    remote(
                        f"kill -TERM {controller_pid} 2>/dev/null || true; n=0; "
                        f"while kill -0 {controller_pid} 2>/dev/null; do n=$((n+1)); "
                        f"[ $n -lt 100 ] || {{ kill -KILL {controller_pid}; break; }}; sleep .1; done",
                        30,
                        False,
                    )
                for remote_name, local_name in (
                    (f"{prefix}-events.csv", "controller-events.csv"),
                    (f"{prefix}-controller.log", "controller.log"),
                    (f"{prefix}-server-a.log", "server-a.log"),
                    (f"{prefix}-server-b.log", "server-b.log"),
                ):
                    execute(
                        scp + [f"{target}:{remote_name}", str(case_dir / local_name)],
                        60,
                        False,
                    )
            if mode:
                log = (case_dir / "controller.log").read_text(errors="replace")
                match = re.search(
                    r"BPF enqueue=(\d+) placed=(\d+) running=(\d+) wrong_cpu=(\d+)", log
                )
                if use_scx and (not match or int(match.group(4))):
                    raise RuntimeError(f"{case}: BPF placement validation failed")
                if case in (
                    "scx_split4",
                    "scx_dynamic",
                    "scx_dynamic_persistent",
                    *PHASE_CASES,
                ) and (int(match.group(2)) == 0 or int(match.group(3)) == 0):
                    raise RuntimeError(f"{case}: BPF did not place granted workers")
                if "status=OK" not in log:
                    raise RuntimeError(f"{case}: controller failed")
                with (case_dir / "controller-events.csv").open(newline="") as stream:
                    rows = list(csv.DictReader(stream))
                if "_persistent" in case:
                    validate_cpu_ownership(rows)
                for item in campaign["runs"]:
                    if item["case_index"] != case_index:
                        continue
                    start, end = (
                        item["event_line_start"] - 1,
                        item["event_line_end"] - 1,
                    )
                    item["control"] = event_metrics(rows[max(0, start) : max(0, end)])
                    if case in PHASE_CASES:
                        if not item["control"]["phase_updates"]:
                            raise RuntimeError("phase-aware case has no stage updates")
                        if (
                            item["scenario"] == "asynchronous"
                            and not campaign["decode_floor_on_prefill"][case]
                            and not item["control"]["decode_pauses"]
                        ):
                            raise RuntimeError("injected prefill did not pause decode")
                        if (
                            item["scenario"] == "asynchronous"
                            and campaign["decode_floor_on_prefill"][case]
                            and not item["control"]["decode_floor_targets"]
                        ):
                            raise RuntimeError(
                                "reserve1 case never assigned the decode floor"
                            )
                    if interface:
                        expected_workers = {8} if mode == "fixed8" else {4}
                        if not expected_workers.issubset(
                            set(item["control"]["workers"])
                        ):
                            raise RuntimeError(
                                f"{case}: missing expected worker grant in {item['path']}"
                            )
                        if mode == "split4" and not {15, 240}.issubset(
                            set(item["control"]["cpu_masks"])
                        ):
                            raise RuntimeError(
                                f"{case}: missing split CPU masks in {item['path']}"
                            )
                        if (
                            mode == "dynamic"
                            and "_persistent" not in case
                            and item["scenario"] == "simultaneous"
                            and not {15, 240}.issubset(
                                set(item["control"]["cpu_masks"])
                            )
                        ):
                            raise RuntimeError(
                                f"{case}: missing dynamic split masks in {item['path']}"
                            )
                    run_dir = output_dir / item["path"]
                    (run_dir / "summary.json").write_text(
                        json.dumps(item, indent=2) + "\n"
                    )
            elif case == "scx_phase_dirty_lease":
                diagnostics = validate_scheduler_log(
                    case_dir / "controller.log", "phase-lease"
                )
                if not diagnostics["valid"]:
                    raise RuntimeError(
                        f"phase dirty lease validation failed: {diagnostics['checks']}"
                    )
                for item in campaign["runs"]:
                    if item["case_index"] != case_index:
                        continue
                    item["scheduler"] = diagnostics
                    run_dir = output_dir / item["path"]
                    (run_dir / "summary.json").write_text(
                        json.dumps(item, indent=2) + "\n"
                    )
        after = remote("dmesg")
        before = campaign.pop("dmesg_before")
        if not after.startswith(before):
            raise RuntimeError("dmesg history changed or wrapped")
        delta = after[len(before) :]
        (output_dir / "dmesg-delta.log").write_text(delta)
        if re.search(
            r"BUG:|WARNING:|Oops:|watchdog.*(stall|lockup)|sched_ext:.*(error|stall)|hung task",
            delta,
            re.I,
        ):
            raise RuntimeError("new kernel warning/error")
        state = remote("cat /sys/kernel/debug/sched/ext")
        if not re.search(r"^enabled\s*:\s*0\s*$", state, re.M):
            raise RuntimeError("scheduler remained attached")
        campaign["aggregate"] = aggregate_runs(campaign["runs"])
        campaign["complete"] = True
    except (
        OSError,
        subprocess.SubprocessError,
        RuntimeError,
        json.JSONDecodeError,
    ) as exc:
        campaign["failure"] = f"{type(exc).__name__}: {exc}"
    (output_dir / "campaign.json").write_text(json.dumps(campaign, indent=2) + "\n")
    print(json.dumps(campaign, indent=2))
    print(f"Evidence: {output_dir}")
    return 0 if campaign["complete"] else 1


if __name__ == "__main__":
    os.chdir(HERE)
    sys.exit(main())
