#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Isolated 2/4/8-process experiments; each llama server handles one request."""
import argparse
import csv
import datetime
import hashlib
import json
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
from run_benchmark import event_metrics, LLAMA_BIN, PROMPTS

HERE = Path(__file__).resolve().parent
CASES = ("cfs_baseline", "scx_dynamic_persistent_phase_reserve1_full_release",
         "scx_dynamic_persistent_phase_reserve1")


def save(path, data):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(data, indent=2) + "\n")
    temporary.replace(path)


class Remote:
    def __init__(self, host, user):
        self.target = f"{user}@{host}"
        self.env = os.environ.copy()
        prefix = []
        if self.env.get("REMOTE_PASS"):
            self.env["SSHPASS"] = self.env.pop("REMOTE_PASS")
            prefix = ["sshpass", "-e"]
        options = ["-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=accept-new"]
        self.ssh = prefix + ["ssh", *options, self.target]
        self.scp = prefix + ["scp", "-O", *options]

    def command(self, args, timeout=120, check=True):
        r = subprocess.run(args, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout)
        if check and r.returncode:
            raise RuntimeError(r.stdout.decode(errors="replace")[-4000:])
        return r

    def shell(self, command, timeout=120, check=True):
        return self.command(self.ssh + [command], timeout, check).stdout.decode(errors="replace")

    def put(self, local, remote):
        self.command(self.scp + [str(local), f"{self.target}:{remote}"])

    def fetch(self, remote, local):
        self.command(self.scp + [f"{self.target}:{remote}", str(local)])

    def idle(self):
        state = self.shell("cat /sys/kernel/debug/sched/ext")
        if not re.search(r"^enabled\s*:\s*0\s*$", state, re.M):
            raise RuntimeError("sched_ext is already active")
        procs = self.shell("ps w")
        if any("llama-server" in line or "dt_controller" in line for line in procs.splitlines()):
            raise RuntimeError("existing llama/controller process on board; left untouched")

    def start(self, command, logfile):
        pid = self.shell(f"{command} >{shlex.quote(logfile)} 2>&1 </dev/null & echo $!").strip()
        return int(pid)

    def stop(self, pid, directory):
        # Check the unique experiment path before signalling a numeric PID.
        cmd = (f"test -r /proc/{pid}/cmdline && "
               f"tr '\\0' ' ' < /proc/{pid}/cmdline | grep -F -q {shlex.quote(directory)}")
        self.shell(f"if {cmd}; then kill -TERM {pid}; n=0; "
                   f"while [ -r /proc/{pid}/stat ]; do "
                   f"state=$(awk '{{print $3}}' /proc/{pid}/stat); [ \"$state\" = Z ] && break; "
                   f"n=$((n+1)); [ $n -lt 100 ] || {{ if {cmd}; then kill -KILL {pid}; fi; break; }}; "
                   "sleep .1; done; fi", timeout=20)

    def memory(self, pids=()):
        raw = self.shell("cat /proc/meminfo")
        info = {k: int(v) for k, v in re.findall(r"^(\w+):\s+(\d+)", raw, re.M)}
        info["processes"] = {}
        for pid in pids:
            raw = self.shell(f"cat /proc/{pid}/status; cat /proc/{pid}/smaps_rollup 2>/dev/null || true")
            info["processes"][str(pid)] = {k: int(v) for k, v in re.findall(
                r"^(VmRSS|VmSize|RssAnon|RssFile|Pss|Private_Dirty|Private_Clean):\s+(\d+)", raw, re.M)}
        return info


def health(url, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url + "/health", timeout=2) as r:
                if r.status == 200:
                    return
        except OSError:
            pass
        time.sleep(.5)
    raise RuntimeError(f"health timeout: {url}")


def warmup(url):
    payload = {"prompt": "multi-process warmup", "n_predict": 2, "temperature": 0,
               "seed": 1234, "ignore_eos": True, "cache_prompt": False, "stream": False}
    request = urllib.request.Request(url + "/completion", data=json.dumps(payload).encode(),
                                     headers={"content-type": "application/json"})
    with urllib.request.urlopen(request, timeout=180) as r:
        if json.load(r).get("tokens_predicted") != 2:
            raise RuntimeError("incomplete warmup")


