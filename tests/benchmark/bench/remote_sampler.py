#!/usr/bin/env python3
"""Low-frequency /proc and platform sampler for one or more llama-server processes.

Uploaded to the board and started as a background process. Each ``--proc``
argument is ``MODEL:PID``; the model label is copied into every task row so the
host-side metrics can group samples per model.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import time
from pathlib import Path


running = True


def stop(_signum, _frame):
    global running
    running = False


def read_text(path):
    try:
        return Path(path).read_text(errors="replace")
    except OSError:
        return ""


def read_int(path):
    try:
        return int(read_text(path).strip())
    except ValueError:
        return None


def parse_status(path):
    result = {}
    for line in read_text(path).splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            result[key] = value.strip()
    return result


def parse_migrations(path):
    for line in read_text(path).splitlines():
        if line.lstrip().startswith("se.nr_migrations") and ":" in line:
            try:
                return int(line.split(":", 1)[1].strip())
            except ValueError:
                return None
    return None


def task_samples(model, pid):
    task_dir = Path(f"/proc/{pid}/task")
    try:
        tids = sorted(task_dir.iterdir(), key=lambda item: int(item.name))
    except OSError:
        return []

    rows = []
    for tid_dir in tids:
        schedstat = read_text(tid_dir / "schedstat").split()
        if len(schedstat) < 3:
            continue
        status = parse_status(tid_dir / "status")
        try:
            rows.append(
                {
                    "model": model,
                    "pid": pid,
                    "tid": int(tid_dir.name),
                    "comm": read_text(tid_dir / "comm").strip(),
                    "runtime_ns": int(schedstat[0]),
                    "runqueue_wait_ns": int(schedstat[1]),
                    "timeslices": int(schedstat[2]),
                    "voluntary_ctxt": int(status.get("voluntary_ctxt_switches", 0)),
                    "nonvoluntary_ctxt": int(status.get("nonvoluntary_ctxt_switches", 0)),
                    "migrations": parse_migrations(tid_dir / "sched"),
                    "allowed_cpus": status.get("Cpus_allowed_list", ""),
                }
            )
        except ValueError:
            continue
    return rows


def platform_sample():
    frequencies = {}
    for path in sorted(Path("/sys/devices/system/cpu/cpufreq").glob("policy*/scaling_cur_freq")):
        frequencies[path.parent.name] = read_int(path)

    temperatures = {}
    for path in sorted(Path("/sys/class/thermal").glob("thermal_zone*/temp")):
        temperatures[path.parent.name] = read_int(path)

    meminfo = {}
    for line in read_text("/proc/meminfo").splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        if key in {"MemAvailable", "SwapFree", "SwapTotal"}:
            try:
                meminfo[key] = int(value.strip().split()[0])
            except (ValueError, IndexError):
                pass

    return {
        "frequencies_khz": frequencies,
        "temperatures_millic": temperatures,
        "memory_kib": meminfo,
        "sched_ext_state": read_text("/sys/kernel/sched_ext/state").strip(),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--proc",
        action="append",
        required=True,
        help="MODEL:PID to sample; may be repeated",
    )
    parser.add_argument("--output", required=True)
    parser.add_argument("--interval", type=float, default=1.0)
    args = parser.parse_args()

    procs = []
    for item in args.proc:
        model, _, pid = item.partition(":")
        procs.append((model, int(pid)))

    try:
        os.nice(19)
    except OSError:
        pass
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    next_sample = time.monotonic()
    with open(args.output, "w", buffering=1) as output:
        while running:
            now_ns = time.monotonic_ns()
            tasks = []
            for model, pid in procs:
                tasks.extend(task_samples(model, pid))
            record = {
                "monotonic_ns": now_ns,
                "wall_time_ns": time.time_ns(),
                "tasks": tasks,
                "platform": platform_sample(),
            }
            output.write(json.dumps(record, separators=(",", ":")) + "\n")
            next_sample += max(0.1, args.interval)
            delay = next_sample - time.monotonic()
            if delay > 0:
                time.sleep(delay)


if __name__ == "__main__":
    main()
