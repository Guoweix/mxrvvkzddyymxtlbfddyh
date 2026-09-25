#!/usr/bin/env python3
"""Unified CLI for the benchmark v2 rewrite (CFS only).

Scenarios:
  dual-async  two llama-servers, incumbent first, second injected at a token milestone
  dual-sync   two llama-servers, every request released at one shared time
  multi       one to eight llama-servers, every request released at one shared time
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from bench.runner import SuiteRunner, validate_models  # noqa: E402

SCENARIOS = ("dual-async", "dual-sync", "multi")
DEFAULT_PROMPTS = SCRIPT_DIR / "prompts.json"
DEFAULT_CONFIG_SH = SCRIPT_DIR.parent / "config.sh"
DEFAULT_MODEL_FILENAME = "Llama-3.2-3B-Instruct-Q4_K_M.gguf"
# Submission repo layout: this file lives at <repo>/tests/benchmark.
REPO_DIR = SCRIPT_DIR.parent.parent
# Patched llama runtimes live in the sibling working repository (as tests/config.sh does).
WORK_DIR = REPO_DIR.parent / "experiments"
DYNAMIC_DIR = WORK_DIR / "dynamic_threads"
SCX_DIR = REPO_DIR / "src" / "scx_llm_pin"

CONFIG_SH_KEYS = {
    "REMOTE_IP": "host",
    "REMOTE_USER": "user",
    "REMOTE_PASS": "password",
    "LLM_PATH": "llm_path",
    "REMOTE_PORT": "port_a",
    "RVV_REMOTE_DIR": "rvv_remote_dir",
}


def load_config_sh(path):
    """Source a shell config file and return the benchmark v2 defaults it defines."""
    script = (
        "set -a; "
        'source "$1" >/dev/null 2>&1; '
        "printf 'REMOTE_IP=%s\\n' \"$REMOTE_IP\"; "
        "printf 'REMOTE_USER=%s\\n' \"$REMOTE_USER\"; "
        "printf 'REMOTE_PASS=%s\\n' \"$REMOTE_PASS\"; "
        "printf 'LLM_PATH=%s\\n' \"$LLM_PATH\"; "
        "printf 'REMOTE_PORT=%s\\n' \"$REMOTE_PORT\"; "
        "printf 'RVV_REMOTE_DIR=%s\\n' \"$RVV_REMOTE_DIR\""
    )
    result = subprocess.run(
        ["bash", "-c", script, "bash", str(path)],
        capture_output=True,
        text=True,
    )
    values = {}
    for line in result.stdout.splitlines():
        key, separator, value = line.partition("=")
        if separator and value:
            values[key] = value
    return values


def apply_config_defaults(args):
    """Fill connection defaults from config.sh; CLI flags always take precedence."""
    path = Path(args.config_sh) if args.config_sh else DEFAULT_CONFIG_SH
    if not path.is_file():
        raise SystemExit(
            f"config file does not exist: {path} (pass --config-sh or --host/--user/...)"
        )
    values = load_config_sh(path)
    for source, target in CONFIG_SH_KEYS.items():
        if getattr(args, target, None) is None:
            if source not in values:
                raise SystemExit(f"{path} does not define {source}")
            setattr(args, target, values[source])
    return args


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=SCENARIOS)
    parser.add_argument(
        "--strategy",
        choices=("cfs", "dynamic", "scx-llm-pin", "cfs-affinity"),
        default="cfs",
        help="scheduling strategy: cfs, dynamic (experiments/dynamic_threads), "
        "scx-llm-pin (experiments/scx_llm_pin), or cfs-affinity "
        "(worker affinity + long base slice + short GOMP spin)",
    )
    parser.add_argument(
        "--config-sh",
        default=None,
        help="shell config file to source for board defaults; defaults to tests/config.sh",
    )
    parser.add_argument("--host", default=None, help="board IP (REMOTE_IP)")
    parser.add_argument("--user", default=None, help="SSH user (REMOTE_USER)")
    parser.add_argument("--password", default=None, help="SSH password (REMOTE_PASS)")
    parser.add_argument("--llm-path", default=None, help="board llama directory (LLM_PATH)")
    parser.add_argument("--runtime-bin-dir", default="", help="board runtime dir containing llama-server")
    parser.add_argument("--results-dir", default=str(SCRIPT_DIR / "results"))
    parser.add_argument("--loops", type=int, default=1)
    parser.add_argument("--max-attempts", type=int, default=2)
    parser.add_argument("--output-tokens", type=int, default=256)
    parser.add_argument("--lead-sec", type=float)
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--context", type=int, default=4096)
    parser.add_argument("--request-timeout", type=float, default=3600.0)
    parser.add_argument("--milestone-timeout", type=float, default=1800.0)
    parser.add_argument("--server-start-timeout", type=float, default=180.0)
    parser.add_argument("--warmup-tokens", type=int, default=2)
    parser.add_argument("--quiet-sec", type=float, default=5.0)
    parser.add_argument("--sample-interval", type=float, default=1.0)
    parser.add_argument("--min-frequency-khz", type=int, default=1_600_000)
    parser.add_argument("--max-temperature-millic", type=int, default=85_000)
    parser.add_argument("--min-available-memory-kib", type=int, default=1_048_576)
    parser.add_argument("--prompts-file", default=str(DEFAULT_PROMPTS))

    dynamic = parser.add_argument_group("dynamic strategy")
    dynamic.add_argument(
        "--dynamic-runtime-bin",
        default=str(DYNAMIC_DIR / "llama.cpp" / "build-dynamic-riscv" / "bin"),
        help="local patched llama-server runtime directory to deploy",
    )
    dynamic.add_argument(
        "--dynamic-controller",
        default=str(DYNAMIC_DIR / "build" / "dt_controller"),
        help="local dt_controller binary to deploy",
    )
    dynamic.add_argument("--dynamic-mode", default="dynamic")
    dynamic.add_argument("--dynamic-grant-lifetime", default="persistent")
    dynamic.add_argument("--dynamic-allocation-policy", default="prefill-first")
    dynamic.add_argument("--dynamic-decode-wait-ms", type=int, default=0)
    dynamic.add_argument(
        "--dynamic-decode-floor-on-prefill",
        dest="dynamic_decode_floor_on_prefill",
        action="store_true",
        default=True,
    )
    dynamic.add_argument(
        "--no-dynamic-decode-floor-on-prefill",
        dest="dynamic_decode_floor_on_prefill",
        action="store_false",
    )
    dynamic.add_argument("--dynamic-slice-ms", type=int, default=0)
    dynamic.add_argument(
        "--dynamic-retain-shrink",
        dest="dynamic_retain_shrink",
        action="store_true",
        default=True,
        help="retain surviving workers on shrink (default); use --dynamic-full-release to disable",
    )
    dynamic.add_argument(
        "--dynamic-full-release",
        dest="dynamic_retain_shrink",
        action="store_false",
    )

    scx = parser.add_argument_group("scx-llm-pin strategy")
    scx.add_argument(
        "--user-hint",
        choices=("on", "off"),
        default="on",
        help="scx-llm-pin: start servers with the ABI v4 phase-hint path (on) "
        "or without any userspace hint (off)",
    )
    scx.add_argument(
        "--scx-llm-pin-bin",
        default=str(SCX_DIR / "scx_llm_pin"),
        help="local scx_llm_pin loader binary to deploy",
    )
    scx.add_argument(
        "--scx-hint-runtime-bin",
        default="/mnt/llm/llama/bin-nteam-v4",
        help="board ABI v4 hinted runtime used when --user-hint on",
    )
    scx.add_argument("--scx-control-dir", default="/sys/fs/bpf/scx_rvv_dirty_lease")
    scx.add_argument("--scx-vector-slice-ms", type=int, default=100)
    scx.add_argument("--scx-vector-local-window-ms", type=int, default=500)
    scx.add_argument("--scx-dirty-threshold", type=int, default=2)
    scx.add_argument("--scx-phase-hint-timeout-ms", type=int, default=300000)
    scx.add_argument("--scx-prefill-starvation-ms", type=int, default=1000)
    scx.add_argument("--scx-phase-wakeup-preempt", choices=("on", "off"), default="on")

    aff = parser.add_argument_group("cfs-affinity strategy")
    aff.add_argument("--cfs-affinity-slice-ms", type=int, default=100)
    aff.add_argument("--cfs-affinity-spincount", default="1000")

    trace = parser.add_argument_group("rvv trace")
    trace.add_argument(
        "--rvv-trace",
        action="store_true",
        help="run rvv_sched_cost against every server (uses the board copy)",
    )
    trace.add_argument(
        "--rvv-futex-time",
        action="store_true",
        help="also record futex blocked time (implies --rvv-trace)",
    )
    trace.add_argument(
        "--rvv-remote-dir",
        default=None,
        help="board directory holding rvv_sched_cost and lib/ (default: RVV_REMOTE_DIR)",
    )
    trace.add_argument(
        "--rvv-upload",
        action="store_true",
        help="upload the local rvv_sched_cost/libelf instead of using the board copy",
    )
    trace.add_argument(
        "--rvv-local-bin",
        default=str(REPO_DIR / "src" / "rvv_sched_cost" / "rvv_sched_cost"),
        help="local rvv_sched_cost binary used with --rvv-upload",
    )
    trace.add_argument(
        "--rvv-local-libelf",
        default=str(
            REPO_DIR.parent
            / "jupiter-buildroot/output/k1_v2/target/usr/lib/libelf-0.186.so"
        ),
        help="local libelf shared object used with --rvv-upload",
    )

    dual = parser.add_argument_group("dual scenario")
    dual.add_argument("--concurrency", choices=(2, 4, 8), type=int, default=8)
    dual.add_argument("--direction", choices=("ab", "ba", "both"), default="ab")
    dual.add_argument("--port-a", type=int, default=None)
    dual.add_argument("--port-b", type=int, default=9091)
    dual.add_argument(
        "--model-a",
        default=None,
        help=f"board model path; defaults to <LLM_PATH>/models/{DEFAULT_MODEL_FILENAME}",
    )
    dual.add_argument(
        "--model-b",
        default=None,
        help=f"board model path; defaults to <LLM_PATH>/models/{DEFAULT_MODEL_FILENAME}",
    )
    dual.add_argument("--injection-token", type=int, default=4)
    dual.add_argument("--arrival-gap-sec", type=float)

    multi = parser.add_argument_group("multi scenario")
    multi.add_argument("--config", help="JSON model list for the multi scenario")
    multi.add_argument("--check-config", action="store_true")
    parser.add_argument("--list-scenarios", action="store_true")
    return parser


def resolve_args(args):
    if args.rvv_futex_time:
        args.rvv_trace = True
    if args.rvv_trace:
        if not args.rvv_remote_dir:
            raise SystemExit(
                "--rvv-trace needs the board directory: set RVV_REMOTE_DIR in config.sh "
                "or pass --rvv-remote-dir"
            )
        args.rvv_remote_bin = f"{args.rvv_remote_dir}/rvv_sched_cost"
        args.rvv_remote_lib_dir = f"{args.rvv_remote_dir}/lib"
        if args.rvv_upload:
            for label, path in (
                ("rvv-local-bin", args.rvv_local_bin),
                ("rvv-local-libelf", args.rvv_local_libelf),
            ):
                if not Path(path).is_file():
                    raise SystemExit(f"--{label} does not exist: {path}")
    else:
        args.rvv_remote_bin = None
        args.rvv_remote_lib_dir = None
    if args.rvv_remote_dir is None:
        args.rvv_remote_dir = ""
    if args.scenario != "multi":
        default_model = f"{args.llm_path}/models/{DEFAULT_MODEL_FILENAME}"
        args.model_a = args.model_a or default_model
        args.model_b = args.model_b or default_model
    if args.scenario == "dual-async":
        if args.arrival_gap_sec is None:
            args.arrival_gap_sec = 1.0
        if args.lead_sec is None:
            args.lead_sec = 1.0
        if not 0 < args.injection_token < args.output_tokens:
            raise SystemExit("--injection-token must be between 1 and output-tokens - 1")
        args.config = None
    elif args.scenario == "dual-sync":
        args.arrival_gap_sec = 0.0
        if args.lead_sec is None:
            args.lead_sec = 2.0
        args.config = None
    else:
        if not args.config:
            raise SystemExit("--config is required for the multi scenario")
        if args.lead_sec is None:
            args.lead_sec = 2.0
        args.direction = "na"
        config = json.loads(Path(args.config).read_text())
        args.concurrency = sum(int(m.get("requests", 1)) for m in config.get("models", []))
    return args


def main():
    parser = build_parser()
    args = parser.parse_args()

    if args.list_scenarios:
        for scenario in SCENARIOS:
            print(scenario)
        return 0

    if not args.scenario:
        parser.error("--scenario is required")

    if args.scenario == "multi" and args.check_config:
        config = json.loads(Path(args.config).read_text())
        models = validate_models(config)
        print(json.dumps({"models": models}, indent=2))
        return 0

    apply_config_defaults(args)
    resolve_args(args)
    if not Path(args.prompts_file).is_file():
        raise SystemExit(f"prompts file does not exist: {args.prompts_file}")
    if args.strategy == "dynamic":
        runtime = Path(args.dynamic_runtime_bin) / "llama-server"
        if not runtime.is_file():
            raise SystemExit(f"dynamic runtime not built: {runtime}")
        if not Path(args.dynamic_controller).is_file():
            raise SystemExit(f"dynamic controller not built: {args.dynamic_controller}")
    if args.strategy == "scx-llm-pin" and not Path(args.scx_llm_pin_bin).is_file():
        raise SystemExit(f"scx_llm_pin binary not built: {args.scx_llm_pin_bin}")
    for command in ("sshpass", "ssh", "scp"):
        if shutil.which(command) is None:
            raise SystemExit(f"missing required command: {command}")

    raise SystemExit(SuiteRunner(args).run())


if __name__ == "__main__":
    main()
