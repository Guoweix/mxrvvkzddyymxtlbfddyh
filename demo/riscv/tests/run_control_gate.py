#!/usr/bin/env python3
"""Build and validate the event-driven controller on the target board."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import uuid

import config_env
from control_evidence import validate_cpu_ownership

HERE = Path(__file__).resolve().parent
# demo：使用预编译二进制（demo/riscv/bin），无需源码/编译。
SRC = HERE.parent
BUILD = SRC / "bin"


def main():
    config_env.ensure_remote_password()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=config_env.value("REMOTE_IP"))
    parser.add_argument("--user", default=config_env.value("REMOTE_USER"))
    parser.add_argument("--iterations", type=int, default=1000)
    parser.add_argument(
        "--grant-lifetime", choices=("graph", "persistent"), default="graph"
    )
    parser.add_argument(
        "--allocation-policy", choices=("equal", "prefill-first"), default="equal"
    )
    parser.add_argument(
        "--decode-wait-ms",
        type=int,
        default=0,
        help="0 tests strict priority; positive values test sticky decode protection",
    )
    parser.add_argument("--decode-floor-on-prefill", action="store_true")
    args = parser.parse_args()
    if args.allocation_policy != "equal" and args.grant_lifetime != "persistent":
        parser.error("prefill-first requires persistent")
    if not 4 <= args.iterations <= 100000:
        parser.error("iterations must be 4..100000")
    if not 0 <= args.decode_wait_ms <= 10000:
        parser.error("probe decode-wait-ms must be 0..10000")
    if (
        args.decode_wait_ms or args.decode_floor_on_prefill
    ) and args.allocation_policy != "prefill-first":
        parser.error("decode protection requires prefill-first")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_id = f"control-{stamp}-{uuid.uuid4().hex[:8]}"
    result = HERE / "results" / run_id
    result.mkdir(parents=True)
    build = BUILD
    build.mkdir(exist_ok=True)
    env = os.environ.copy()
    prefix = []
    if env.get("REMOTE_PASS"):
        env["SSHPASS"] = env.pop("REMOTE_PASS")
        prefix = ["sshpass", "-e"]
    target = f"{args.user}@{args.host}"
    options = ["-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=accept-new"]
    ssh = prefix + ["ssh", *options, target]
    scp = prefix + ["scp", "-O", *options]
    remote = f"/tmp/{run_id}"
    report = {
        "gate_passed": False,
        "run_id": run_id,
        "host": args.host,
        "iterations_per_probe": args.iterations,
        "remote_dir": remote,
    }
    report["grant_lifetime"] = args.grant_lifetime
    report["allocation_policy"] = args.allocation_policy
    report["decode_wait_ms"] = args.decode_wait_ms
    report["decode_floor_on_prefill"] = args.decode_floor_on_prefill

    def run(command, filename, timeout=90, check=True):
        with (result / filename).open("wb") as stream:
            completed = subprocess.run(
                command,
                env=env,
                stdout=stream,
                stderr=subprocess.STDOUT,
                timeout=timeout,
            )
        if check and completed.returncode:
            raise RuntimeError(f"{filename}: exit {completed.returncode}")
        return completed.returncode, (result / filename).read_text(errors="replace")

    before = None
    try:
        _, preflight = run(
            ssh
            + [
                "uname -a; cat /sys/devices/system/cpu/online; "
                "cat /sys/kernel/debug/sched/ext; "
                "test -r /sys/kernel/btf/vmlinux"
            ],
            "preflight.log",
        )
        if not re.search(r"^enabled\s*:\s*0\s*$", preflight, re.M):
            raise RuntimeError("sched_ext already enabled; board left untouched")
        _, before = run(ssh + ["dmesg"], "dmesg-before.log")
        with (build / "vmlinux.btf").open("wb") as stream:
            subprocess.run(
                ssh + ["cat /sys/kernel/btf/vmlinux"],
                env=env,
                stdout=stream,
                check=True,
                timeout=30,
            )
        # demo 使用预编译二进制，跳过编译。
        binaries = [
            build / name
            for name in (
                "dt_controller",
                "dt_probe",
                "dt_persistent_probe",
                "dt_phase_probe",
            )
        ]
        report["sha256"] = {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in binaries
        }
        run(ssh + [f"mkdir {shlex.quote(remote)}"], "deploy-mkdir.log")
        run(
            scp + [*(str(p) for p in binaries), f"{target}:{remote}/"],
            "deploy-control.log",
        )

        def case(name, mode, probes, compute_us, pattern=None, phase_prefills=None):
            sock = f"{remote}/{name}.sock"
            ctl = f"{remote}/{name}.controller.log"
            cmds = [
                "set -eu",
                f"{remote}/dt_controller --socket {sock} --mode {mode} --scx "
                f"--grant-lifetime {args.grant_lifetime} --allocation-policy {args.allocation_policy} "
                f"--decode-wait-ms {args.decode_wait_ms} "
                f"{'--decode-floor-on-prefill ' if args.decode_floor_on_prefill else ''}"
                f"--events {remote}/{name}.events.csv >{ctl} 2>&1 &",
                "controller_pid=$!",
                "trap 'kill -TERM $controller_pid 2>/dev/null || true' EXIT INT TERM",
                f"count=0; while [ ! -S {sock} ]; do count=$((count+1)); "
                f"[ $count -lt 100 ] || {{ cat {ctl}; exit 1; }}; sleep .05; done",
            ]
            for index in range(probes):
                binary = "dt_persistent_probe" if pattern else "dt_probe"
                suffix = f" {pattern}" if pattern else ""
                if phase_prefills is not None:
                    binary = "dt_phase_probe"
                    suffix = ""
                arguments = (
                    f"{phase_prefills[index]} {compute_us}"
                    if phase_prefills is not None
                    else f"{compute_us}{suffix}"
                )
                cmds.append(
                    f"{remote}/{binary} {sock} {args.iterations} {arguments} "
                    f">{remote}/{name}.probe{index}.csv 2>&1 &"
                )
                cmds.append(f"probe{index}_pid=$!")
                if index == 0 and probes == 2 and args.decode_floor_on_prefill:
                    cmds.append(
                        f"count=0; while ! grep -q ',MEMBERS,0,' {remote}/{name}.events.csv; do "
                        "count=$((count+1)); [ $count -lt 100 ] || exit 1; sleep .01; done"
                    )
            for index in range(probes):
                cmds.append(f"wait $probe{index}_pid")
            cmds.extend(
                [
                    "kill -TERM $controller_pid",
                    "wait $controller_pid",
                    "trap - EXIT INT TERM",
                ]
            )
            rc, _ = run(
                ssh + ["\n".join(cmds)], f"{name}.runner.log", timeout=120, check=False
            )
            _, ctl_text = run(ssh + [f"cat {ctl}"], f"{name}.controller.log")
            run(ssh + [f"cat {remote}/{name}.events.csv"], f"{name}.events.csv")
            outputs = []
            for index in range(probes):
                _, output = run(
                    ssh + [f"cat {remote}/{name}.probe{index}.csv"],
                    f"{name}.probe{index}.csv",
                )
                outputs.append(output)
            if rc:
                raise RuntimeError(f"{name}: probe runner exited {rc}; logs retained")
            if "status=OK" not in ctl_text or not all(
                "PASS iterations=" in out for out in outputs
            ):
                raise RuntimeError(f"{name}: missing pass marker")
            if not re.search(r"wrong_cpu=0\b", ctl_text):
                raise RuntimeError(f"{name}: BPF observed wrong CPU")
            return ctl_text, outputs

        if args.allocation_policy == "prefill-first":
            import csv

            case(
                "phase-stable",
                "dynamic",
                1,
                100,
                phase_prefills=(args.iterations // 4,),
            )
            with (result / "phase-stable.events.csv").open() as stream:
                events = list(csv.DictReader(stream))
            counts = {
                kind: sum(row["event"] == kind for row in events)
                for kind in ("GRANT", "MEMBERS", "PHASE")
            }
            if counts != {"GRANT": 1, "MEMBERS": 1, "PHASE": 2}:
                raise RuntimeError(
                    f"phase-stable: repeated per-graph communication {counts}"
                )
            report["phase_stable"] = counts
            # Ensure prefill spans the budget, then leave time to observe all-decode.
            prefill_graphs = (
                args.iterations // 2 if args.decode_wait_ms else args.iterations // 4
            )
            compute_us = (
                max(1000, (args.decode_wait_ms + 1000) * 1000 // prefill_graphs)
                if args.decode_wait_ms
                else 1000
            )
            dynamic_ctl, dynamic = case(
                "dynamic", "dynamic", 2, compute_us, phase_prefills=(0, prefill_graphs)
            )
            with (result / "dynamic.events.csv").open() as stream:
                events = list(csv.DictReader(stream))
            decode_target = "1" if args.decode_floor_on_prefill else "0"
            if not any(
                row["event"] == "TARGET"
                and row["phase"] == "2"
                and row["target_workers"] == decode_target
                for row in events
            ):
                raise RuntimeError(f"phase: no decode target {decode_target}")
            if not any(
                row["event"] == "GRANT"
                and row["phase"] == "1"
                and row["workers"] == ("7" if args.decode_floor_on_prefill else "8")
                for row in events
            ):
                raise RuntimeError("phase: missing expected prefill grant")
            if args.decode_floor_on_prefill:
                shrink_widths = {
                    row["workers"] for row in events if row["event"] == "SHRINK"
                }
                if not {"1", "4"}.issubset(shrink_widths):
                    raise RuntimeError(
                        f"phase: missing actual 8->1 and 7->4 shrink: {shrink_widths}"
                    )
            rows = [
                re.findall(r"GRAPH (\d+) phase=(\d+) workers=(\d+) mask=(\d+)", output)
                for output in dynamic
            ]
            pinned_masks = [{int(row[3]) for row in group} for group in rows]
            report["ownership"] = validate_cpu_ownership(events)
            if args.decode_wait_ms:
                for kind in ("DECODE_PROTECT", "DECODE_RESUME"):
                    if not any(
                        row["event"] == kind and row["decode_protected"] == "1"
                        for row in events
                    ):
                        raise RuntimeError(f"guard: missing {kind}")
                for phase, width in (("2", "1"), ("1", "7")):
                    if not any(
                        row[1:3] == (phase, width) for group in rows for row in group
                    ):
                        raise RuntimeError(
                            f"guard: real probe never ran phase={phase} workers={width}"
                        )
                report["decode_guard"] = {
                    "decode_workers": 1,
                    "prefill_workers": 7,
                    "recoveries_ms": [
                        int(row["decode_recovery_ns"]) / 1e6
                        for row in events
                        if row["event"] == "DECODE_RESUME"
                        and int(row["decode_recovery_ns"])
                    ],
                }
        elif args.grant_lifetime == "persistent":
            import csv

            for pattern in ("stable", "vary", "recreate"):
                case(pattern, "dynamic", 1, 100, pattern)
                with (result / f"{pattern}.events.csv").open() as stream:
                    events = list(csv.DictReader(stream))
                grants = sum(row["event"] == "GRANT" for row in events)
                members = sum(row["event"] == "MEMBERS" for row in events)
                if grants != 1 or (pattern == "stable" and members != 1):
                    raise RuntimeError(
                        f"{pattern}: expected one persistent grant/registration"
                    )
                report[pattern] = {
                    "graphs": args.iterations,
                    "grants": grants,
                    "members": members,
                }
            dynamic_ctl, dynamic = case("dynamic", "dynamic", 2, 1000, "stable")
            rows = [
                re.findall(r"GRAPH (\d+) epoch=(\d+) workers=(\d+) mask=(\d+)", output)
                for output in dynamic
            ]
            pinned_masks = [{int(row[3]) for row in group} for group in rows]
        else:
            manual_ctl, manual = case("manual", "manual", 1, 100)
            manual_widths = [
                int(line.split(",")[2])
                for line in manual[0].splitlines()
                if re.match(r"^\d+,", line)
            ]
            expected = [8, 4, 2, 8]
            if manual_widths[:4] != expected or any(
                width != expected[i % 4] for i, width in enumerate(manual_widths)
            ):
                raise RuntimeError(
                    "manual: actual OpenMP width sequence differs from 8,4,2,8"
                )
            report["manual"] = {"samples": len(manual_widths), "sequence": expected}
            dynamic_ctl, dynamic = case("dynamic", "dynamic", 2, 500)
            rows = [
                [
                    line.split(",")
                    for line in output.splitlines()
                    if re.match(r"^\d+,", line)
                ]
                for output in dynamic
            ]
            pinned_masks = [
                {int(row[3]) for row in group if int(row[4]) == 1} for group in rows
            ]
        split_masks = [masks - {255} for masks in pinned_masks]
        if not (args.decode_wait_ms or args.decode_floor_on_prefill) and sorted(
            split_masks, key=lambda masks: min(masks or {999})
        ) != [{15}, {240}]:
            raise RuntimeError(
                f"dynamic: expected split masks 15/240, got {pinned_masks}"
            )
        match = re.search(r"placed=(\d+).*running=(\d+).*wrong_cpu=(\d+)", dynamic_ctl)
        if (
            not match
            or int(match.group(1)) == 0
            or int(match.group(2)) == 0
            or int(match.group(3))
        ):
            raise RuntimeError("dynamic: missing successful BPF placement observations")
        report["dynamic"] = {
            "samples": sum(map(len, rows)),
            "pinned_masks": [sorted(masks) for masks in pinned_masks],
            "bpf_placed": int(match.group(1)),
            "bpf_running": int(match.group(2)),
        }
        report["probes_passed"] = True
    except (OSError, subprocess.SubprocessError, RuntimeError) as exc:
        report["failure"] = str(exc)
    finally:
        if before is not None:
            try:
                _, after = run(ssh + ["dmesg"], "dmesg-after.log", check=False)
                if not after.startswith(before):
                    raise RuntimeError("dmesg history changed/wrapped")
                delta = after[len(before) :]
                (result / "dmesg-delta.log").write_text(delta)
                if re.search(
                    r"BUG:|WARNING:|Oops:|watchdog.*(stall|lockup)|"
                    r"sched_ext:.*(error|stall)|Out of memory|hung task",
                    delta,
                    re.I,
                ):
                    raise RuntimeError("new kernel warning/error")
                _, state = run(
                    ssh + ["cat /sys/kernel/debug/sched/ext"],
                    "cleanup-state.log",
                    check=False,
                )
                if not re.search(r"^enabled\s*:\s*0\s*$", state, re.M):
                    raise RuntimeError("scheduler did not detach")
                report["kernel_clean"] = True
            except (OSError, subprocess.SubprocessError, RuntimeError) as exc:
                report["cleanup_failure"] = str(exc)
        report["gate_passed"] = bool(
            report.get("probes_passed")
            and report.get("kernel_clean")
            and not report.get("failure")
            and not report.get("cleanup_failure")
        )
        (result / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    print(f"Evidence: {result}")
    return 0 if report["gate_passed"] else 1


if __name__ == "__main__":
    os.chdir(HERE)
    sys.exit(main())