def validate_controller(rows, log, servers, full_release):
    if "status=OK" not in log:
        raise RuntimeError("controller did not exit cleanly")
    counters = re.search(r"BPF enqueue=(\d+) placed=(\d+) running=(\d+) wrong_cpu=(\d+)", log)
    if not counters or not int(counters[2]) or not int(counters[3]) or int(counters[4]):
        raise RuntimeError("missing BPF placements or wrong CPU")
    ownership = validate_cpu_ownership(rows)
    hello = {int(r["pid"]): int(r["slot"]) for r in rows if r["event"] == "HELLO"}
    if len(hello) != len(servers):
        raise RuntimeError(f"expected {len(servers)} registered processes, got {hello}")
    for s in servers:
        s["controller_slot"] = hello[s["pid"]]
    if full_release and ownership["shrinks_checked"]:
        raise RuntimeError("full-release ablation unexpectedly used SHRINK")
    return {"ownership": ownership, "wrong_cpu": int(counters[4]),
            "placed": int(counters[2]), "running": int(counters[3])}


def run_case(remote, args, campaign_dir, remote_dir, index, case, count, loop):
    local = campaign_dir / f"{index:02d}-{case}-p{count}-r{loop}"
    local.mkdir()
    prefix = f"{remote_dir}/c{index}"
    socket_path = prefix + ".sock"
    servers, pids, ctl = [], [], None
    item = {"case": case, "process_count": count, "requests_per_process": 1,
            "loop": loop, "valid": False, "path": local.name, "servers": servers}
    scx = case != "cfs_baseline"
    full_release = case.endswith("_full_release")
    before_lines = after_lines = 0
    try:
        remote.idle()
        memories = [remote.memory()]
        if scx:
            ctl = remote.start(f"{remote_dir}/dt_controller --socket {socket_path} --mode dynamic "
                "--scx --grant-lifetime persistent --allocation-policy prefill-first "
                f"--decode-wait-ms 0 --decode-floor-on-prefill --events {prefix}.events.csv",
                prefix + ".controller.log")
            deadline = time.monotonic() + 10
            while remote.command(remote.ssh + [f"test -S {socket_path}"], check=False).returncode:
                if time.monotonic() >= deadline:
                    raise RuntimeError("controller socket timeout")
                time.sleep(.05)
        for i in range(count):
            available = remote.memory()["MemAvailable"]
            # The model has private repacked weights as well as file-backed
            # pages. Budget measured load growth plus 512 MiB, not GGUF sharing.
            growth = max((a["MemAvailable"] - b["MemAvailable"]
                          for a, b in zip(memories, memories[1:])), default=2 * 1024 * 1024)
            if available < max(growth, 1024 * 1024) + 512 * 1024:
                raise RuntimeError(f"insufficient memory before server {i}: available={available} KiB")
            url = f"http://{args.host}:{args.port_base + i}"
            dynamic = (f"GGML_DYNAMIC_THREADS_SOCKET={socket_path} "
                       f"GGML_DYNAMIC_THREADS_RETAIN_SHRINK={int(not full_release)} ") if scx else ""
            loader = shlex.quote(args.llm_path + "/bin/ld-linux-riscv64-lp64d.so.1")
            libpath = shlex.quote(remote_dir + "/bin:" + args.llm_path + "/bin")
            command = (f"env OMP_DYNAMIC=FALSE {dynamic}{loader} --library-path {libpath} "
                f"{remote_dir}/bin/llama-server -m {shlex.quote(args.model)} --host 0.0.0.0 "
                f"--port {args.port_base + i} -ngl 0 -t 8 -tb 8 -np 1 -c 1024 "
                "--cache-ram 0 --fit off")
            pid = remote.start(command, f"{prefix}.server{i}.log")
            pids.append(pid)
            servers.append({"process_id": f"P{i}", "pid": pid, "port": args.port_base + i,
                            "url": url, "controller_slot": None})
            save(local / "servers.json", servers)
            health(url, args.server_timeout)
            warmup(url)
            memories.append(remote.memory(pids))
            save(local / "memory.json", memories)
        time.sleep(1)
        if scx:
            before_lines = int(remote.shell(f"wc -l < {prefix}.events.csv"))
        command = [sys.executable, str(HERE / "multi_workload.py"),
            "--servers", str(local / "servers.json"), "--prompts", str(PROMPTS),
            "--events", str(local / "events.jsonl"), "--output", str(local / "workload.json"),
            "--output-tokens", str(args.output_tokens), "--injection-token", str(args.injection_token)]
        completed = remote.command(command, timeout=2500, check=False)
        (local / "workload.log").write_bytes(completed.stdout)
        if scx:
            after_lines = int(remote.shell(f"wc -l < {prefix}.events.csv"))
        if completed.returncode:
            raise RuntimeError(f"workload failed: exit {completed.returncode}")
        workload = json.loads((local / "workload.json").read_text())
        if not workload["valid"]:
            raise RuntimeError("incomplete workload or missing injection milestone")
        item["metrics"] = workload["metrics"]
        item["within_run_deterministic"] = workload["within_run_deterministic"]
        memories.append(remote.memory(pids))
        save(local / "memory.json", memories)
    except Exception as exc:
        item["failure"] = f"{type(exc).__name__}: {exc}"
    finally:
        errors = []
        for pid in pids:
            try:
                remote.stop(pid, remote_dir)
            except Exception as exc:
                errors.append(str(exc))
        if ctl:
            try:
                remote.stop(ctl, remote_dir)
            except Exception as exc:
                errors.append(str(exc))
        names = [f"server{i}.log" for i in range(len(pids))]
        if ctl:
            names += ["controller.log", "events.csv"]
        for name in names:
            try:
                remote.fetch(f"{prefix}.{name}", local / name)
            except Exception as exc:
                errors.append(str(exc))
        try:
            remote.idle()
            if scx and not errors:
                with (local / "events.csv").open() as stream:
                    rows = list(csv.DictReader(stream))
                item["controller"] = validate_controller(rows, (local / "controller.log").read_text(),
                                                          servers, full_release)
                measured = rows[max(0, before_lines - 1):max(0, after_lines - 1)]
                item["control"] = event_metrics(measured)
                observed = {int(r["pid"]) for r in measured if r["event"] == "MEMBERS"}
                if not set(pids).issubset(observed):
                    raise RuntimeError("not every process computed during the measured workload")
                if not item["control"]["decode_floor_targets"]:
                    raise RuntimeError("decode floor was never exercised")
        except Exception as exc:
            errors.append(str(exc))
        if errors:
            item["cleanup_or_validation_failure"] = errors
        item["valid"] = not item.get("failure") and not errors
        save(local / "servers.json", servers)
        save(local / "summary.json", item)
    return item


