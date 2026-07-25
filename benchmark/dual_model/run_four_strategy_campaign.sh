#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
CAMPAIGN_DIR="$SCRIPT_DIR/results/four_strategy_c2_c4_c8_20260725"
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
    printf 'This campaign does not accept arguments.\n' >&2
    exit 2
fi

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
    remote "test -x '$RUNTIME_BIN_DIR/llama-server' && \
test -x '$REMOTE_SCHEDULER_BIN' && test -r '$MODEL_PATH'" || {
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
    local loop=$3
    local cell_dir="$CAMPAIGN_DIR/c$concurrency/loop$loop/$strategy"
    local results_dir="$cell_dir/results"
    local log_file="$cell_dir/run.log"
    local done_file="$cell_dir/.done"
    local exit_file="$cell_dir/exit_code"
    local report_file=""
    local rc=0
    local -a command=(
        "$SCRIPT_DIR/run.sh"
        --scenario asynchronous
        --directions ab
        --concurrency "$concurrency"
        --loops 1
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
        --max-attempts 3
        --results-dir "$results_dir"
    )

    mkdir -p "$cell_dir" "$results_dir"
    if [[ -f "$done_file" ]]; then
        report_file=$(cat "$done_file")
        printf '%s\t%s\t%s\tSKIPPED\t0\t%s\n' \
            "$strategy" "$concurrency" "$loop" "$report_file" >> "$STATUS_FILE"
        printf '[SKIP] strategy=%s c=%s loop=%s report=%s\n' \
            "$strategy" "$concurrency" "$loop" "$report_file"
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
        cfs-4plus4)
            command+=(--strategy cfs-split)
            ;;
        *)
            printf 'Unknown strategy: %s\n' "$strategy" >&2
            return 2
            ;;
    esac

    {
        printf '[START] strategy=%s c=%s loop=%s %s\n' \
            "$strategy" "$concurrency" "$loop" "$(date --iso-8601=seconds)"
        printf '[COMMAND]'
        printf ' %q' "${command[@]}"
        printf '\n'
    } | tee -a "$log_file"

    "${command[@]}" </dev/null 2>&1 | tee -a "$log_file"
    rc=${PIPESTATUS[0]}
    printf '%s\n' "$rc" > "$exit_file"
    report_file=$(find "$results_dir" -type f -name report.md -printf '%T@\t%p\n' 2>/dev/null \
        | sort -nr | head -n 1 | cut -f2-)

    if ((rc == 0)) && [[ -n "$report_file" ]]; then
        printf '%s\n' "$report_file" > "$done_file"
        printf '%s\t%s\t%s\tPASS\t0\t%s\n' \
            "$strategy" "$concurrency" "$loop" "$report_file" >> "$STATUS_FILE"
        printf '[PASS] strategy=%s c=%s loop=%s report=%s\n' \
            "$strategy" "$concurrency" "$loop" "$report_file" | tee -a "$log_file"
        return 0
    fi

    printf '%s\t%s\t%s\tFAIL\t%s\t%s\n' \
        "$strategy" "$concurrency" "$loop" "$rc" "$report_file" >> "$STATUS_FILE"
    printf '[FAIL] strategy=%s c=%s loop=%s rc=%s report=%s\n' \
        "$strategy" "$concurrency" "$loop" "$rc" "${report_file:-none}" | tee -a "$log_file"
    return 1
}

strategy_order() {
    case "$1" in
        1) printf '%s\n' cfs observe phase-aware cfs-4plus4 ;;
        2) printf '%s\n' observe phase-aware cfs-4plus4 cfs ;;
        3) printf '%s\n' phase-aware cfs-4plus4 cfs observe ;;
        *) return 1 ;;
    esac
}

main() {
    local failures=0
    local concurrency loop strategy
    local -a order=()

    mkdir -p "$CAMPAIGN_DIR"
    printf 'strategy\tconcurrency\tloop\tstatus\texit_code\treport\n' > "$STATUS_FILE"
    preflight || exit 1

    for concurrency in 2 4 8; do
        for loop in 1 2 3; do
            mapfile -t order < <(strategy_order "$loop")
            for strategy in "${order[@]}"; do
                run_cell "$strategy" "$concurrency" "$loop" || failures=$((failures + 1))
            done
        done
    done

    printf '\nCampaign status: %s\n' "$STATUS_FILE"
    column -ts $'\t' "$STATUS_FILE" 2>/dev/null || cat "$STATUS_FILE"
    if ((failures > 0)); then
        printf '%s campaign cells failed; rerun to retry only failed cells.\n' "$failures" >&2
        exit 1
    fi
}

main
