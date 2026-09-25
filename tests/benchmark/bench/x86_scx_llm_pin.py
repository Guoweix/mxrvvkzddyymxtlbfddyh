#!/usr/bin/env python3
"""Adapter for the ``experiments/x86_scx_llm_pin`` scheduler.

The x86_64 port of ``scx_llm_pin`` with SMT support. It is launched locally
after the servers, one ``--team PID:4:4:0xff`` per llama-server, with
a fixed spread placement (team0 -> CPU0-3, team1 -> CPU4-7). The RISC-V loader path is skipped: servers exec
the x86 runtime directly with ``GGML_SCX_LEASE_HINT_PATH`` pointing at the
fixed control directory, so the ABI v4 phase hints are published. Stats parsing
and validation are inherited unchanged from
:class:`bench.scx_llm_pin.ScxLlmPinStrategy`.
"""

from __future__ import annotations

import shlex
import time
from pathlib import Path

from .board import start_background
from .scx_llm_pin import ScxLlmPinStrategy


class X86ScxLlmPinStrategy(ScxLlmPinStrategy):
    name = "x86-scx-llm-pin"
    starts_before_servers = False

    def __init__(self, args, remote, remote_dir):
        self.args = args
        self.remote = remote
        self.remote_dir = remote_dir
        self.local_bin = Path(args.x86_scx_llm_bin)
        self.remote_bin = str(self.local_bin)
        self.control_dir = args.x86_control_dir
        self.pid = None

    def prepare(self):
        if not self.local_bin.is_file():
            raise RuntimeError(f"x86 scx_llm binary not found: {self.local_bin}")

    def server_overrides(self):
        return {
            "direct": True,
            "runtime_bin": self.args.x86_runtime_bin,
            "library_path": self.args.x86_runtime_bin,
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
                f"--team {int(pid)}:{int(self.args.x86_threads)}:"
                f"{int(self.args.x86_threads)}:{mask:#x}"
            )
        return teams

    def start(self, run_tag, pids=None, specs=None):
        if not pids or not specs:
            raise RuntimeError("x86-scx-llm-pin start requires server PIDs and specs")
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
                raise RuntimeError(f"x86_scx_llm exited during startup:\n{log[-2000:]}")
            time.sleep(0.25)
        raise RuntimeError("x86_scx_llm attach timeout")
