#!/usr/bin/env python3
"""Parse and validate scx_llm_pin diagnostics captured by the benchmark."""

import re
from pathlib import Path


KEY_VALUE = re.compile(r"([a-zA-Z0-9_]+)=([0-9]+)")
GANG_MODE = re.compile(
    r"--team-gang(?:=|\s+)(off|observe|always|prefill-lease)(?:\s|$)"
)
PHASE_WAKEUP_PREEMPT = re.compile(
    r"--phase-wakeup-preempt(?:=|\s+)(on|off)(?:\s|$)"
)


def parse_values(line):
    return {key: int(value) for key, value in KEY_VALUE.findall(line)}


def parse_scheduler_log(path):
    path = Path(path)
    text = path.read_text(errors="replace") if path.exists() else ""
    stats = {}
    samples = {}
    for line in text.splitlines():
        if line.startswith("SCX_GANG_STATS "):
            stats = parse_values(line)
        elif line.startswith("SCX_GANG_SAMPLES "):
            samples = parse_values(line)
    return {
        "present": path.exists(),
        "attached": "scx_llm_pin attached successfully" in text,
        "detached": "scx_llm_pin detached" in text,
        "stats": stats,
        "samples": samples,
    }


def detect_profile(strategy, command, requested="auto"):
    if requested != "auto":
        return requested
    if strategy != "external" or "scx_llm_pin" not in command:
        return "none"
    match = GANG_MODE.search(command)
    mode = match.group(1) if match else "off"
    if mode == "observe":
        return "gang-observe"
    if mode == "prefill-lease":
        preempt_match = PHASE_WAKEUP_PREEMPT.search(command)
        if preempt_match and preempt_match.group(1) == "off":
            return "phase-lease-no-preempt"
        return "phase-lease"
    if mode == "always":
        return "gang-active"
    return "lease"


def validate_scheduler_log(path, profile):
    diagnostics = parse_scheduler_log(path)
    stats = diagnostics["stats"]
    samples = diagnostics["samples"]
    checks = {}

    if profile == "none":
        return {
            "profile": profile,
            "valid": True,
            "checks": checks,
            **diagnostics,
        }

    checks["attached"] = diagnostics["attached"]
    checks["machine_stats"] = bool(stats)
    checks["workers_8_plus_8"] = (
        stats.get("rvv_team0") == 8 and stats.get("rvv_team1") == 8
    )
    checks["hinted_workers_16"] = stats.get("hint_valid_workers", 0) >= 16
    checks["dsq_balanced"] = (
        "dsq_dispatches" in stats
        and stats.get("dsq_dispatches") == stats.get("dsq_consumes")
        and stats.get("dsq_dispatches_team0") == stats.get("dsq_consumes_team0")
        and stats.get("dsq_dispatches_team1") == stats.get("dsq_consumes_team1")
    )

    if profile in {
        "gang-observe",
        "gang-active",
        "phase-lease",
        "phase-lease-no-preempt",
    }:
        checks["phase_hints_seen"] = (
            stats.get("phase_valid", 0) > 0
            and stats.get("phase_prefill", 0) > 0
            and stats.get("phase_decode", 0) > 0
        )
    if profile == "gang-observe":
        checks["observe_did_not_schedule"] = stats.get("gang_engagements", 0) == 0
    if profile == "gang-active":
        checks["gang_engaged"] = (
            stats.get("gang_engagements", 0) > 0
            and stats.get("gang_quanta", 0) > 0
            and stats.get("gang_enqueues", 0) > 0
        )
    if profile in {"phase-lease", "phase-lease-no-preempt"}:
        checks["phase_lease_engaged"] = (
            stats.get("phase_lease_engagements", 0) > 0
            and stats.get("phase_lease_preferred", 0) > 0
            and stats.get("phase_lease_runs", 0) > 0
        )
        checks["no_gang_churn"] = (
            stats.get("gang_quanta", 0) == 0
            and stats.get("gang_kicks", 0) == 0
            and stats.get("gang_enqueues", 0) == 0
        )
        checks["pending_wait_safe"] = (
            "phase_lease_pending_max_ns" in stats
            and stats.get("phase_lease_pending_ge_4s", 0) == 0
        )
        if profile == "phase-lease":
            checks["wakeup_preemption_active"] = (
                stats.get("phase_lease_wakeup_candidates", 0) > 0
                and stats.get("phase_lease_wakeup_preempts", 0) > 0
            )
        else:
            checks["wakeup_preemption_disabled"] = (
                stats.get("phase_lease_wakeup_preempts", 0) == 0
            )
    if samples.get("interval_us", 0):
        checks["sampler_healthy"] = (
            samples.get("samples", 0) > 0 and samples.get("read_errors", 0) == 0
        )

    diagnostics["gang_run_match_percent"] = (
        100.0 * stats.get("gang_active_runs", 0) / stats["gang_runs"]
        if stats.get("gang_runs", 0)
        else None
    )
    diagnostics["sample_full_gang_percent"] = (
        100.0 * samples.get("full_gang", 0) / samples["engaged"]
        if samples.get("engaged", 0)
        else None
    )
    diagnostics["phase_lease_run_match_percent"] = (
        100.0 * stats.get("phase_lease_preferred_runs", 0) / stats["phase_lease_runs"]
        if stats.get("phase_lease_runs", 0)
        else None
    )
    diagnostics["profile"] = profile
    diagnostics["checks"] = checks
    diagnostics["valid"] = bool(checks and all(checks.values()))
    return diagnostics
