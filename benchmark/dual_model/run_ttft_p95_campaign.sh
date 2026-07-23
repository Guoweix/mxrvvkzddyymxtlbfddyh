#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
CAMPAIGN_DIR="$SCRIPT_DIR/results/ttft_p95_equal128"
STATUS_FILE="$CAMPAIGN_DIR/status.tsv"
CONFIG_FILE="$SCRIPT_DIR/../config.sh"

if [[ ! -r "$CONFIG_FILE" ]]; then
    printf 'Missing local benchmark configuration: %s\n' "$CONFIG_FILE" >&2
    exit 1
fi
# shellcheck source=../config.sh
source "$CONFIG_FILE"

BOARD_HOST="$REMOTE_IP"
BOARD_USER="$REMOTE_USER"
BOARD_PASSWORD="$REMOTE_PASS"
RUNTIME_BIN_DIR="${DUAL_MODEL_RUNTIME_BIN_DIR:-$LLM_PATH/bin-phase-v3-20260721}"
MODEL_PATH="$LLM_PATH/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf"
REMOTE_SCHEDULER_BIN="${DUAL_MODEL_SCHEDULER_BIN:-/root/scx_llm_pin_ablation/scx_llm_pin}"
LEASE_HINT_PATH="/sys/fs/bpf/scx_rvv_dirty_lease/workers"

if (($# != 0)); then
    printf 'This one-shot campaign does not accept arguments.\n' >&2
    exit 2
fi

loops_for_concurrency() {
    case "$1" in
        2) printf '60\n' ;;
        4) printf '30\n' ;;
        8) printf '15\n' ;;
        *) return 1 ;;
    esac
}

injected_samples_per_run() {
    printf '%s\n' "$(( $1 / 2 ))"
}

remote() {
    sshpass -p "$BOARD_PASSWORD" ssh \
        -o ConnectTimeout=5 \
        -o StrictHostKeyChecking=no \
        "$BOARD_USER@$BOARD_HOST" "$@"
}

preflight() {
    local command
    for command in python3 ssh sshpass scp jq tee; do
        command -v "$command" >/dev/null 2>&1 || {
            printf 'Missing required command: %s\n' "$command" >&2
            return 1
        }
    done

    python3 -m unittest discover -s "$SCRIPT_DIR" -p 'test_*.py' >/dev/null || return 1
    python3 - "$SCRIPT_DIR/prompts.json" <<'PY'
import json
import sys

prompts = json.load(open(sys.argv[1]))
assert len(prompts) == 4
assert len({item["id"] for item in prompts}) == 4
assert {item["target_tokens"] for item in prompts} == {128}
assert len({item["prompt"] for item in prompts}) == 4
PY

    remote "test -x '$RUNTIME_BIN_DIR/llama-server' && \
test -x '$REMOTE_SCHEDULER_BIN' && \
test -r '$MODEL_PATH'" || {
        printf 'Board preflight failed: runtime, scheduler, or model is unavailable.\n' >&2
        return 1
    }
}

external_command() {
    local mode=$1
    printf '%s' "$REMOTE_SCHEDULER_BIN \
--tgid \"\$MODEL_A_PID\" --tgid2 \"\$MODEL_B_PID\" \
--strategy rvv_dirty --vector-slice-ms 100 \
--vector-local-window-ms 500 --dirty-threshold 2 \
--user-control active --user-cpu-map auto \
--team-gang $mode --prefill-starvation-ms 1000 \
--vector-local-wakeup off --phase-wakeup-preempt on"
}

