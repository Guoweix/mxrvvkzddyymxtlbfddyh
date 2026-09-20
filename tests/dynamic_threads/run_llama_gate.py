#!/usr/bin/env python3
"""Validate dynamic worker grants with two real llama-server processes on the board."""

import argparse
import concurrent.futures
import csv
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
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
LLAMA_BIN = Path(
    os.environ.get("DT_LLAMA_BIN", str(SRC / "llama" / "build-dynamic-riscv" / "bin"))
)
MODEL_DEFAULT = "/mnt/llm/llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("REMOTE_IP", "192.168.3.67"))
    parser.add_argument("--user", default=os.environ.get("REMOTE_USER", "root"))
    parser.add_argument("--password", default=os.environ.get("REMOTE_PASS", "bianbu"))
    parser.add_argument("--llm-path", default="/mnt/llm/llama")
    parser.add_argument("--model", default=MODEL_DEFAULT)
    parser.add_argument("--port-a", type=int, default=18080)
    parser.add_argument("--port-b", type=int, default=18081)
    parser.add_argument("--output-tokens", type=int, default=16)
    parser.add_argument("--server-timeout", type=float, default=180)
    parser.add_argument(
        "--grant-lifetime", choices=("graph", "persistent"), default="graph"
    )
    parser.add_argument(
        "--allocation-policy", choices=("equal", "prefill-first"), default="equal"
    )
    parser.add_argument("--decode-wait-ms", type=int, default=0)
    args = parser.parse_args()
    if args.allocation_policy != "equal" and args.grant_lifetime != "persistent":
        parser.error("prefill-first requires persistent")
    if not 0 <= args.decode_wait_ms <= 3600000:
        parser.error("decode-wait-ms must be 0..3600000")
    if args.decode_wait_ms and args.allocation_policy != "prefill-first":
        parser.error("decode protection requires prefill-first")
    if args.port_a == args.port_b or not all(
        1024 <= p <= 65535 for p in (args.port_a, args.port_b)
    ):
        parser.error("ports must be distinct and between 1024 and 65535")
    if not 4 <= args.output_tokens <= 256:
        parser.error("output-tokens must be 4..256")

    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_id = f"llama-{stamp}-{uuid.uuid4().hex[:8]}"
    result_dir = HERE / "results" / run_id
    result_dir.mkdir(parents=True)
    remote_dir = f"/tmp/{run_id}"
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
    report = {
        "gate_passed": False,
        "run_id": run_id,
        "host": args.host,
        "remote_dir": remote_dir,
        "threads": 8,
        "threads_batch": 8,
        "output_tokens": args.output_tokens,
    }
    report["grant_lifetime"] = args.grant_lifetime
    report["allocation_policy"] = args.allocation_policy
    report["decode_wait_ms"] = args.decode_wait_ms
    before = None
    controller_started = False

    def run(command, name, timeout=120, check=True):
        completed = subprocess.run(
            command,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
        )
        (result_dir / name).write_bytes(completed.stdout)
        if check and completed.returncode:
            raise RuntimeError(f"{name}: exit {completed.returncode}")
        return completed.stdout.decode(errors="replace")

    def remote(command, name, timeout=120, check=True):
        return run(ssh + [command], name, timeout, check)

    def health(port):
        deadline = time.monotonic() + args.server_timeout
        last_error = None
        while time.monotonic() < deadline:
            try:
                with urllib.request.urlopen(
                    f"http://{args.host}:{port}/health", timeout=2
                ) as response:
                    if response.status == 200:
                        return
            except Exception as exc:  # health can refuse connections during model load
                last_error = exc
            time.sleep(0.5)
        raise RuntimeError(f"server port {port} did not become healthy: {last_error}")

    def completion(port, prompt):
        payload = {
            "prompt": prompt,
            "n_predict": args.output_tokens,
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
        start = time.monotonic()
        with urllib.request.urlopen(request, timeout=600) as response:
            body = json.load(response)
        elapsed = time.monotonic() - start
        if body.get("tokens_predicted") != args.output_tokens:
            raise RuntimeError(
                f"port {port}: incomplete response {body.get('tokens_predicted')}"
            )
        return {
            "port": port,
            "elapsed_sec": elapsed,
            "tokens_predicted": body["tokens_predicted"],
            "timings": body.get("timings"),
            "content_sha256": hashlib.sha256(
                body.get("content", "").encode()
            ).hexdigest(),
        }

    try:
        required = [LLAMA_BIN / "llama-server", BUILD / "dt_controller"]
        if not all(path.is_file() for path in required):
            raise RuntimeError(
                "missing RISC-V build; build llama-server and make all first"
            )
        preflight = remote(
            "set -eu; cat /sys/devices/system/cpu/online; cat /sys/kernel/debug/sched/ext; "
            f"test -f {shlex.quote(args.model)}; "
            f"for p in {args.port_a} {args.port_b}; do "
            "h=$(printf '%04X' $p); ! grep -qi \":$h .* 0A\" /proc/net/tcp /proc/net/tcp6; done",
            "preflight.log",
        )
        if not re.search(r"^enabled\s*:\s*0\s*$", preflight, re.M):
            raise RuntimeError("sched_ext already enabled; board left untouched")
        before = remote("dmesg", "dmesg-before.log")
        remote(f"mkdir -p {shlex.quote(remote_dir)}/bin", "deploy-mkdir.log")
        with tempfile.NamedTemporaryFile(
            prefix="dt-runtime-", suffix=".tar"
        ) as archive:
            with tarfile.open(fileobj=archive, mode="w", dereference=False) as tar:
                for path in LLAMA_BIN.iterdir():
                    tar.add(path, arcname=path.name, recursive=False)
            archive.flush()
            run(
                scp + [archive.name, f"{target}:{remote_dir}/runtime.tar"],
                "deploy-runtime.log",
            )
        run(
            scp + [str(BUILD / "dt_controller"), f"{target}:{remote_dir}/"],
            "deploy-controller.log",
        )
        remote(
            f"tar -C {remote_dir}/bin -xf {remote_dir}/runtime.tar; rm {remote_dir}/runtime.tar",
            "deploy-extract.log",
        )
        binaries = [
            BUILD / "dt_controller",
            LLAMA_BIN / "llama-server",
            LLAMA_BIN / "libggml-cpu.so.0.13.0",
        ]
        report["sha256"] = {
            path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in binaries
        }

        loader = f"{args.llm_path}/bin/ld-linux-riscv64-lp64d.so.1"
        library_path = f"{remote_dir}/bin:{args.llm_path}/bin"
        common = (
            f"env OMP_DYNAMIC=FALSE GGML_DYNAMIC_THREADS_SOCKET={remote_dir}/control.sock "
            f"{loader} --library-path {library_path} {remote_dir}/bin/llama-server "
            f"-m {shlex.quote(args.model)} --host 0.0.0.0 -ngl 0 -t 8 -tb 8 "
            "-c 2048 -np 1 --cache-ram 0"
        )
        start = "\n".join(
            [
                "set -eu",
                f"r={remote_dir}",
                f"$r/dt_controller --socket $r/control.sock --mode dynamic --scx --grant-lifetime {args.grant_lifetime} "
                f"--allocation-policy {args.allocation_policy} "
                f"--decode-wait-ms {args.decode_wait_ms} "
                "--events $r/events.csv >$r/controller.log 2>&1 </dev/null & echo $! >$r/controller.pid",
                "n=0; while [ ! -S $r/control.sock ]; do n=$((n+1)); "
                "[ $n -lt 200 ] || { cat $r/controller.log; exit 1; }; sleep .05; done",
                f"{common} --port {args.port_a} >$r/server-a.log 2>&1 </dev/null & echo $! >$r/server-a.pid",
            ]
        )
        remote(start, "start-a.log")
        controller_started = True
        health(args.port_a)
        report["single"] = completion(args.port_a, "Dynamic thread single-server gate.")
        remote(
            f"r={remote_dir}; {common} --port {args.port_b} "
            ">$r/server-b.log 2>&1 </dev/null & echo $! >$r/server-b.pid",
            "start-b.log",
        )
        health(args.port_b)
        prompt = "Dynamic thread dual-server deterministic gate."
        dual_start = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures = [
                pool.submit(completion, port, prompt)
                for port in (args.port_a, args.port_b)
            ]
            dual = [future.result() for future in futures]
        report["dual"] = {
            "wall_sec": time.monotonic() - dual_start,
            "responses": dual,
            "outputs_match": dual[0]["content_sha256"] == dual[1]["content_sha256"],
        }
        if not report["dual"]["outputs_match"]:
            raise RuntimeError("dual deterministic outputs differ")
    except (OSError, subprocess.SubprocessError, RuntimeError) as exc:
        report["failure"] = str(exc)
    finally:
        if controller_started:
            stop = "\n".join(
                [
                    "set +e",
                    f"r={remote_dir}",
                    "for f in server-a.pid server-b.pid; do [ -f $r/$f ] || continue; "
                    "p=$(cat $r/$f); kill -TERM $p 2>/dev/null; done",
                    "for f in server-a.pid server-b.pid; do [ -f $r/$f ] || continue; "
                    "p=$(cat $r/$f); n=0; while kill -0 $p 2>/dev/null; do n=$((n+1)); "
                    "[ $n -lt 100 ] || { kill -KILL $p 2>/dev/null; break; }; sleep .1; done; done",
                    "p=$(cat $r/controller.pid); kill -TERM $p 2>/dev/null; n=0; "
                    "while kill -0 $p 2>/dev/null; do n=$((n+1)); "
                    "[ $n -lt 100 ] || { kill -KILL $p 2>/dev/null; exit 1; }; sleep .1; done",
                ]
            )
            try:
                remote(stop, "cleanup.log", 30)
            except (OSError, subprocess.SubprocessError, RuntimeError) as exc:
                report["cleanup_failure"] = str(exc)
        for name in ("controller.log", "events.csv", "server-a.log", "server-b.log"):
            try:
                run(
                    scp + [f"{target}:{remote_dir}/{name}", str(result_dir / name)],
                    f"collect-{name}.log",
                    30,
                    False,
                )
            except (OSError, subprocess.SubprocessError):
                pass
        try:
            state = remote(
                "cat /sys/kernel/debug/sched/ext", "cleanup-state.log", check=False
            )
            if not re.search(r"^enabled\s*:\s*0\s*$", state, re.M):
                raise RuntimeError("scheduler did not detach")
            report["scheduler_clean"] = True
            if before is not None:
                after = remote("dmesg", "dmesg-after.log", check=False)
                if not after.startswith(before):
                    raise RuntimeError("dmesg history changed or wrapped")
                delta = after[len(before) :]
                (result_dir / "dmesg-delta.log").write_text(delta)
                if re.search(
                    r"BUG:|WARNING:|Oops:|watchdog.*(stall|lockup)|"
                    r"sched_ext:.*(error|stall)|Out of memory|hung task",
                    delta,
                    re.I,
                ):
                    raise RuntimeError("new kernel warning/error")
                report["kernel_clean"] = True
            controller_log = (result_dir / "controller.log").read_text(errors="replace")
            match = re.search(
                r"BPF enqueue=(\d+) placed=(\d+) running=(\d+) wrong_cpu=(\d+)",
                controller_log,
            )
            if (
                not match
                or int(match.group(2)) == 0
                or int(match.group(3)) == 0
                or int(match.group(4))
            ):
                raise RuntimeError("BPF placement counters failed")
            if "status=OK" not in controller_log:
                raise RuntimeError("controller did not stop cleanly")
            report["bpf"] = {
                "enqueue": int(match.group(1)),
                "placed": int(match.group(2)),
                "running": int(match.group(3)),
                "wrong_cpu": int(match.group(4)),
            }
            with (result_dir / "events.csv").open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            grants = [row for row in rows if row["event"] == "GRANT"]
            allocations = [row for row in rows if row["event"] in ("GRANT", "SHRINK")]
            if args.allocation_policy == "prefill-first":
                phases = [row for row in rows if row["event"] == "PHASE"]
                if {row["phase"] for row in phases} != {"1", "2"}:
                    raise RuntimeError("missing real prefill/decode transitions")
                report["phase_updates"] = len(phases)
            full = [
                row
                for row in grants
                if row["workers"] == "8" and row["cpu_mask"] == "255"
            ]
            split = {
                (row["slot"], row["cpu_mask"])
                for row in allocations
                if row["workers"] == "4"
            }
            if not full or len({slot for slot, _ in split}) != 2:
                raise RuntimeError("event log lacks real 8-core and 4+4 grants")
            report["ownership"] = validate_cpu_ownership(rows)
            report["grants"] = {
                "total": len(grants),
                "full8": len(full),
                "shrinks": len(allocations) - len(grants),
                "split4": len([row for row in allocations if row["workers"] == "4"]),
                "split_masks": sorted({int(mask) for _, mask in split}),
            }
            if args.grant_lifetime == "persistent":
                members = [row for row in rows if row["event"] == "MEMBERS"]
                if (
                    len(grants) >= args.output_tokens
                    or len(members) >= args.output_tokens
                ):
                    raise RuntimeError(
                        "persistent gate unexpectedly communicates per graph"
                    )
                report["member_updates"] = len(members)
        except (OSError, RuntimeError) as exc:
            report["validation_failure"] = str(exc)
        report["gate_passed"] = bool(
            report.get("single")
            and report.get("dual", {}).get("outputs_match")
            and report.get("scheduler_clean")
            and report.get("kernel_clean")
            and report.get("bpf", {}).get("wrong_cpu") == 0
            and report.get("grants")
            and not report.get("failure")
            and not report.get("cleanup_failure")
            and not report.get("validation_failure")
        )
        (result_dir / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    print(f"Evidence: {result_dir}")
    return 0 if report["gate_passed"] else 1


if __name__ == "__main__":
    os.chdir(HERE)
    sys.exit(main())
