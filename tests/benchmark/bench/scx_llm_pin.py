#!/usr/bin/env python3
"""Adapter for the ``experiments/scx_llm_pin`` scheduler.

`scx_llm_pin` is a sched_ext scheduler with a single `rvv_dirty_local` base and
automatic phase-aware scheduling that engages only when the ABI v4 userspace
runtime publishes valid phase hints. This adapter deploys and launches the
loader with one `--team PID:decode:prefill[:mask]` per llama-server, optionally
enabling the userspace hint path on the servers, then reads back the scheduler
stats for validation.

No ABI here; the versioned worker page lives in the experiment's
``scx_rvv_lease_control.h``.
"""

from __future__ import annotations

import re
import shlex
import time
from pathlib import Path

from .board import start_background, stop_pid


REPO_DIR = Path(__file__).resolve().parents[3]
SCX_DIR = REPO_DIR / "src" / "scx_llm_pin"
DEFAULT_BINARY = SCX_DIR / "scx_llm_pin"
DEFAULT_HINT_RUNTIME = "/mnt/llm/llama/bin-nteam-v4"
DEFAULT_CONTROL_DIR = "/sys/fs/bpf/scx_rvv_dirty_lease"

KV = re.compile(r"([a-zA-Z0-9_]+)=([0-9]+)")


def _parse_kv(line):
    return {key: int(value) for key, value in KV.findall(line)}


