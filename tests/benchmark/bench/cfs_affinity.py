#!/usr/bin/env python3
"""CFS lever strategy: worker affinity + long base slice + short GOMP spin.

Reproduces the ``experiments/cfs_slice_spin_compare`` affinity case:
- pin each server's eight busiest workers one-per-CPU (0..7), so every CPU
  hosts one worker of each server;
- set the kernel CFS ``base_slice_ns`` to ``--cfs-affinity-slice-ms`` (default
  100ms) for the measured window and restore it afterwards;
- start the servers with ``GOMP_SPINCOUNT`` set to ``--cfs-affinity-spincount``
  (default 1000).

No sched_ext is loaded.
"""

from __future__ import annotations

import json
import shlex
from pathlib import Path


BASE_SLICE_PATH = "/sys/kernel/debug/sched/base_slice_ns"


class CfsAffinityStrategy:
    name = "cfs-affinity"
    starts_before_servers = False

    def __init__(self, args, remote, remote_dir):
        self.args = args
        self.remote = remote
        self.remote_dir = remote_dir
        self.original_slice = None
        self.affinity = None

    def prepare(self):
        probe = self.remote.run(f"cat {BASE_SLICE_PATH} 2>/dev/null || true", check=False)
        if not probe.stdout.strip().isdigit():
            raise RuntimeError(f"{BASE_SLICE_PATH} not available on the board")
        self.original_slice = probe.stdout.strip()

    def server_overrides(self):
        return {
            "extra_env": {"GOMP_SPINCOUNT": str(self.args.cfs_affinity_spincount)}
        }

    def start(self, run_tag, pids=None, specs=None):
        pass

    def _pin_workers(self, pids):
        code = (
            "import json,os\n"
            f"pids={json.dumps(pids)}\n"
            "result={}\n"
            "for model,pid in pids.items():\n"
            " rows=[]\n"
            " for name in os.listdir(f'/proc/{pid}/task'):\n"
            "  if not name.isdigit(): continue\n"
            "  try:\n"
            "   runtime=int(open(f'/proc/{pid}/task/{name}/schedstat').read().split()[0])\n"
            "  except (OSError,ValueError,IndexError): continue\n"
            "  rows.append((runtime,int(name)))\n"
            " selected=sorted(rows,reverse=True)[:8]\n"
            " if len(selected)!=8: raise RuntimeError(f'{model}: expected 8 workers')\n"
            " result[model]=[]\n"
            " for cpu,(runtime,tid) in enumerate(selected):\n"
            "  os.sched_setaffinity(tid,{cpu})\n"
            "  result[model].append({'tid':tid,'cpu':cpu,'runtime_ns':runtime,'affinity':sorted(os.sched_getaffinity(tid))})\n"
            "print(json.dumps(result))\n"
        )
        result = self.remote.run(f"python3 -c {shlex.quote(code)}")
        return result.stdout

    def post_warmup(self, run_dir, pids, specs):
        self.affinity = self._pin_workers(pids)
        (run_dir / "affinity.json").write_text(self.affinity)
        slice_ns = int(self.args.cfs_affinity_slice_ms) * 1_000_000
        self.remote.run(f"sh -c 'echo {slice_ns} > {BASE_SLICE_PATH}'")
        got = self.remote.run(f"cat {BASE_SLICE_PATH}").stdout.strip()
        if got != str(slice_ns):
            raise RuntimeError(f"base_slice_ns write did not stick: {got} != {slice_ns}")

    def alive(self):
        return True

    def stop(self):
        if self.original_slice is not None:
            self.remote.run(
                f"sh -c 'echo {int(self.original_slice)} > {BASE_SLICE_PATH}'", check=False
            )
            self.original_slice = None

    def collect(self, run_dir, run_tag):
        pass

    def validate(self, run_dir, expected_pids):
        return {"profile": self.name, "valid": True, "checks": {}}
