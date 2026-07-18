#!/usr/bin/env python3
"""Lifecycle and suite runner for the deterministic dual-model benchmark."""

import argparse
import concurrent.futures
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
EXPERIMENTS_DIR = SCRIPT_DIR.parents[1]
KERNEL_FAILURE = re.compile(
    r"(sched_ext.*(stall|error|disabled|watchdog)|runnable task stall|out of memory|oom-kill)",
    re.IGNORECASE,
)


class Remote:
    def __init__(self, host, user, password):
        self.host = host
        self.user = user
        self.password = password
        self.ssh_base = [
            "sshpass",
            "-p",
            password,
            "ssh",
            "-o",
            "StrictHostKeyChecking=no",
            f"{user}@{host}",
        ]
        self.scp_base = [
            "sshpass",
            "-p",
            password,
            "scp",
            "-o",
            "StrictHostKeyChecking=no",
        ]

    def run(self, command, check=True, timeout=None):
        result = subprocess.run(
            self.ssh_base + [command],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
        if check and result.returncode:
            raise RuntimeError(
                f"remote command failed ({result.returncode}): {command}\n{result.stderr.strip()}"
            )
        return result

    def upload(self, local_path, remote_path):
        subprocess.run(
            self.scp_base + [str(local_path), f"{self.user}@{self.host}:{remote_path}"],
            check=True,
            stdout=subprocess.DEVNULL,
        )

    def download(self, remote_path, local_path, optional=False):
        result = subprocess.run(
            self.scp_base + [f"{self.user}@{self.host}:{remote_path}", str(local_path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL if optional else None,
        )
        if result.returncode and not optional:
            raise RuntimeError(f"failed to download {remote_path}")
        return result.returncode == 0


def http_json(url, payload=None, timeout=10):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"} if data is not None else {},
        method="POST" if data is not None else "GET",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.load(response)


def wait_health(url, timeout=180):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            payload = http_json(f"{url}/health", timeout=5)
            if payload.get("status") in {"ok", "no slot available"}:
                return
        except (OSError, urllib.error.URLError, json.JSONDecodeError) as exc:
            last_error = exc
        time.sleep(1)
    raise RuntimeError(f"server health timeout at {url}: {last_error}")


def start_background(remote, command, log_path):
    wrapped = f"nohup {command} > {shlex.quote(log_path)} 2>&1 < /dev/null & echo $!"
    result = remote.run(wrapped)
    pids = [int(line) for line in result.stdout.splitlines() if line.strip().isdigit()]
    if not pids:
        raise RuntimeError(f"failed to capture remote PID for: {command}")
    pid = pids[-1]
    time.sleep(0.2)
    if remote.run(f"kill -0 {pid}", check=False).returncode:
        log = remote.run(f"cat {shlex.quote(log_path)}", check=False).stdout
        raise RuntimeError(f"remote process {pid} exited during startup\n{log}")
    return pid


def stop_pid(remote, pid, grace=10):
    if not pid:
        return
    remote.run(f"kill -TERM {int(pid)} 2>/dev/null || true", check=False)
    deadline = time.monotonic() + grace
    while time.monotonic() < deadline:
        if remote.run(f"kill -0 {int(pid)} 2>/dev/null", check=False).returncode:
            return
        time.sleep(0.25)
    remote.run(f"kill -KILL {int(pid)} 2>/dev/null || true", check=False)


def pids_for_port(remote, port):
    code = (
        "import os\n"
        f"port={json.dumps(str(int(port)))}\n"
        "for name in os.listdir('/proc'):\n"
        "    if not name.isdigit(): continue\n"
        "    try:\n"
        "        args=open('/proc/'+name+'/cmdline','rb').read().rstrip(b'\\0').split(b'\\0')\n"
        "    except OSError: continue\n"
        "    decoded=[arg.decode(errors='replace') for arg in args]\n"
        "    if not any(arg.endswith('/llama-server') for arg in decoded): continue\n"
        "    if any(decoded[i]=='--port' and decoded[i+1]==port for i in range(len(decoded)-1)):\n"
        "        print(name)\n"
    )
    result = remote.run(f"python3 -c {shlex.quote(code)}", check=False)
    return [int(line) for line in result.stdout.splitlines() if line.strip().isdigit()]


def dmesg_delta(before, after):
    before_lines = before.splitlines()
    after_lines = after.splitlines()
    if not before_lines:
        return after
    last = before_lines[-1]
    for index in range(len(after_lines) - 1, -1, -1):
        if after_lines[index] == last:
            return "\n".join(after_lines[index + 1 :]) + ("\n" if index + 1 < len(after_lines) else "")
    return "# Unable to align dmesg snapshots; full post-run log follows.\n" + after


class SuiteRunner:
    def __init__(self, args):
        self.args = args
        self.remote = Remote(args.host, args.user, args.password)
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.suite_dir = Path(args.results_dir) / f"{timestamp}_{args.strategy}_dual_suite"
        self.suite_dir.mkdir(parents=True)
        self.remote_dir = f"/tmp/dual_model_benchmark_{os.getpid()}"
        self.restore_default_server = False
        self.server_pids = {}
        self.sampler_pid = None
        self.scheduler_pid = None
        self.trace_pids = {}

    def prepare(self):
        self.remote.run(f"mkdir -p {shlex.quote(self.remote_dir)}")
        self.remote.upload(SCRIPT_DIR / "remote_sampler.py", f"{self.remote_dir}/remote_sampler.py")
        self.remote.run(f"chmod +x {self.remote_dir}/remote_sampler.py")
        if self.args.metrics in {"rvv", "rvv-barrier"}:
            rvv_bin = Path(self.args.rvv_bin)
            rvv_libelf = Path(self.args.rvv_libelf)
            if not rvv_bin.exists() or not rvv_libelf.exists():
                raise RuntimeError("--metrics rvv requires existing rvv_sched_cost and libelf paths")
            self.remote.run(f"mkdir -p {self.remote_dir}/rvv/lib")
            self.remote.upload(rvv_bin, f"{self.remote_dir}/rvv/rvv_sched_cost")
            self.remote.upload(rvv_libelf, f"{self.remote_dir}/rvv/lib/libelf-0.186.so")
            self.remote.run(
                f"chmod +x {self.remote_dir}/rvv/rvv_sched_cost && "
                f"ln -sf libelf-0.186.so {self.remote_dir}/rvv/lib/libelf.so.1"
            )

        if self.args.strategy.startswith("cfs-"):
            scx_state = self.remote.run(
                "if [ -r /sys/kernel/sched_ext/state ]; then cat /sys/kernel/sched_ext/state; "
                "else echo disabled; fi"
            ).stdout.strip()
            if scx_state not in {"", "disabled"}:
                raise RuntimeError(
                    f"CFS baseline requested while sched_ext state is {scx_state!r}"
                )

        existing_a = pids_for_port(self.remote, self.args.port_a)
        existing_b = pids_for_port(self.remote, self.args.port_b)
        if existing_a or existing_b:
            print(f"[INFO] stopping existing test-port servers: A={existing_a}, B={existing_b}")
        if existing_a and self.args.port_a == 9090 and self.args.preserve_default_server:
            self.restore_default_server = True
        for pid in existing_a + existing_b:
            stop_pid(self.remote, pid)

        config = vars(self.args).copy()
        config["suite_dir"] = str(self.suite_dir)
        (self.suite_dir / "config.json").write_text(json.dumps(config, indent=2) + "\n")
        shutil.copy2(SCRIPT_DIR / "prompts.json", self.suite_dir / "prompts.json")
        system_info = self.remote.run(
            "uname -a; echo; nproc; echo; free -h; echo; "
            "cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor 2>/dev/null || true; "
            "cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq 2>/dev/null || true"
        ).stdout
        (self.suite_dir / "system_info.txt").write_text(system_info)

    def server_spec(self, model):
        if model == "A":
            port, model_path, cpus = self.args.port_a, self.args.model_a, set(range(0, 4))
        else:
            port, model_path, cpus = self.args.port_b, self.args.model_b, set(range(4, 8))
        threads = 4 if self.args.strategy == "cfs-split" else 8
        affinity = cpus if self.args.strategy == "cfs-split" else None
        return port, model_path, threads, affinity

    def start_server(self, model, run_tag):
        port, model_path, threads, affinity = self.server_spec(model)
        base = self.args.llm_path
        loader = f"{base}/bin/ld-linux-riscv64-lp64d.so.1"
        argv = [
            loader,
            "--library-path",
            f"{base}/bin",
            f"{base}/bin/llama-server",
            "-m",
            model_path,
            "--host",
            "0.0.0.0",
            "--port",
            str(port),
            "-ngl",
            "0",
            "-t",
            str(threads),
            "-tb",
            str(threads),
            "-c",
            str(self.args.context),
            "-np",
            "4",
            "--cache-ram",
            "0",
        ]
        affinity_code = f"os.sched_setaffinity(0, {repr(affinity)})\n" if affinity else ""
        lease_hint_code = (
            f"os.environ['GGML_SCX_LEASE_HINT_PATH']={self.args.lease_hint_path!r}\n"
            if self.args.lease_hint_path
            else ""
        )
        code = (
            "import json,os\n"
            + affinity_code
            + lease_hint_code
            + f"argv=json.loads({json.dumps(json.dumps(argv))})\n"
            + "os.execv(argv[0],argv)\n"
        )
        command = f"python3 -c {shlex.quote(code)}"
        log_path = f"{self.remote_dir}/{run_tag}_server_{model.lower()}.log"
        pid = start_background(self.remote, command, log_path)
        self.server_pids[model] = pid
        wait_health(f"http://{self.args.host}:{port}", self.args.server_start_timeout)
        print(f"[INFO] server {model}: pid={pid}, port={port}, threads={threads}, affinity={affinity}")

    def stop_servers(self):
        for model in ("A", "B"):
            stop_pid(self.remote, self.server_pids.pop(model, None))

    def warmup(self):
        def one(model, slot):
            port, _, _, _ = self.server_spec(model)
            payload = {
                "prompt": f"dual model deterministic warmup {model} slot {slot}",
                "n_predict": self.args.warmup_tokens,
                "temperature": 0,
                "top_k": 1,
                "top_p": 1.0,
                "seed": self.args.seed,
                "ignore_eos": True,
                "cache_prompt": False,
                "stream": False,
            }
            response = http_json(
                f"http://{self.args.host}:{port}/completion",
                payload,
                timeout=self.args.server_start_timeout,
            )
            if int(response.get("tokens_predicted", 0)) != self.args.warmup_tokens:
                raise RuntimeError(f"incomplete warmup for {model} slot {slot}")

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            futures = [pool.submit(one, model, slot) for model in ("A", "B") for slot in range(4)]
            for future in futures:
                future.result()
        time.sleep(self.args.quiet_sec)

    def start_sampler(self, run_tag):
        output = f"{self.remote_dir}/{run_tag}_sampler.jsonl"
        command = (
            f"python3 {self.remote_dir}/remote_sampler.py "
            f"--pid-a {self.server_pids['A']} --pid-b {self.server_pids['B']} "
            f"--output {output} --interval {self.args.sample_interval}"
        )
        self.sampler_pid = start_background(
            self.remote, command, f"{self.remote_dir}/{run_tag}_sampler.log"
        )

    def stop_sampler(self):
        stop_pid(self.remote, self.sampler_pid, grace=3)
        self.sampler_pid = None

    def start_traces(self, run_tag):
        if self.args.metrics not in {"rvv", "rvv-barrier"}:
            return
        duration = int(self.args.request_timeout + self.args.milestone_timeout + 60)
        trace_options = "--rvv-only"
        if self.args.metrics == "rvv-barrier":
            trace_options += " --futex-time"
        for model in ("A", "B"):
            command = (
                f"/usr/bin/env LD_LIBRARY_PATH={self.remote_dir}/rvv/lib "
                f"{self.remote_dir}/rvv/rvv_sched_cost "
                f"--tgid {self.server_pids[model]} --duration {duration} "
                f"{trace_options} --json {self.remote_dir}/{run_tag}_rvv_{model.lower()}.json"
            )
            self.trace_pids[model] = start_background(
                self.remote, command, f"{self.remote_dir}/{run_tag}_rvv_{model.lower()}.log"
            )

    def stop_traces(self):
        for model in ("A", "B"):
            stop_pid(self.remote, self.trace_pids.pop(model, None), grace=10)

    def start_external_scheduler(self, run_tag):
        if self.args.strategy != "external":
            return
        exports = (
            f"MODEL_A_PID={self.server_pids['A']} MODEL_B_PID={self.server_pids['B']} "
            "MODEL_A_CPUS=0-7 MODEL_B_CPUS=0-7"
        )
        command = f"export {exports}; exec sh -c {shlex.quote(self.args.external_start_cmd)}"
        self.scheduler_pid = start_background(
            self.remote, f"sh -c {shlex.quote(command)}", f"{self.remote_dir}/{run_tag}_scheduler.log"
        )
        time.sleep(1)

    def stop_external_scheduler(self):
        if self.args.strategy != "external":
            return
        if self.args.external_stop_cmd:
            exports = (
                f"MODEL_A_PID={self.server_pids.get('A', 0)} "
                f"MODEL_B_PID={self.server_pids.get('B', 0)} "
                f"SCHEDULER_PID={self.scheduler_pid or 0}"
            )
            command = f"export {exports}; sh -c {shlex.quote(self.args.external_stop_cmd)}"
            self.remote.run(f"sh -c {shlex.quote(command)}", check=False)
        stop_pid(self.remote, self.scheduler_pid)
        self.scheduler_pid = None

    def collect_remote_files(self, run_dir, run_tag):
        mappings = {
            f"{run_tag}_server_a.log": "server_a.log",
            f"{run_tag}_server_b.log": "server_b.log",
            f"{run_tag}_sampler.jsonl": "sampler.jsonl",
            f"{run_tag}_sampler.log": "sampler.log",
            f"{run_tag}_scheduler.log": "scheduler.log",
            f"{run_tag}_rvv_a.json": "rvv_a.json",
            f"{run_tag}_rvv_b.json": "rvv_b.json",
            f"{run_tag}_rvv_a.log": "rvv_a.log",
            f"{run_tag}_rvv_b.log": "rvv_b.log",
        }
        for remote_name, local_name in mappings.items():
            self.remote.download(
                f"{self.remote_dir}/{remote_name}", run_dir / local_name, optional=True
            )

    def validate_traces(self, run_dir, expected_pids):
        required = self.args.metrics in {"rvv", "rvv-barrier"}
        details = {"required": required, "profile": self.args.metrics, "models": {}}
        if not required:
            details["valid"] = True
            return details

        valid = True
        for model in ("A", "B"):
            path = run_dir / f"rvv_{model.lower()}.json"
            item = {"path": path.name, "present": path.exists()}
            try:
                data = json.loads(path.read_text())
            except (OSError, json.JSONDecodeError) as exc:
                item["error"] = f"{type(exc).__name__}: {exc}"
                details["models"][model] = item
                valid = False
                continue

            vector = data.get("vector") or {}
            futex = data.get("futex") or {}
            item.update(
                {
                    "target_tgid": data.get("target_tgid"),
                    "expected_tgid": expected_pids.get(model),
                    "rvv_only": data.get("rvv_only"),
                    "sample_futex_time": data.get("sample_futex_time"),
                    "v_save_candidates": vector.get("v_save_candidates"),
                    "v_restore_candidates": vector.get("v_restore_candidates"),
                    "futex_wait_completed_ops": futex.get("wait_completed_ops"),
                    "futex_wait_blocked_ops": futex.get("wait_blocked_ops"),
                    "lost_ts_updates": data.get("lost_ts_updates"),
                }
            )
            item_valid = bool(
                item["target_tgid"] == item["expected_tgid"]
                and item["rvv_only"] is True
                and isinstance(item["v_save_candidates"], int)
                and item["v_save_candidates"] > 0
                and isinstance(item["v_restore_candidates"], int)
                and item["v_restore_candidates"] > 0
                and item["lost_ts_updates"] == 0
            )
            if self.args.metrics == "rvv-barrier":
                item_valid = bool(
                    item_valid
                    and item["sample_futex_time"] is True
                    and isinstance(item["futex_wait_completed_ops"], int)
                    and item["futex_wait_completed_ops"] > 0
                    and isinstance(item["futex_wait_blocked_ops"], int)
                    and item["futex_wait_blocked_ops"] > 0
                    and any(
                        int(entry.get("wait_ops", 0)) > 0
                        for entry in data.get("futex_addrs", [])
                    )
                )
            else:
                item_valid = bool(item_valid and item["sample_futex_time"] is False)
            item["valid"] = item_valid
            details["models"][model] = item
            valid = valid and item_valid

        details["valid"] = valid
        return details

    def run_workload(self, run_dir, direction):
        incumbent = "A" if direction == "ab" else "B"
        injected = "B" if direction == "ab" else "A"
        inc_port = self.args.port_a if incumbent == "A" else self.args.port_b
        inj_port = self.args.port_b if injected == "B" else self.args.port_a
        command = [
            sys.executable,
            str(SCRIPT_DIR / "workload.py"),
            "--incumbent-url",
            f"http://{self.args.host}:{inc_port}",
            "--injected-url",
            f"http://{self.args.host}:{inj_port}",
            "--incumbent-model",
            incumbent,
            "--injected-model",
            injected,
            "--prompts",
            str(SCRIPT_DIR / "prompts.json"),
            "--events",
            str(run_dir / "events.jsonl"),
            "--output",
            str(run_dir / "workload.json"),
            "--output-tokens",
            str(self.args.output_tokens),
            "--injection-token",
            str(self.args.injection_token),
            "--arrival-gap-sec",
            str(self.args.arrival_gap_sec),
            "--seed",
            str(self.args.seed),
            "--request-timeout",
            str(self.args.request_timeout),
            "--milestone-timeout",
            str(self.args.milestone_timeout),
        ]
        if self.args.injection_hook:
            command.extend(
                [
                    "--injection-hook",
                    self.args.injection_hook,
                    "--injection-hook-timeout",
                    str(self.args.injection_hook_timeout),
                ]
            )
        return subprocess.run(command).returncode

    def validate_platform(self, run_dir):
        sampler_path = run_dir / "sampler.jsonl"
        frequencies = []
        temperatures = []
        available_memory = []
        if sampler_path.exists():
            for line in sampler_path.read_text(errors="replace").splitlines():
                try:
                    platform = json.loads(line).get("platform", {})
                except json.JSONDecodeError:
                    continue
                frequencies.extend(
                    value
                    for value in platform.get("frequencies_khz", {}).values()
                    if value is not None
                )
                temperatures.extend(
                    value
                    for value in platform.get("temperatures_millic", {}).values()
                    if value is not None
                )
                memory = platform.get("memory_kib", {}).get("MemAvailable")
                if memory is not None:
                    available_memory.append(memory)
        details = {
            "samples_present": bool(frequencies and temperatures and available_memory),
            "frequency_min_khz": min(frequencies) if frequencies else None,
            "temperature_max_millic": max(temperatures) if temperatures else None,
            "memory_available_min_kib": min(available_memory) if available_memory else None,
        }
        valid = bool(
            details["samples_present"]
            and details["frequency_min_khz"] >= self.args.min_frequency_khz
            and details["temperature_max_millic"] <= self.args.max_temperature_millic
            and details["memory_available_min_kib"] >= self.args.min_available_memory_kib
        )
        details["valid"] = valid
        return details

    def run_attempt(self, loop, direction, attempt):
        run_tag = f"run{loop:02d}_{direction}_attempt{attempt}"
        run_dir = self.suite_dir / run_tag
        run_dir.mkdir()
        metadata = {
            "loop": loop,
            "direction": direction,
            "attempt": attempt,
            "selected": False,
            "strategy": self.args.strategy,
        }
        (run_dir / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
        print(f"[INFO] starting {run_tag}")

        dmesg_before = self.remote.run("dmesg", check=False).stdout
        (run_dir / "dmesg_before.log").write_text(dmesg_before)
        return_code = 1
        run_pids = {}
        try:
            order = ("A", "B") if direction == "ab" else ("B", "A")
            for model in order:
                self.start_server(model, run_tag)
            run_pids = dict(self.server_pids)
            self.warmup()
            self.start_external_scheduler(run_tag)
            self.start_sampler(run_tag)
            self.start_traces(run_tag)
            return_code = self.run_workload(run_dir, direction)
        except Exception as exc:
            (run_dir / "harness_error.txt").write_text(f"{type(exc).__name__}: {exc}\n")
            print(f"[ERROR] {run_tag}: {exc}")
        finally:
            self.stop_traces()
            self.stop_sampler()
            self.stop_external_scheduler()
            dmesg_after = self.remote.run("dmesg", check=False).stdout
            (run_dir / "dmesg_after.log").write_text(dmesg_after)
            delta = dmesg_delta(dmesg_before, dmesg_after)
            (run_dir / "dmesg_delta.log").write_text(delta)
            self.collect_remote_files(run_dir, run_tag)
            self.stop_servers()

        workload_path = run_dir / "workload.json"
        valid = False
        platform_validation = self.validate_platform(run_dir)
        trace_validation = self.validate_traces(run_dir, run_pids)
        if return_code == 0 and workload_path.exists():
            workload = json.loads(workload_path.read_text())
            valid = (
                workload.get("valid", False)
                and not KERNEL_FAILURE.search((run_dir / "dmesg_delta.log").read_text())
                and platform_validation["valid"]
                and trace_validation["valid"]
            )
        metadata["selected"] = valid
        metadata["return_code"] = return_code
        metadata["valid"] = valid
        metadata["platform_validation"] = platform_validation
        metadata["trace_validation"] = trace_validation
        (run_dir / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
        print(f"[INFO] completed {run_tag}: valid={valid}")
        return valid

    def run(self):
        self.prepare()
        failed = False
        try:
            directions = ("ab", "ba") if self.args.directions == "both" else (self.args.directions,)
            for loop in range(1, self.args.loops + 1):
                for direction in directions:
                    accepted = False
                    for attempt in range(1, self.args.max_attempts + 1):
                        if self.run_attempt(loop, direction, attempt):
                            accepted = True
                            break
                    if not accepted:
                        failed = True
                        print(f"[ERROR] no valid attempt for loop={loop} direction={direction}")
                        break
                if failed:
                    break
        finally:
            self.stop_traces()
            self.stop_sampler()
            self.stop_external_scheduler()
            self.stop_servers()
            if self.restore_default_server:
                print("[INFO] restoring default llama-server on port 9090")
                start_background(
                    self.remote,
                    f"sh {self.args.llm_path}/run-server.sh",
                    "/tmp/llama-server.log",
                )
                wait_health(f"http://{self.args.host}:9090", self.args.server_start_timeout)

        report_result = subprocess.run(
            [sys.executable, str(SCRIPT_DIR / "report.py"), str(self.suite_dir)]
        )
        print(f"[INFO] suite results: {self.suite_dir}")
        return 2 if failed or report_result.returncode else 0


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--strategy", choices=("cfs-shared", "cfs-split", "external"), default="cfs-shared")
    parser.add_argument("--loops", type=int, default=1)
    parser.add_argument("--directions", choices=("ab", "ba", "both"), default="ab")
    parser.add_argument("--output-tokens", type=int, default=256)
    parser.add_argument("--injection-token", type=int, default=4)
    parser.add_argument("--arrival-gap-sec", type=float, default=1.0)
    parser.add_argument(
        "--metrics", choices=("standard", "rvv", "rvv-barrier"), default="standard"
    )
    parser.add_argument("--host", default=os.environ.get("REMOTE_IP", "192.168.3.67"))
    parser.add_argument("--user", default=os.environ.get("REMOTE_USER", "root"))
    parser.add_argument("--password", default=os.environ.get("REMOTE_PASS", "bianbu"))
    parser.add_argument("--port-a", type=int, default=9090)
    parser.add_argument("--port-b", type=int, default=9091)
    parser.add_argument("--llm-path", default="/mnt/llm/llama")
    parser.add_argument("--model-a", default="/mnt/llm/llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf")
    parser.add_argument("--model-b", default="/mnt/llm/llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf")
    parser.add_argument("--context", type=int, default=4096)
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--warmup-tokens", type=int, default=2)
    parser.add_argument("--quiet-sec", type=float, default=5.0)
    parser.add_argument("--sample-interval", type=float, default=1.0)
    parser.add_argument("--min-frequency-khz", type=int, default=1_600_000)
    parser.add_argument("--max-temperature-millic", type=int, default=85_000)
    parser.add_argument("--min-available-memory-kib", type=int, default=1_048_576)
    parser.add_argument("--request-timeout", type=float, default=1800.0)
    parser.add_argument("--milestone-timeout", type=float, default=600.0)
    parser.add_argument("--server-start-timeout", type=float, default=180.0)
    parser.add_argument("--max-attempts", type=int, default=2)
    parser.add_argument("--results-dir", default=str(SCRIPT_DIR / "results"))
    parser.add_argument("--external-start-cmd", default="")
    parser.add_argument("--external-stop-cmd", default="")
    parser.add_argument("--lease-hint-path", default="")
    parser.add_argument("--injection-hook", default="")
    parser.add_argument("--injection-hook-timeout", type=float, default=30.0)
    parser.add_argument("--rvv-bin", default=str(EXPERIMENTS_DIR / "rvv_sched_cost/rvv_sched_cost"))
    parser.add_argument(
        "--rvv-libelf",
        default=str(EXPERIMENTS_DIR.parent / "jupiter-buildroot/output/k1_v2/target/usr/lib/libelf-0.186.so"),
    )
    parser.add_argument("--no-preserve-default-server", dest="preserve_default_server", action="store_false")
    parser.add_argument("--smoke", action="store_true")
    parser.set_defaults(preserve_default_server=True)
    args = parser.parse_args()
    if args.strategy == "external" and not args.external_start_cmd:
        parser.error("--strategy external requires --external-start-cmd")
    if args.injection_token <= 0 or args.injection_token >= args.output_tokens:
        parser.error("--injection-token must be between 1 and output-tokens - 1")
    if args.loops < 1 or args.max_attempts < 1:
        parser.error("loops and max-attempts must be positive")
    if args.smoke:
        args.loops = 1
        args.output_tokens = 16
        args.injection_token = 4
        args.quiet_sec = 1.0
        args.warmup_tokens = 1
        args.request_timeout = min(args.request_timeout, 300.0)
    return args


def main():
    missing = [command for command in ("sshpass", "ssh", "scp") if shutil.which(command) is None]
    if missing:
        raise SystemExit("missing required commands: " + ", ".join(missing))
    args = parse_args()
    raise SystemExit(SuiteRunner(args).run())


if __name__ == "__main__":
    main()