class ScxLlmPinStrategy:
    name = "scx-llm-pin"
    starts_before_servers = False

    def __init__(self, args, remote, remote_dir):
        self.args = args
        self.remote = remote
        self.remote_dir = remote_dir
        self.local_bin = Path(args.scx_llm_pin_bin)
        self.remote_bin = f"{remote_dir}/scx_llm_pin"
        self.control_dir = args.scx_control_dir
        self.pid = None

    def prepare(self):
        if not self.local_bin.is_file():
            raise RuntimeError(f"scx_llm_pin binary not found: {self.local_bin}")
        state = self.remote.run(
            "cat /sys/kernel/sched_ext/state 2>/dev/null || true", check=False
        ).stdout.strip()
        if state and state != "disabled":
            raise RuntimeError(f"sched_ext is already {state!r}; scx-llm-pin needs it free")
        if self.args.user_hint == "on":
            probe = self.remote.run(
                f"test -x {shlex.quote(self.args.scx_hint_runtime_bin)}/llama-server",
                check=False,
            )
            if probe.returncode:
                raise RuntimeError(
                    f"ABI v4 hint runtime not found on board: "
                    f"{self.args.scx_hint_runtime_bin}/llama-server"
                )
        self.remote.upload(self.local_bin, self.remote_bin)
        self.remote.run(f"chmod +x {shlex.quote(self.remote_bin)}")

    def server_overrides(self):
        if self.args.user_hint == "off":
            return None
        runtime_bin = self.args.scx_hint_runtime_bin
        return {
            "runtime_bin": runtime_bin,
            "library_path": f"{runtime_bin}:{self.args.llm_path}/bin",
            "loader": f"{self.args.llm_path}/bin/ld-linux-riscv64-lp64d.so.1",
            "extra_env": {
                "GGML_SCX_LEASE_HINT_PATH": f"{self.control_dir}/workers",
            },
        }

    def _team_args(self, pids, specs):
        by_model = {spec["model"]: spec for spec in specs}
        teams = []
        for model, pid in pids.items():
            spec = by_model[model]
            mask = spec.get("cpu_mask")
            mask = 0xFF if mask is None else int(mask)
            teams.append(
                f"--team {int(pid)}:{int(spec['threads'])}:"
                f"{int(spec['threads_batch'])}:{mask:#x}"
            )
        return teams

    def start(self, run_tag, pids=None, specs=None):
        if not pids or not specs:
            raise RuntimeError("scx-llm-pin start requires server PIDs and specs")
        options = [
            *self._team_args(pids, specs),
            f"--vector-slice-ms {self.args.scx_vector_slice_ms}",
            f"--vector-local-window-ms {self.args.scx_vector_local_window_ms}",
            f"--dirty-threshold {self.args.scx_dirty_threshold}",
            f"--phase-hint-timeout-ms {self.args.scx_phase_hint_timeout_ms}",
            f"--prefill-starvation-ms {self.args.scx_prefill_starvation_ms}",
            f"--phase-wakeup-preempt {self.args.scx_phase_wakeup_preempt}",
        ]
        command = f"{shlex.quote(self.remote_bin)} " + " ".join(options)
        self.pid = start_background(
            self.remote, command, f"{self.remote_dir}/{run_tag}_scx.log"
        )
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            log = self.remote.run(
                f"cat {shlex.quote(self.remote_dir)}/{run_tag}_scx.log",
                check=False,
            ).stdout
            if "attached successfully" in log:
                return
            if self.remote.run(f"kill -0 {int(self.pid)} 2>/dev/null", check=False).returncode:
                raise RuntimeError(f"scx_llm_pin exited during startup:\n{log[-2000:]}")
            time.sleep(0.25)
        raise RuntimeError("scx_llm_pin attach timeout")

    def alive(self):
        if not self.pid:
            return False
        return self.remote.run(
            f"kill -0 {int(self.pid)} 2>/dev/null", check=False
        ).returncode == 0

    def stop(self):
        stop_pid(self.remote, self.pid, grace=10)
        self.pid = None

    def collect(self, run_dir, run_tag):
        self.remote.download(
            f"{self.remote_dir}/{run_tag}_scx.log", run_dir / "scheduler.log", optional=True
        )

    def validate(self, run_dir, expected_pids):
        details = {
            "profile": self.name,
            "valid": False,
            "checks": {},
            "phase_hint_mode": self.args.user_hint,
        }
        path = run_dir / "scheduler.log"
        text = path.read_text(errors="replace") if path.exists() else ""
        details["attached"] = "scx_llm_pin attached successfully" in text
        details["detached"] = "scx_llm_pin detached" in text

        rvv = {}
        phase = {}
        for line in text.splitlines():
            if line.startswith("SCX_RVV_STATS "):
                rvv = _parse_kv(line)
            elif line.startswith("SCX_PHASE_STATS "):
                phase = _parse_kv(line)
        scx = {
            "phase_reads": phase.get("phase_reads"),
            "phase_valid": phase.get("phase_valid"),
            "phase_invalid": phase.get("phase_invalid"),
            "phase_prefill": phase.get("phase_prefill"),
            "phase_decode": phase.get("phase_decode"),
            "phase_idle": phase.get("phase_idle"),
            "phase_mixed": phase.get("phase_mixed"),
            "lease_engagements": phase.get("engagements"),
            "lease_preferred": phase.get("preferred"),
            "lease_safety_forced": phase.get("safety_forced"),
            "lease_pending_max_ns": phase.get("pending_max_ns"),
            "wakeup_candidates": phase.get("wakeup_candidates"),
            "wakeup_preempts": phase.get("wakeup_preempts"),
            "vector_dispatches": rvv.get("vector_dispatches"),
            "vector_dsq_dispatches": rvv.get("vector_dsq_dispatches"),
            "vector_dsq_consumes": rvv.get("vector_dsq_consumes"),
            "local_dispatches": rvv.get("local_dispatches"),
            "window_starts": rvv.get("window_starts"),
        }
        details["scx"] = scx

        checks = {
            "attached": details["attached"],
            "vector_work_seen": bool((scx["vector_dispatches"] or 0) > 0),
            "consumed": bool((scx["vector_dsq_consumes"] or 0) > 0),
        }
        if self.args.user_hint == "on":
            checks["hint_seen"] = bool((scx["phase_valid"] or 0) > 0)
        else:
            checks["no_hint"] = bool((scx["phase_valid"] or 0) == 0)
        details["checks"] = checks
        details["valid"] = all(checks.values())
        return details
