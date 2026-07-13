#!/bin/bash
# Linux affinity worker pin benchmark wrapper.
# This does not load sched_ext. It pins the top-runtime llama-server workers
# to fixed CPUs with taskset before the formal benchmark window.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$#" -eq 0 ]; then
    set -- --duration 300 --tasks 4 --loops 3 --tokens 100
fi

export AFFINITY_PIN_WORKERS="${AFFINITY_PIN_WORKERS:-1}"
export AFFINITY_WORKER_CPUS="${AFFINITY_WORKER_CPUS:-0-7}"
export RVV_TRACE="${RVV_TRACE:-1}"

echo "[INFO] Linux affinity benchmark"
echo "[INFO] AFFINITY_WORKER_CPUS=$AFFINITY_WORKER_CPUS"
echo "[INFO] RVV_TRACE=$RVV_TRACE"
echo "[INFO] benchmark args: $*"

exec "$SCRIPT_DIR/benchmark.sh" --strategy linux_affinity "$@"
