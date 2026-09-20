"""Single source of truth for board/paths: reads tests/benchmark/config.sh.

Lookup order is: process environment -> tests/benchmark/config.sh -> fallback.
Scripts should not define their own board defaults; set values in config.sh.
"""

import os
import subprocess
from pathlib import Path

_HERE = Path(__file__).resolve()
CONFIG_SH = _HERE.parent.parent / "config.sh"  # tests/config.sh
REPO_ROOT = _HERE.parents[2]  # 仓库根目录（含 src/ 与 tests/）
_KEYS = (
    "REMOTE_IP",
    "REMOTE_USER",
    "REMOTE_PASS",
    "REMOTE_PORT",
    "LLM_PATH",
    "LLAMA_CPU_SRC",
    "DT_LLAMA_BIN",
)
_cache = None


def _load():
    global _cache
    if _cache is not None:
        return _cache
    values = {}
    if CONFIG_SH.is_file():
        script = (
            'source "$1" >/dev/null 2>&1; '
            'for n in %s; do printf "%%s=%%s\\n" "$n" "${!n}"; done'
        ) % " ".join(_KEYS)
        try:
            completed = subprocess.run(
                ["bash", "-c", script, "_", str(CONFIG_SH)],
                capture_output=True,
                text=True,
                timeout=10,
            )
            for line in completed.stdout.splitlines():
                if "=" in line:
                    key, val = line.split("=", 1)
                    values[key] = val
        except (OSError, subprocess.SubprocessError):
            pass
    _cache = values
    return values


def value(name, fallback=""):
    """Return environment override, else config.sh value, else fallback."""
    if os.environ.get(name):
        return os.environ[name]
    return _load().get(name) or fallback


def path(name, fallback=""):
    """Like value(), but resolve a relative path against the repo root."""
    raw = value(name, fallback)
    if not raw:
        return None
    candidate = Path(raw)
    if candidate.is_absolute():
        return candidate
    return (REPO_ROOT / raw).resolve()


def ensure_remote_password():
    """Populate REMOTE_PASS from config.sh when the caller did not set it."""
    if not os.environ.get("REMOTE_PASS") and value("REMOTE_PASS"):
        os.environ["REMOTE_PASS"] = value("REMOTE_PASS")
