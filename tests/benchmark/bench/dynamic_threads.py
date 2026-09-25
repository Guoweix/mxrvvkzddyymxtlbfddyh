#!/usr/bin/env python3
"""Adapter for the ``experiments/dynamic_threads`` strategy.

The patched llama-server negotiates OpenMP team width with ``dt_controller``
over a Unix SOCK_SEQPACKET socket (``GGML_DYNAMIC_THREADS_SOCKET``) while a
sched_ext BPF program places the granted workers on custom DSQs. This module
deploys the runtime/controller, launches the controller, and reads back the
controller event trace for validation and metrics.

``event_metrics`` and ``validate_cpu_ownership`` are copied from
``experiments/dynamic_threads/{run_benchmark,control_evidence}.py`` to keep
benchmark_v2 self-contained; keep them in sync with the experiment.
"""

from __future__ import annotations

import csv
import math
import re
import shlex
import statistics
import tarfile
import tempfile
import time
from pathlib import Path

from .board import start_background, stop_pid


REPO_DIR = Path(__file__).resolve().parents[3]
WORK_DIR = REPO_DIR.parent / "experiments"
DYNAMIC_DIR = WORK_DIR / "dynamic_threads"
DEFAULT_RUNTIME_BIN = DYNAMIC_DIR / "llama.cpp" / "build-dynamic-riscv" / "bin"
DEFAULT_CONTROLLER = DYNAMIC_DIR / "build" / "dt_controller"

COUNTERS = re.compile(r"BPF enqueue=(\d+) placed=(\d+) running=(\d+) wrong_cpu=(\d+)")


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, math.ceil(fraction * len(ordered)) - 1)]


def validate_cpu_ownership(rows):
    held = {}
    grants = shrinks = 0
    for row in rows:
        slot, kind = row["slot"], row["event"]
        if kind == "GRANT":
            mask, workers = int(row["cpu_mask"]), int(row["workers"])
            if not 0 < mask <= 255 or bin(mask).count("1") != workers:
                raise RuntimeError(f"invalid grant width/mask: {row}")
            if slot in held or any(mask & other for other in held.values()):
                raise RuntimeError(f"CPU reused before RELEASE/EXIT: {row}, held={held}")
            held[slot] = mask
            grants += 1
        elif kind == "SHRINK":
            mask, workers = int(row["cpu_mask"]), int(row["workers"])
            previous = held.get(slot, 0)
            if (not mask or mask == previous or mask & ~previous or
                    bin(mask).count("1") != workers):
                raise RuntimeError(f"invalid partial return: {row}, held={held}")
            held[slot] = mask
            shrinks += 1
        elif kind in ("RELEASE", "EXIT"):
            held.pop(slot, None)
    if held:
        raise RuntimeError(f"unreleased grants at end of log: {held}")
    return {"grants_checked": grants, "shrinks_checked": shrinks, "overlap_free": True}


