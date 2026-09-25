#!/usr/bin/env python3
"""Suite orchestration for the v2 benchmark (server lifecycle + validity)."""

from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

from .board import (
    Local,
    Remote,
    dmesg_delta,
    pids_for_port,
    pids_with_cmdline_arg,
    start_background,
    stop_pid,
)
from .cfs_affinity import CfsAffinityStrategy
from .dynamic_threads import DynamicThreadsStrategy
from .report import generate_suite
from .scx_llm_pin import ScxLlmPinStrategy
from .server import ServerManager
from .x86_dynamic import X86DynamicStrategy
from .x86_scx_llm_pin import X86ScxLlmPinStrategy


SCRIPT_DIR = Path(__file__).resolve().parent
SAMPLE_SCRIPT = SCRIPT_DIR / "remote_sampler.py"

DUAL_SCENARIOS = {"dual-async": "asynchronous", "dual-sync": "synchronized"}


def validate_models(config):
    """Validate the multi-scenario model list, returning the normalized list."""
    models = config.get("models", [])
    if not 1 <= len(models) <= 8:
        raise ValueError("models must contain 1..8 servers")
    ids, ports = set(), set()
    for model in models:
        model_id = str(model.get("id", ""))
        if not model_id or not all(ch.isalnum() or ch in "_-" for ch in model_id):
            raise ValueError("model id must use letters, digits, _ or -")
        if model_id in ids or model.get("port") in ports:
            raise ValueError("duplicate model id or port")
        ids.add(model_id)
        ports.add(model.get("port"))
        if type(model.get("port")) is not int or not 1024 <= model["port"] <= 65535:
            raise ValueError("port must be 1024..65535")
        if not isinstance(model.get("model"), str) or not model["model"].startswith("/"):
            raise ValueError("model must be an absolute board path")
        mask = model.get("cpu_mask")
        if mask is not None:
            if type(mask) is not int or not 0 < mask <= 255:
                raise ValueError("cpu_mask must be an integer in 1..255")
        for field in ("threads", "threads_batch"):
            value = model.get(field, 8 if field == "threads" else model.get("threads", 8))
            if type(value) is not int or not 1 <= value <= 8:
                raise ValueError(f"{field} must be 1..8")
        if type(model.get("requests", 1)) is not int or not 1 <= model.get("requests", 1) <= 8:
            raise ValueError("requests must be 1..8")
    return models


