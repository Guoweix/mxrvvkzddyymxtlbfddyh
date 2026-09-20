#!/usr/bin/env python3
"""Board test: eight actual runtime/OpenMP clients under sched_ext."""

import csv
import datetime
import json
import os
from pathlib import Path
import re
import time
import uuid

from run_multi_benchmark import Remote, HERE, save, validate_controller
from run_benchmark import BUILD


def main():
    remote = Remote(
        os.environ.get("REMOTE_IP", "192.168.3.67"),
        os.environ.get("REMOTE_USER", "root"),
    )
    name = (
        "multi-probe-"
        + datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ-")
        + uuid.uuid4().hex[:8]
    )
    output, directory = HERE / "results" / name, "/tmp/" + name
    output.mkdir(parents=True)
    report = {"passed": False, "processes": 8}
    ctl, pids, before = None, [], None
    try:
        remote.idle()
        before = remote.shell("dmesg")
        remote.shell(f"mkdir {directory}")
        for binary in ("dt_controller", "dt_phase_probe"):
            remote.put(BUILD / binary, f"{directory}/{binary}")
        sock = directory + "/control.sock"
        ctl = remote.start(
            f"{directory}/dt_controller --socket {sock} --mode dynamic --scx "
            "--grant-lifetime persistent --allocation-policy prefill-first "
            f"--decode-wait-ms 0 --decode-floor-on-prefill --events {directory}/events.csv",
            directory + "/controller.log",
        )
        deadline = time.monotonic() + 10
        while remote.command(remote.ssh + [f"test -S {sock}"], check=False).returncode:
            if time.monotonic() > deadline:
                raise RuntimeError("controller did not start")
            time.sleep(0.05)
        for i in range(8):
            pids.append(
                remote.start(
                    f"{directory}/dt_phase_probe {sock} 600 {100 if i % 2 else 0} 10000",
                    f"{directory}/probe{i}.log",
                )
            )
        checks = " && ".join(
            f"grep -q '^PASS ' {directory}/probe{i}.log" for i in range(8)
        )
        remote.shell(
            f"n=0; until {checks}; do n=$((n+1)); [ $n -lt 120 ] || exit 1; sleep .5; done",
            timeout=65,
        )
        report["probes_completed"] = True
    except Exception as exc:
        report["failure"] = str(exc)
    finally:
        errors = []
        for pid in pids + ([ctl] if ctl else []):
            try:
                remote.stop(pid, directory)
            except Exception as exc:
                errors.append(str(exc))
        if ctl:
            try:
                for filename in ["events.csv", "controller.log"] + [
                    f"probe{i}.log" for i in range(len(pids))
                ]:
                    remote.fetch(directory + "/" + filename, output / filename)
                with (output / "events.csv").open() as stream:
                    rows = list(csv.DictReader(stream))
                servers = [{"pid": pid} for pid in pids]
                report["controller"] = validate_controller(
                    rows, (output / "controller.log").read_text(), servers, False
                )
                published, peak = set(), 0
                for row in rows:
                    if row["event"] == "MEMBERS":
                        published.add(row["slot"])
                        peak = max(peak, len(published))
                    elif row["event"] in ("UNMEMBERS", "EXIT"):
                        published.discard(row["slot"])
                report["peak_computing_processes"] = peak
                if peak != 8:
                    raise RuntimeError(f"only {peak} simultaneously registered teams")
                if not report["controller"]["ownership"]["shrinks_checked"]:
                    raise RuntimeError("no partial shrink exercised")
            except Exception as exc:
                errors.append(str(exc))
        if before is not None:
            try:
                after = remote.shell("dmesg")
                if not after.startswith(before):
                    raise RuntimeError("kernel log wrapped")
                delta = after[len(before) :]
                (output / "dmesg-delta.log").write_text(delta)
                if re.search(
                    r"BUG:|WARNING:|Oops:|watchdog.*(stall|lockup)|Out of memory|hung task",
                    delta,
                    re.I,
                ):
                    raise RuntimeError("kernel warning/error")
                remote.idle()
            except Exception as exc:
                errors.append(str(exc))
        report["errors"] = errors
        report["passed"] = bool(
            report.get("probes_completed") and not errors and not report.get("failure")
        )
        save(output / "summary.json", report)
    print(json.dumps(report, indent=2))
    print(f"Evidence: {output}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