def event_metrics(rows):
    requests = {(row["slot"], row["epoch"]): int(row["time_ns"])
                for row in rows if row["event"] == "REQUEST"}
    grants = {(row["slot"], row["epoch"]): int(row["time_ns"])
              for row in rows if row["event"] == "GRANT"}
    members = {}
    for row in rows:
        if row["event"] == "MEMBERS":
            members.setdefault((row["slot"], row["epoch"]), int(row["time_ns"]))
    revoked, handoffs = {}, []
    for row in rows:
        key = row["slot"], row["epoch"]
        if row["event"] == "REVOKE":
            revoked[key] = int(row["time_ns"])
        elif row["event"] in ("RELEASE", "SHRINK") and key in revoked:
            handoffs.append((int(row["time_ns"]) - revoked.pop(key)) / 1000)
        elif row["event"] == "EXIT":
            revoked.pop(key, None)
    waits = [(grants[key] - value) / 1000 for key, value in requests.items() if key in grants]
    startups = [(members[key] - value) / 1000 for key, value in grants.items() if key in members]
    grant_rows = [row for row in rows if row["event"] == "GRANT"]
    allocation_rows = [row for row in rows if row["event"] in ("GRANT", "SHRINK")]
    decode_waits = [int(row.get("decode_wait_ns", 0)) / 1e6 for row in rows
                    if row["event"] == "DECODE_RESUME"]
    observed_waits = [int(row["decode_wait_ns"]) / 1e6 for row in rows if "decode_wait_ns" in row]
    recoveries = [int(row.get("decode_recovery_ns", 0)) / 1e6 for row in rows
                  if row["event"] == "DECODE_RESUME" and int(row.get("decode_recovery_ns", 0))]
    return {
        "grants": len(grant_rows),
        "shrinks": len(allocation_rows) - len(grant_rows),
        "workers": sorted({int(row["workers"]) for row in allocation_rows}),
        "protocol_messages": sum(row["event"] in ("WIRE_RX", "WIRE_TX") for row in rows),
        "member_updates": sum(row["event"] == "MEMBERS" for row in rows),
        "revocations": sum(row["event"] == "REVOKE" for row in rows),
        "phase_updates": sum(row["event"] == "PHASE" for row in rows),
        "decode_protections": sum(row["event"] == "DECODE_PROTECT" for row in rows),
        "decode_wait_p95_ms": percentile(decode_waits, .95),
        "decode_wait_max_ms": max(observed_waits, default=None),
        "decode_recovery_p95_ms": percentile(recoveries, .95),
        "decode_recovery_max_ms": max(recoveries, default=None),
        "decode_floor_targets": sum(row["event"] == "TARGET" and row.get("phase") == "2" and
                                    row.get("target_workers") == "1" for row in rows),
        "handoff_mean_us": statistics.fmean(handoffs) if handoffs else None,
        "handoff_p95_us": percentile(handoffs, .95),
        "cpu_masks": sorted({int(row["cpu_mask"]) for row in allocation_rows}),
        "grant_wait_mean_us": statistics.fmean(waits) if waits else None,
        "grant_wait_p95_us": percentile(waits, .95),
        "team_publish_mean_us": statistics.fmean(startups) if startups else None,
        "team_publish_p95_us": percentile(startups, .95),
    }