run_cell() {
    local strategy=$1
    local concurrency=$2
    local loops=$3
    local cell_dir="$CAMPAIGN_DIR/$strategy/c$concurrency"
    local result_dir="$cell_dir/results"
    local log_file="$cell_dir/run.log"
    local done_file="$cell_dir/.done"
    local exit_file="$cell_dir/exit_code"
    local report_file=""
    local rc=0
    local expected_samples=$((loops * $(injected_samples_per_run "$concurrency")))
    local -a command=(
        "$SCRIPT_DIR/run.sh"
        --scenario asynchronous
        --directions ab
        --concurrency "$concurrency"
        --loops "$loops"
        --output-tokens 256
        --injection-token 4
        --arrival-gap-sec 1
        --metrics standard
        --host "$BOARD_HOST"
        --user "$BOARD_USER"
        --password "$BOARD_PASSWORD"
        --llm-path "$LLM_PATH"
        --runtime-bin-dir "$RUNTIME_BIN_DIR"
        --model-a "$MODEL_PATH"
        --model-b "$MODEL_PATH"
        --prompts-file "$SCRIPT_DIR/prompts.json"
        --scheduler-validation auto
        --results-dir "$result_dir"
    )

    mkdir -p "$cell_dir" "$result_dir"
    if [[ -f "$done_file" ]]; then
        report_file=$(cat "$done_file")
        printf '%s\t%s\t%s\t%s\tSKIPPED\t0\t%s\n' \
            "$strategy" "$concurrency" "$loops" "$expected_samples" "$report_file" \
            >> "$STATUS_FILE"
        printf '[SKIP] %s c=%s already completed: %s\n' \
            "$strategy" "$concurrency" "$report_file"
        return 0
    fi

    case "$strategy" in
        cfs)
            command+=(--strategy cfs-shared)
            ;;
        observe)
            command+=(
                --strategy external
                --lease-hint-path "$LEASE_HINT_PATH"
                --external-start-cmd "$(external_command observe)"
            )
            ;;
        phase-aware)
            command+=(
                --strategy external
                --lease-hint-path "$LEASE_HINT_PATH"
                --external-start-cmd "$(external_command prefill-lease)"
            )
            ;;
        *)
            printf 'Unknown strategy: %s\n' "$strategy" >&2
            return 2
            ;;
    esac

    {
        printf '[START] %s c=%s loops=%s expected_injected_samples=%s %s\n' \
            "$strategy" "$concurrency" "$loops" "$expected_samples" "$(date --iso-8601=seconds)"
        printf '[COMMAND]'
        printf ' %q' "${command[@]}"
        printf '\n'
    } | tee -a "$log_file"

    "${command[@]}" 2>&1 | tee -a "$log_file"
    rc=${PIPESTATUS[0]}
    printf '%s\n' "$rc" > "$exit_file"

    report_file=$(find "$result_dir" -type f -name report.md -printf '%T@\t%p\n' 2>/dev/null \
        | sort -nr | head -n 1 | cut -f2-)
    if ((rc == 0)) && [[ -n "$report_file" ]]; then
        printf '%s\n' "$report_file" > "$done_file"
        printf '%s\t%s\t%s\t%s\tPASS\t0\t%s\n' \
            "$strategy" "$concurrency" "$loops" "$expected_samples" "$report_file" \
            >> "$STATUS_FILE"
        printf '[PASS] %s c=%s report=%s\n' "$strategy" "$concurrency" "$report_file" \
            | tee -a "$log_file"
        return 0
    fi

    printf '%s\t%s\t%s\t%s\tFAIL\t%s\t%s\n' \
        "$strategy" "$concurrency" "$loops" "$expected_samples" "$rc" "$report_file" \
        >> "$STATUS_FILE"
    printf '[FAIL] %s c=%s rc=%s report=%s\n' \
        "$strategy" "$concurrency" "$rc" "${report_file:-none}" | tee -a "$log_file"
    return 1
}

main() {
    local failures=0
    local strategy concurrency loops

    mkdir -p "$CAMPAIGN_DIR"
    printf 'strategy\tconcurrency\tloops\tinjected_samples\tstatus\texit_code\treport\n' \
        > "$STATUS_FILE"

    preflight || exit 1

    for strategy in cfs observe phase-aware; do
        for concurrency in 2 4 8; do
            loops=$(loops_for_concurrency "$concurrency")
            run_cell "$strategy" "$concurrency" "$loops" || failures=$((failures + 1))
        done
    done

    printf '\nCampaign status: %s\n' "$STATUS_FILE"
    column -ts $'\t' "$STATUS_FILE" 2>/dev/null || cat "$STATUS_FILE"
    if ((failures > 0)); then
        printf '%s campaign cells failed; rerun this script to retry only failed cells.\n' \
            "$failures" >&2
        exit 1
    fi
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main
fi
