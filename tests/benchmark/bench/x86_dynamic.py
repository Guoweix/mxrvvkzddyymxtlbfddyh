#!/usr/bin/env python3
"""Adapter for the ``experiments/x86_dynamic_threads`` strategy.

Same controller protocol as :mod:`bench.dynamic_threads`, but the runtime and
controller are the x86_64 builds that run on this host. The controller is
launched locally, the servers exec the x86 runtime directly (no RISC-V
``ld-linux`` loader), and event-trace validation is inherited unchanged from
:class:`bench.dynamic_threads.DynamicThreadsStrategy`.
"""

from __future__ import annotations

import shlex
import time
from pathlib import Path

from .board import start_background
from .dynamic_threads import DynamicThreadsStrategy


class X86DynamicStrategy(DynamicThreadsStrategy):
    name = "x86-dynamic"
    starts_before_servers = True

    def __init__(self, args, remote, remote_dir):
        self.args = args
        self.remote = remote
        self.remote_dir = remote_dir
        self.runtime_bin = Path(args.x86_runtime_bin)
        self.controller = Path(args.x86_dynamic_controller)
        self.socket = f"{remote_dir}/dynamic.sock"
        self.controller_pid = None
        self.events_remote = None

    def prepare(self):
        if not (self.runtime_bin / "llama-server").is_file():
            raise RuntimeError(f"x86 dynamic runtime not built: {self.runtime_bin}/llama-server")
        if not self.controller.is_file():
            raise RuntimeError(f"x86 dynamic controller not built: {self.controller}")

    def server_overrides(self):
        return {
            "direct": True,
            "runtime_bin": str(self.runtime_bin),
            "library_path": str(self.runtime_bin),
            "extra_env": {
                "OMP_DYNAMIC": "FALSE",
                "GGML_DYNAMIC_THREADS_SOCKET": self.socket,
                "GGML_DYNAMIC_THREADS_RETAIN_SHRINK": "1",
            },
        }

    def start(self, run_tag):
        self.events_remote = f"{self.remote_dir}/{run_tag}.events.csv"
        options = [
            f"--socket {shlex.quote(self.socket)}",
            "--mode dynamic",
            "--scx",
            "--grant-lifetime persistent",
            "--allocation-policy prefill-first",
            "--decode-wait-ms 0",
            "--decode-floor-on-prefill",
            f"--events {shlex.quote(self.events_remote)}",
        ]
        command = f"{shlex.quote(str(self.controller))} " + " ".join(options)
        self.controller_pid = start_background(
            self.remote, command, f"{self.remote_dir}/{run_tag}_controller.log"
        )
        deadline = time.monotonic() + 15
        while self.remote.run(f"test -S {shlex.quote(self.socket)}", check=False).returncode:
            if time.monotonic() >= deadline:
                raise RuntimeError("dt_controller socket did not appear")
            time.sleep(0.1)