class DynamicThreadsStrategy:
    name = "dynamic"
    starts_before_servers = True

    def __init__(self, args, remote, remote_dir):
        self.args = args
        self.remote = remote
        self.remote_dir = remote_dir
        self.runtime_bin = Path(args.dynamic_runtime_bin)
        self.controller = Path(args.dynamic_controller)
        self.socket = f"{remote_dir}/dynamic.sock"
        self.controller_pid = None
        self.events_remote = None

    def prepare(self):
        if not (self.runtime_bin / "llama-server").is_file():
            raise RuntimeError(f"dynamic runtime not built: {self.runtime_bin}/llama-server")
        if not self.controller.is_file():
            raise RuntimeError(f"dynamic controller not built: {self.controller}")

        state = self.remote.run(
            "cat /sys/kernel/sched_ext/state 2>/dev/null || true", check=False
        ).stdout.strip()
        if state and state != "disabled":
            raise RuntimeError(f"sched_ext is already {state!r}; dynamic strategy needs it free")

        self.remote.run(
            f"mkdir -p {shlex.quote(self.remote_dir)}/bin"
        )
        with tempfile.NamedTemporaryFile(suffix=".tar") as archive:
            with tarfile.open(fileobj=archive, mode="w", dereference=False) as tar:
                for path in self.runtime_bin.iterdir():
                    tar.add(path, arcname=path.name, recursive=False)
            archive.flush()
            self.remote.upload(archive.name, f"{self.remote_dir}/runtime.tar")
        self.remote.upload(self.controller, f"{self.remote_dir}/dt_controller")
        self.remote.run(
            f"tar -C {shlex.quote(self.remote_dir)}/bin -xf "
            f"{shlex.quote(self.remote_dir)}/runtime.tar && "
            f"chmod +x {shlex.quote(self.remote_dir)}/dt_controller && "
            f"chmod +x {shlex.quote(self.remote_dir)}/bin/llama-server"
        )

    def server_overrides(self):
        return {
            "runtime_bin": f"{self.remote_dir}/bin",
            "library_path": f"{self.remote_dir}/bin:{self.args.llm_path}/bin",
            "loader": f"{self.args.llm_path}/bin/ld-linux-riscv64-lp64d.so.1",
            "extra_args": ("--fit", "off"),
            "extra_env": {
                "OMP_DYNAMIC": "FALSE",
                "GGML_DYNAMIC_THREADS_SOCKET": self.socket,
                "GGML_DYNAMIC_THREADS_RETAIN_SHRINK": (
                    "1" if self.args.dynamic_retain_shrink else "0"
                ),
            },
        }

    def start(self, run_tag):
        self.events_remote = f"{self.remote_dir}/{run_tag}.events.csv"
        options = [
            f"--socket {shlex.quote(self.socket)}",
            f"--mode {self.args.dynamic_mode}",
            "--scx",
            f"--grant-lifetime {self.args.dynamic_grant_lifetime}",
            f"--allocation-policy {self.args.dynamic_allocation_policy}",
            f"--decode-wait-ms {self.args.dynamic_decode_wait_ms}",
        ]
        if self.args.dynamic_decode_floor_on_prefill:
            options.append("--decode-floor-on-prefill")
        if self.args.dynamic_slice_ms:
            options.append(f"--slice-ms {self.args.dynamic_slice_ms}")
        command = (
            f"{shlex.quote(self.remote_dir)}/dt_controller "
            + " ".join(options)
            + f" --events {shlex.quote(self.events_remote)}"
        )
        self.controller_pid = start_background(
            self.remote, command, f"{self.remote_dir}/{run_tag}_controller.log"
        )
        deadline = time.monotonic() + 15
        while self.remote.run(f"test -S {shlex.quote(self.socket)}", check=False).returncode:
            if time.monotonic() >= deadline:
                raise RuntimeError("dt_controller socket did not appear")
            time.sleep(0.1)

    def alive(self):
        if not self.controller_pid:
            return False
        return self.remote.run(
            f"kill -0 {int(self.controller_pid)} 2>/dev/null", check=False
        ).returncode == 0

    def stop(self):
        stop_pid(self.remote, self.controller_pid)
        self.controller_pid = None

    def collect(self, run_dir, run_tag):
        for remote_name, local_name in (
            (f"{run_tag}.events.csv", "controller_events.csv"),
            (f"{run_tag}_controller.log", "controller.log"),
        ):
            self.remote.download(
                f"{self.remote_dir}/{remote_name}", run_dir / local_name, optional=True
            )

    def validate(self, run_dir, expected_pids):
        details = {"profile": self.name, "valid": False, "checks": {}}
        log_path = run_dir / "controller.log"
        events_path = run_dir / "controller_events.csv"
        log = log_path.read_text(errors="replace") if log_path.exists() else ""
        details["controller_exit_ok"] = "status=OK" in log
        counters = COUNTERS.search(log)
        details["counters"] = counters.groups() if counters else None
        if not events_path.exists():
            details["error"] = "controller event trace missing"
            return details

        with events_path.open() as stream:
            rows = list(csv.DictReader(stream))
        details["events"] = len(rows)
        try:
            details["ownership"] = validate_cpu_ownership(rows)
        except RuntimeError as exc:
            details["error"] = str(exc)
            return details
        details["control"] = event_metrics(rows)
        hello = {int(row["pid"]): int(row["slot"]) for row in rows if row["event"] == "HELLO"}
        details["registered"] = len(hello)
        details["expected"] = len(expected_pids)
        observed = {int(row["pid"]) for row in rows if row["event"] == "MEMBERS"}
        details["computed_all"] = set(expected_pids.values()).issubset(observed)
        checks = {
            "controller_exit_ok": details["controller_exit_ok"],
            "bpf_placed": bool(counters and int(counters.group(2)) > 0),
            "bpf_running": bool(counters and int(counters.group(3)) > 0),
            "no_wrong_cpu": bool(counters and int(counters.group(4)) == 0),
            "ownership_overlap_free": details["ownership"]["overlap_free"],
            "all_registered": details["registered"] == len(expected_pids),
            "all_computed": details["computed_all"],
        }
        details["checks"] = checks
        details["valid"] = all(checks.values())
        return details