class SuiteRunner:
    def __init__(self, args):
        self.args = args
        self.remote = (
            Local()
            if args.local
            else Remote(args.host, args.user, args.password)
        )
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.suite_dir = Path(args.results_dir) / (
            f"{timestamp}_{args.scenario}_{args.strategy}_suite"
        )
        self.suite_dir.mkdir(parents=True)
        self.remote_dir = f"/tmp/benchmark_v2_{os.getpid()}"
        self.servers = ServerManager(self.remote, args, self.remote_dir)
        self.sampler_pid = None
        self.trace_pids = {}
        self.models = self._load_models()
        self.strategy = None
        if args.strategy == "dynamic":
            self.strategy = DynamicThreadsStrategy(args, self.remote, self.remote_dir)
        elif args.strategy == "scx-llm-pin":
            self.strategy = ScxLlmPinStrategy(args, self.remote, self.remote_dir)
        elif args.strategy == "cfs-affinity":
            self.strategy = CfsAffinityStrategy(args, self.remote, self.remote_dir)
        elif args.strategy == "x86-dynamic":
            self.strategy = X86DynamicStrategy(args, self.remote, self.remote_dir)
        elif args.strategy == "x86-scx-llm-pin":
            self.strategy = X86ScxLlmPinStrategy(args, self.remote, self.remote_dir)

    # ------------------------------------------------------------------ setup
    def _load_models(self):
        if self.args.scenario != "multi":
            return None
        config = json.loads(Path(self.args.config).read_text())
        return validate_models(config)

    def prepare(self):
        self.remote.run(f"mkdir -p {shlex.quote(self.remote_dir)}")
        self.remote.upload(SAMPLE_SCRIPT, f"{self.remote_dir}/remote_sampler.py")
        self.remote.run(f"chmod +x {self.remote_dir}/remote_sampler.py")
        if self.strategy is not None:
            self.strategy.prepare()
        if self.args.rvv_trace:
            remote_bin = shlex.quote(self.args.rvv_remote_bin)
            remote_lib = shlex.quote(self.args.rvv_remote_lib_dir)
            if self.args.rvv_upload:
                self.remote.run(f"mkdir -p {remote_lib}")
                self.remote.upload(self.args.rvv_local_bin, self.args.rvv_remote_bin)
                self.remote.upload(
                    self.args.rvv_local_libelf, f"{self.args.rvv_remote_lib_dir}/libelf-0.186.so"
                )
                self.remote.run(
                    f"chmod +x {remote_bin} && "
                    f"ln -sf libelf-0.186.so {remote_lib}/libelf.so.1"
                )
            probe = self.remote.run(f"test -x {remote_bin}", check=False)
            if probe.returncode:
                raise RuntimeError(
                    f"board rvv_sched_cost is not executable: {self.args.rvv_remote_bin}; "
                    f"deploy it or pass --rvv-upload"
                )

        config = vars(self.args).copy()
        config["suite_dir"] = str(self.suite_dir)
        if self.models is not None:
            config["models"] = self.models
            shutil.copy2(self.args.config, self.suite_dir / "models.json")
        (self.suite_dir / "config.json").write_text(json.dumps(config, indent=2) + "\n")
        if self.args.prompts_file:
            shutil.copy2(self.args.prompts_file, self.suite_dir / "prompts.json")

        system_info = self.remote.run(
            "uname -a; echo; nproc; echo; free -h; echo; "
            "cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor 2>/dev/null || true; "
            "cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq 2>/dev/null || true"
        ).stdout
        (self.suite_dir / "system_info.txt").write_text(system_info)

        self._stop_existing()

    def _ports(self):
        if self.args.scenario == "multi":
            return [m["port"] for m in self.models]
        return [self.args.port_a, self.args.port_b]

    def _stop_existing(self):
        for port in self._ports():
            existing = pids_for_port(self.remote, port)
            if existing:
                print(f"[INFO] stopping existing test-port server on {port}: {existing}")
            for pid in existing:
                stop_pid(self.remote, pid)

    # --------------------------------------------------------------- per run
    def _server_overrides(self):
        """Merge the strategy overrides with the local x86 direct-exec base."""
        overrides = dict(self.strategy.server_overrides() or {}) if self.strategy else {}
        if self.args.local:
            base = {
                "direct": True,
                "runtime_bin": self.args.x86_runtime_bin,
                "library_path": self.args.x86_runtime_bin,
            }
            base.update(overrides)
            return base
        return overrides or None

    def _server_specs(self, direction):
        if self.args.scenario == "multi":
            specs = []
            for model in self.models:
                specs.append(
                    {
                        "model": model["id"],
                        "port": model["port"],
                        "model_path": model["model"],
                        "threads": model.get("threads", 8),
                        "threads_batch": model.get("threads_batch", model.get("threads", 8)),
                        "requests": model.get("requests", 1),
                        "cpu_mask": model.get("cpu_mask"),
                    }
                )
            return specs
        threads = 4 if self.args.local else 8
        requests = self.args.concurrency // 2
        a = {
            "model": "A",
            "port": self.args.port_a,
            "model_path": self.args.model_a,
            "threads": threads,
            "threads_batch": threads,
            "requests": requests,
            "cpu_mask": None,
        }
        b = {
            "model": "B",
            "port": self.args.port_b,
            "model_path": self.args.model_b,
            "threads": threads,
            "threads_batch": threads,
            "requests": requests,
            "cpu_mask": None,
        }
        return [a, b] if direction == "ab" else [b, a]

    def _workload_command(self, run_dir, direction):
        workload = SCRIPT_DIR / "workload.py"
        if self.args.scenario == "multi":
            endpoints = []
            for model in self.models:
                endpoints.extend(
                    [
                        "--endpoint",
                        f"{model['id']}:http://{self.args.host}:{model['port']}:"
                        f"{model.get('requests', 1)}",
                    ]
                )
            command = [
                sys.executable,
                str(workload),
                "--mode",
                "multi",
                "--events",
                str(run_dir / "events.jsonl"),
                "--output",
                str(run_dir / "workload.json"),
                "--output-tokens",
                str(self.args.output_tokens),
                "--lead-sec",
                str(self.args.lead_sec),
                "--seed",
                str(self.args.seed),
                "--request-timeout",
                str(self.args.request_timeout),
                "--prompts",
                str(self.args.prompts_file),
                *endpoints,
            ]
            return command

        incumbent = "A" if direction == "ab" else "B"
        injected = "B" if direction == "ab" else "A"
        inc_port = self.args.port_a if incumbent == "A" else self.args.port_b
        inj_port = self.args.port_b if injected == "B" else self.args.port_a
        command = [
            sys.executable,
            str(workload),
            "--mode",
            "dual",
            "--scenario",
            DUAL_SCENARIOS[self.args.scenario],
            "--incumbent-url",
            f"http://{self.args.host}:{inc_port}",
            "--injected-url",
            f"http://{self.args.host}:{inj_port}",
            "--incumbent-model",
            incumbent,
            "--injected-model",
            injected,
            "--prompts",
            str(self.args.prompts_file),
            "--request-count",
            str(self.args.concurrency // 2),
            "--events",
            str(run_dir / "events.jsonl"),
            "--output",
            str(run_dir / "workload.json"),
            "--output-tokens",
            str(self.args.output_tokens),
            "--arrival-gap-sec",
            str(self.args.arrival_gap_sec),
            "--lead-sec",
            str(self.args.lead_sec),
            "--seed",
            str(self.args.seed),
            "--request-timeout",
            str(self.args.request_timeout),
            "--milestone-timeout",
            str(self.args.milestone_timeout),
        ]
        if self.args.scenario == "dual-async":
            command.extend(["--injection-token", str(self.args.injection_token)])
        return command

    def start_sampler(self, run_tag):
        procs = [f"{model}:{pid}" for model, pid in self.servers.pids.items()]
        argv = ["python3", f"{self.remote_dir}/remote_sampler.py", "--output",
                f"{self.remote_dir}/{run_tag}_sampler.jsonl",
                "--interval", str(self.args.sample_interval)]
        for proc in procs:
            argv.extend(["--proc", proc])
        exec_command = "exec " + " ".join(shlex.quote(value) for value in argv)
        self.sampler_pid = start_background(
            self.remote, f"sh -c {shlex.quote(exec_command)}",
            f"{self.remote_dir}/{run_tag}_sampler.log",
        )

    def stop_sampler(self):
        tracked = self.sampler_pid
        stop_pid(self.remote, tracked, grace=3)
        for pid in pids_with_cmdline_arg(self.remote, f"{self.remote_dir}/remote_sampler.py"):
            if pid != tracked:
                stop_pid(self.remote, pid, grace=1)
        self.sampler_pid = None

    def start_traces(self, run_tag):
        if not self.args.rvv_trace:
            return
        duration = int(self.args.request_timeout + self.args.milestone_timeout + 60)
        options = "--rvv-only"
        if self.args.rvv_futex_time:
            options += " --futex-time"
        lib_dir = shlex.quote(self.args.rvv_remote_lib_dir)
        remote_bin = shlex.quote(self.args.rvv_remote_bin)
        for model, pid in self.servers.pids.items():
            slug = str(model).lower()
            command = (
                f"/usr/bin/env LD_LIBRARY_PATH={lib_dir} {remote_bin} "
                f"--tgid {pid} --duration {duration} {options} "
                f"--json {self.remote_dir}/{run_tag}_rvv_{slug}.json"
            )
            self.trace_pids[model] = start_background(
                self.remote, command, f"{self.remote_dir}/{run_tag}_rvv_{slug}.log"
            )

    def stop_traces(self):
        for model in list(self.trace_pids):
            stop_pid(self.remote, self.trace_pids.pop(model), grace=10)

    def validate_traces(self, run_dir, expected_pids):
        if not self.args.rvv_trace:
            return {"required": False, "valid": True, "models": {}}
        valid = True
        details = {"required": True, "models": {}}
        for model in self._server_labels():
            slug = str(model).lower()
            path = run_dir / f"rvv_{slug}.json"
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
            if self.args.rvv_futex_time:
                item_valid = bool(
                    item_valid and item["sample_futex_time"] is True
                )
            item["valid"] = item_valid
            details["models"][model] = item
            valid = valid and item_valid
        details["valid"] = valid
        return details

    def collect_remote_files(self, run_dir, run_tag):
        mappings = {f"{run_tag}_sampler.jsonl": "sampler.jsonl"}
        for model in self.servers.pids or list(self._server_labels()):
            slug = str(model).lower()
            mappings[f"{run_tag}_server_{slug}.log"] = f"server_{slug}.log"
            mappings[f"{run_tag}_rvv_{slug}.json"] = f"rvv_{slug}.json"
            mappings[f"{run_tag}_rvv_{slug}.log"] = f"rvv_{slug}.log"
        for remote_name, local_name in mappings.items():
            self.remote.download(
                f"{self.remote_dir}/{remote_name}", run_dir / local_name, optional=True
            )

    def _server_labels(self):
        if self.args.scenario == "multi":
            return [model["id"] for model in self.models]
        return ["A", "B"]

    def validate_platform(self, run_dir):
        sampler_path = run_dir / "sampler.jsonl"
        frequencies, temperatures, available_memory = [], [], []
        if sampler_path.exists():
            for line in sampler_path.read_text(errors="replace").splitlines():
                try:
                    platform = json.loads(line).get("platform", {})
                except json.JSONDecodeError:
                    continue
                frequencies.extend(
                    value for value in platform.get("frequencies_khz", {}).values()
                    if value is not None
                )
                temperatures.extend(
                    value for value in platform.get("temperatures_millic", {}).values()
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
        details["valid"] = bool(
            details["samples_present"]
            and details["frequency_min_khz"] >= self.args.min_frequency_khz
            and details["temperature_max_millic"] <= self.args.max_temperature_millic
            and details["memory_available_min_kib"] >= self.args.min_available_memory_kib
        )
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
            "scenario": self.args.scenario,
        }
        (run_dir / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
        print(f"[INFO] starting {run_tag}")

        dmesg_before = self.remote.run("dmesg", check=False).stdout
        (run_dir / "dmesg_before.log").write_text(dmesg_before)
        return_code = 1
        run_pids = {}
        controller_alive = self.strategy is None
        try:
            if self.strategy is not None and self.strategy.starts_before_servers:
                self.strategy.start(run_tag)
            overrides = self._server_overrides()
            for spec in self._server_specs(direction):
                self.servers.start(
                    spec["model"], spec["port"], spec["model_path"], spec["threads"],
                    spec["threads_batch"], spec["requests"], run_tag, spec["cpu_mask"],
                    overrides,
                )
            run_pids = dict(self.servers.pids)
            if self.strategy is not None and not self.strategy.starts_before_servers:
                self.strategy.start(run_tag, run_pids, self._server_specs(direction))
            self.servers.warmup(
                [
                    {"label": spec["model"], "port": spec["port"], "requests": spec["requests"]}
                    for spec in self._server_specs(direction)
                ]
            )
            if self.strategy is not None and hasattr(self.strategy, "post_warmup"):
                self.strategy.post_warmup(run_dir, run_pids, self._server_specs(direction))
            self.start_sampler(run_tag)
            self.start_traces(run_tag)
            return_code = subprocess.run(self._workload_command(run_dir, direction)).returncode
            if self.strategy is not None:
                controller_alive = self.strategy.alive()
        except Exception as exc:  # noqa: BLE001 - record and keep the suite going
            (run_dir / "harness_error.txt").write_text(f"{type(exc).__name__}: {exc}\n")
            print(f"[ERROR] {run_tag}: {exc}")
        finally:
            self.stop_traces()
            self.stop_sampler()
            self.servers.stop_all()
            if self.strategy is not None:
                self.strategy.stop()
            dmesg_after = self.remote.run("dmesg", check=False).stdout
            (run_dir / "dmesg_after.log").write_text(dmesg_after)
            (run_dir / "dmesg_delta.log").write_text(dmesg_delta(dmesg_before, dmesg_after))
            self.collect_remote_files(run_dir, run_tag)
            if self.strategy is not None:
                self.strategy.collect(run_dir, run_tag)

        workload_path = run_dir / "workload.json"
        platform_validation = self.validate_platform(run_dir)
        trace_validation = self.validate_traces(run_dir, run_pids)
        control_validation = (
            self.strategy.validate(run_dir, run_pids)
            if self.strategy is not None
            else {"profile": "cfs", "valid": True, "checks": {}}
        )
        control_validation["controller_alive_at_workload_end"] = controller_alive
        if self.strategy is not None:
            control_validation["valid"] = bool(
                control_validation["valid"] and controller_alive
            )
        valid = False
        if return_code == 0 and workload_path.exists():
            workload = json.loads(workload_path.read_text())
            valid = bool(
                workload.get("valid", False)
                and platform_validation["valid"]
                and trace_validation["valid"]
                and control_validation["valid"]
                and not _kernel_failure(run_dir)
            )
        metadata.update(
            {
                "selected": valid,
                "return_code": return_code,
                "valid": valid,
                "platform_validation": platform_validation,
                "trace_validation": trace_validation,
                "control_validation": control_validation,
            }
        )
        (run_dir / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
        print(f"[INFO] completed {run_tag}: valid={valid}")
        return valid

    def run(self):
        self.prepare()
        failed = False
        directions = self._directions()
        try:
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
            self.stop_sampler()
            self.servers.stop_all()
            if self.strategy is not None:
                self.strategy.stop()

        generate_suite(self.suite_dir)
        print(f"[INFO] suite results: {self.suite_dir}")
        return 2 if failed else 0

    def _directions(self):
        if self.args.scenario == "multi":
            return ["na"]
        if self.args.direction == "both":
            return ["ab", "ba"]
        return [self.args.direction]


def _kernel_failure(run_dir):
    path = run_dir / "dmesg_delta.log"
    if not path.exists():
        return False
    import re

    pattern = re.compile(
        r"(sched_ext.*(stall|error|watchdog)|runnable task stall|out of memory|oom-kill)",
        re.IGNORECASE,
    )
    return any(pattern.search(line) for line in path.read_text(errors="replace").splitlines())