def aggregate(runs):
    groups = {}
    for r in runs:
        if r["valid"]:
            groups.setdefault((r["case"], r["process_count"]), []).append(r)
    result = []
    for (case, n), group in sorted(groups.items()):
        row = {"case": case, "process_count": n, "valid_runs": len(group), "metrics": {}}
        for metric in ("aggregate_tps", "injected_ttft_mean_ms", "itl_p95_ms", "itl_p99_ms", "itl_max_ms",
                       "overlap_itl_p95_ms", "process_token_rate_jain"):
            values = [r["metrics"][metric] for r in group if r["metrics"].get(metric) is not None]
            if values:
                row["metrics"][metric] = {"mean": statistics.fmean(values),
                    "sd": statistics.stdev(values) if len(values) > 1 else 0,
                    "min": min(values), "max": max(values)}
        result.append(row)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("REMOTE_IP", "192.168.3.67"))
    parser.add_argument("--user", default=os.environ.get("REMOTE_USER", "root"))
    parser.add_argument("--llm-path", default="/mnt/llm/llama")
    parser.add_argument("--model", default="/mnt/llm/llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf")
    parser.add_argument("--port-base", type=int, default=18080)
    parser.add_argument("--cases", default=",".join(CASES))
    parser.add_argument("--process-counts", default="2,4,8")
    parser.add_argument("--scenarios", choices=("asynchronous",), default="asynchronous")
    parser.add_argument("--loops", type=int, default=1)
    parser.add_argument("--output-tokens", type=int, default=32)
    parser.add_argument("--injection-token", type=int, default=4)
    parser.add_argument("--seed", type=int, default=20260919)
    parser.add_argument("--interleave", action="store_true")
    parser.add_argument("--server-timeout", type=float, default=180)
    args = parser.parse_args()
    cases = args.cases.split(",")
    try:
        counts = [int(n) for n in args.process_counts.split(",")]
    except ValueError:
        parser.error("process-counts must be drawn from 2,4,8")
    if not counts or any(n not in (2, 4, 8) for n in counts) or len(set(counts)) != len(counts):
        parser.error("process-counts must contain distinct values from 2,4,8")
    if not cases or any(c not in CASES for c in cases) or len(set(cases)) != len(cases):
        parser.error(f"cases must contain distinct values from {CASES}")
    if args.loops < 1 or not 1 <= args.injection_token < args.output_tokens <= 768:
        parser.error("invalid loops/output/injection-token")
    if not 1024 <= args.port_base <= 65536 - max(counts):
        parser.error("port range invalid")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_id = f"multi-{stamp}-{uuid.uuid4().hex[:8]}"
    output = HERE / "results" / run_id
    output.mkdir(parents=True)
    remote_dir = f"/tmp/{run_id}"
    remote = Remote(args.host, args.user)
    campaign = {"campaign_id": run_id, "complete": False, "cases": cases,
        "process_counts": counts, "requests_per_process": 1, "scenario": "asynchronous",
        "threads": 8, "threads_batch": 8, "context": 1024, "loops": args.loops,
        "output_tokens": args.output_tokens, "injection_token": args.injection_token,
        "decode_wait_ms": 0, "decode_floor_on_prefill": True, "remote_dir": remote_dir,
        "host": args.host, "model": args.model, "runs": []}
    before = None
    save(output / "campaign.json", campaign)
    print(f"Evidence: {output}", flush=True)
    try:
        remote.idle()
        before = remote.shell("dmesg")
        (output / "dmesg-before.log").write_text(before)
        remote.shell(f"set -eu; test -r {shlex.quote(args.model)}; mkdir {remote_dir}; mkdir {remote_dir}/bin")
        with tempfile.NamedTemporaryFile(suffix=".tar") as archive:
            with tarfile.open(fileobj=archive, mode="w", dereference=False) as tar:
                for path in LLAMA_BIN.iterdir():
                    tar.add(path, arcname=path.name, recursive=False)
            archive.flush()
            remote.put(archive.name, remote_dir + "/runtime.tar")
        remote.put(HERE / "build/dt_controller", remote_dir + "/dt_controller")
        remote.shell(f"tar -C {remote_dir}/bin -xf {remote_dir}/runtime.tar")
        campaign["sha256"] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in
            (LLAMA_BIN / "llama-server", LLAMA_BIN / "libggml-cpu.so.0.13.0", HERE / "build/dt_controller")}
        order = cases[:]
        random.Random(args.seed).shuffle(order)
        schedule = [(c, n, loop) for n in counts for loop in range(args.loops)
                    for c in (order if not args.interleave or loop % 2 == 0 else order[::-1])]
        campaign["schedule"] = schedule
        save(output / "campaign.json", campaign)
        for index, (case, n, loop) in enumerate(schedule):
            print(f"START {index + 1}/{len(schedule)} {case} processes={n} repeat={loop}", flush=True)
            item = run_case(remote, args, output, remote_dir, index, case, n, loop)
            campaign["runs"].append(item)
            campaign["aggregate"] = aggregate(campaign["runs"])
            save(output / "campaign.json", campaign)
            print(f"END valid={item['valid']}", flush=True)
            if not item["valid"]:
                raise RuntimeError(f"failed run: {item['path']}; see summary.json")
        campaign["complete"] = True
    except Exception as exc:
        campaign["failure"] = f"{type(exc).__name__}: {exc}"
    finally:
        if before is not None:
            try:
                after = remote.shell("dmesg")
                if not after.startswith(before):
                    raise RuntimeError("dmesg history changed/wrapped")
                delta = after[len(before):]
                (output / "dmesg-delta.log").write_text(delta)
                if re.search(r"BUG:|WARNING:|Oops:|watchdog.*(stall|lockup)|Out of memory|hung task|"
                             r"sched_ext:.*(error|stall)", delta, re.I):
                    raise RuntimeError("new kernel warning/error")
                remote.idle()
                campaign["kernel_clean"] = True
            except Exception as exc:
                campaign["cleanup_failure"] = str(exc)
                campaign["complete"] = False
        save(output / "campaign.json", campaign)
    print(json.dumps({k: campaign.get(k) for k in ("campaign_id", "complete", "failure", "cleanup_failure")}))
    return 0 if campaign["complete"] else 1


if __name__ == "__main__":
    sys.exit(main())
