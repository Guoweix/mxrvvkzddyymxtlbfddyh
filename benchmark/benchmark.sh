#!/bin/bash
# ========================================
# RVV LLM Benchmark - 主测试脚本
# ========================================
# 功能：
# - 启动远程llama-server
# - 按配置扫描并发档位和重复轮数
# - 记录首字延迟（TTFT）
# - 记录每个任务的tok/s
# - 生成详细的测试报告
# ========================================

set -e

# 加载配置
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

SCX_VECTOR_SLICE_OVERRIDE_MS=""
SCX_VECTOR_LOCAL_WINDOW_OVERRIDE_MS=""
SCX_DIRTY_THRESHOLD_OVERRIDE=""

# ========================================
# 工具函数
# ========================================

log_info() {
    echo -e "${COLOR_GREEN}[INFO]${COLOR_RESET} $1"
}

log_warn() {
    echo -e "${COLOR_YELLOW}[WARN]${COLOR_RESET} $1"
}

log_error() {
    echo -e "${COLOR_RED}[ERROR]${COLOR_RESET} $1"
}

log_header() {
    echo -e "${COLOR_BOLD}${COLOR_CYAN}"
    echo "========================================"
    echo "$1"
    echo "========================================"
    echo -e "${COLOR_RESET}"
}

# SSH命令封装
ssh_exec() {
    sshpass -p "$REMOTE_PASS" ssh -o StrictHostKeyChecking=no "$REMOTE_USER@$REMOTE_IP" "$@"
}

# SCP命令封装
scp_upload() {
    sshpass -p "$REMOTE_PASS" scp -o StrictHostKeyChecking=no "$1" "$REMOTE_USER@$REMOTE_IP:$2"
}

remote_pid_by_pattern() {
    local pattern=$1
    ssh_exec "ps | awk '/$pattern/ && !/awk/ {print \$1; exit}'"
}

remote_kill_by_pattern() {
    local pattern=$1
    local signal=${2:--TERM}
    ssh_exec "for p in \$(ps | awk '/$pattern/ && !/awk/ {print \$1}'); do kill $signal \$p 2>/dev/null || true; done"
}

resolve_local_path() {
    local path=$1
    if [[ "$path" = /* ]]; then
        echo "$path"
    else
        echo "$SCRIPT_DIR/$path"
    fi
}

require_positive_int() {
    local name=$1
    local value=$2

    if ! [[ "$value" =~ ^[1-9][0-9]*$ ]]; then
        log_error "$name 必须是正整数，当前值: $value"
        exit 1
    fi
}

require_concurrency_list() {
    local name=$1
    local value=$2

    if ! [[ "$value" =~ ^[1-9][0-9]*(,[1-9][0-9]*)*$ ]]; then
        log_error "$name 必须是逗号分隔的正整数列表，当前值: $value"
        exit 1
    fi
}

expand_cpu_list() {
    local spec=$1
    local old_ifs=$IFS
    local parts=()
    local cpus=()
    local part

    IFS=',' read -r -a parts <<< "$spec"
    IFS=$old_ifs

    for part in "${parts[@]}"; do
        if [[ "$part" =~ ^([0-9]+)-([0-9]+)$ ]]; then
            local start=${BASH_REMATCH[1]}
            local end=${BASH_REMATCH[2]}
            local cpu
            for cpu in $(seq "$start" "$end"); do
                cpus+=("$cpu")
            done
        elif [[ "$part" =~ ^[0-9]+$ ]]; then
            cpus+=("$part")
        fi
    done

    printf '%s\n' "${cpus[@]}"
}

RESULTS_DIR=$(resolve_local_path "$RESULTS_DIR")
LOG_DIR=$(resolve_local_path "$LOG_DIR")
SCX_LOCAL_BIN=$(resolve_local_path "$SCX_LOCAL_BIN")
SCX_LEASE_CTL_LOCAL_BIN=$(resolve_local_path "$SCX_LEASE_CTL_LOCAL_BIN")
SCX_LEASE_HINT_LOCAL_LIB=$(resolve_local_path "$SCX_LEASE_HINT_LOCAL_LIB")
RVV_LOCAL_BIN=$(resolve_local_path "$RVV_LOCAL_BIN")
RVV_LOCAL_LIBELF=$(resolve_local_path "$RVV_LOCAL_LIBELF")

is_user_controlled_lease_scheduler() {
    [ "${BENCHMARK_STRATEGY:-}" = "rvv_dirty_lease_user_observe" ] ||
        [ "${BENCHMARK_STRATEGY:-}" = "rvv_dirty_lease_user_active" ]
}

apply_strategy_preset() {
    SCX_ENABLED=1
    SCX_LOADER_STRATEGY="global"
    SCX_VECTOR_SLICE_MS="0"
    SCX_VECTOR_LOCAL_WINDOW_MS="0"
    SCX_DIRTY_THRESHOLD="2"
    SCX_LEASE_CONTROL_MODE="off"

    case "$BENCHMARK_STRATEGY" in
        cfs|baseline)
            BENCHMARK_STRATEGY="cfs"
            SCX_ENABLED=0
            ;;
        linux_affinity|affinity)
            BENCHMARK_STRATEGY="linux_affinity"
            SCX_ENABLED=0
            ;;
        scx_global)
            SCX_LOADER_STRATEGY="global"
            ;;
        rvv_dirty_all)
            SCX_LOADER_STRATEGY="rvv_dirty"
            SCX_VECTOR_SLICE_MS="100"
            SCX_VECTOR_LOCAL_WINDOW_MS="0"
            ;;
        rvv_dirty_lease|rvv_dirty_lease_all|rvv_dirty_lease_unique|rvv_dirty_lease_unique_all)
            SCX_LOADER_STRATEGY="rvv_dirty"
            SCX_VECTOR_SLICE_MS="100"
            SCX_VECTOR_LOCAL_WINDOW_MS="500"
            ;;
        rvv_dirty_lease_user_observe|rvv_dirty_lease_user_active)
            SCX_LOADER_STRATEGY="rvv_dirty"
            SCX_VECTOR_SLICE_MS="100"
            SCX_VECTOR_LOCAL_WINDOW_MS="500"
            if [ "$BENCHMARK_STRATEGY" = "rvv_dirty_lease_user_active" ]; then
                SCX_LEASE_CONTROL_MODE="active"
            else
                SCX_LEASE_CONTROL_MODE="observe"
            fi
            ;;
        rvv_dirty_local_all)
            SCX_LOADER_STRATEGY="rvv_dirty_local"
            SCX_VECTOR_SLICE_MS="100"
            SCX_VECTOR_LOCAL_WINDOW_MS="500"
            ;;
        *)
            log_error "未知策略: $BENCHMARK_STRATEGY"
            echo "可选策略: cfs, linux_affinity, scx_global, rvv_dirty_all, rvv_dirty_lease_all, rvv_dirty_lease_unique_all, rvv_dirty_lease_user_observe, rvv_dirty_lease_user_active, rvv_dirty_local_all"
            exit 1
            ;;
    esac
}

list_strategies() {
    cat << EOF
可选策略:
  cfs                    默认 CFS baseline，不加载 sched_ext
  linux_affinity         CFS + Linux task affinity worker pin，不加载 sched_ext
  scx_global             最小 sched_ext 全局队列
  rvv_dirty_all          旧版全核心 RVV Dirty，vector 每次 wakeup 进入 vector DSQ
  rvv_dirty_lease_all    500ms lease + 8个vector worker一核一个
  rvv_dirty_lease_unique_all  rvv_dirty_lease_all 的兼容别名
  rvv_dirty_lease_user_observe  dirty lease + 用户worker注册，仅观测
  rvv_dirty_lease_user_active   dirty lease + 用户CPU映射和运行时参数控制
  rvv_dirty_local_all    RVV Dirty vector 长时间片 + LLM 辅助线程本地 DSQ
EOF
}

# ========================================
# 环境检查
# ========================================

check_requirements() {
    log_header "检查环境依赖"

    local missing=0

    # 检查必需的命令
    for cmd in sshpass curl jq bc python3; do
        if ! command -v $cmd &> /dev/null; then
            log_error "缺少命令: $cmd"
            missing=1
        else
            log_info "✓ $cmd"
        fi
    done

    if [ $missing -eq 1 ]; then
        log_error "请安装缺失的依赖"
        exit 1
    fi

    # 测试SSH连接
    log_info "测试SSH连接到 $REMOTE_IP..."
    if ssh_exec "echo 'SSH连接成功'" &> /dev/null; then
        log_info "✓ SSH连接正常"
    else
        log_error "无法连接到 $REMOTE_IP"
        exit 1
    fi
}

# ========================================
# LLM服务器管理
# ========================================

start_llm_server() {
    log_header "启动LLM服务器"

    log_info "配置: -t $LLM_THREADS -tb $LLM_THREADS_BATCH -np $LLM_PARALLEL -c $LLM_CONTEXT, GOMP_SPINCOUNT=${GOMP_SPINCOUNT:-default}"

    if [ "${RESTART_LLM:-1}" = "1" ]; then
        # 停止现有服务器
        log_info "停止现有llama-server..."
        remote_kill_by_pattern "llama-server" "-9"
        sleep 2

        # 启动服务器
        log_info "启动llama-server..."
        local gomp_env=""
        local lease_hint_env=""
        if [ -n "${GOMP_SPINCOUNT:-}" ]; then
            gomp_env="GOMP_SPINCOUNT=$GOMP_SPINCOUNT"
        fi
        if is_user_controlled_lease_scheduler && [ "${LEASE_HINT_RUNTIME_PREPARED:-0}" -eq 1 ]; then
            lease_hint_env="GGML_SCX_LEASE_HINT_PATH=$SCX_LEASE_CONTROL_DIR/workers"
        fi
        ssh_exec "cd '$LLM_PATH' && $lease_hint_env $gomp_env THREADS=$LLM_THREADS THREADS_BATCH=$LLM_THREADS_BATCH PARALLEL=$LLM_PARALLEL CTX=$LLM_CONTEXT nohup ./run-server.sh > /tmp/llama-server.log 2>&1 &"
    else
        log_info "复用现有llama-server（RESTART_LLM=0）..."
    fi

    # 等待服务器就绪
    log_info "等待服务器就绪..."
    local retries=30
    for i in $(seq 1 $retries); do
        if curl -s "http://$REMOTE_IP:$REMOTE_PORT/health" &> /dev/null; then
            log_info "✓ 服务器已就绪 (尝试 $i/$retries)"
            break
        fi
        echo -n "."
        sleep 1
        if [ "$i" -eq "$retries" ]; then
            log_error "服务器启动超时"
            exit 1
        fi
    done

    warmup_llm_server
}

warmup_llm_server() {
    log_info "等待模型完成加载并执行warm-up请求..."
    local warmup_retries=60
    for i in $(seq 1 $warmup_retries); do
        local response
        response=$(curl -s --max-time 120 "http://$REMOTE_IP:$REMOTE_PORT/completion" \
            -H "Content-Type: application/json" \
            -d '{"prompt":"Hello","n_predict":1,"temperature":0,"stream":false}' || true)

        if echo "$response" | jq -e '.tokens_predicted' > /dev/null 2>&1; then
            log_info "✓ 模型warm-up完成 (尝试 $i/$warmup_retries)"
            return 0
        fi

        echo -n "."
        sleep 2
    done

    log_error "模型warm-up超时"
    exit 1
}

validate_warmup_stream() {
    python3 - "$1" "$2" <<'PY'
import json
import sys

path = sys.argv[1]
expected_tokens = int(sys.argv[2])
token_count = 0
stopped = False

with open(path, "r", encoding="utf-8") as stream:
    for raw_line in stream:
        line = raw_line.strip()
        if not line.startswith("data:"):
            continue

        data = line[5:].strip()
        if not data or data == "[DONE]":
            continue

        try:
            chunk = json.loads(data)
        except json.JSONDecodeError:
            continue

        tokens = chunk.get("tokens") or []
        if isinstance(tokens, list):
            token_count += len(tokens)
        if chunk.get("stop"):
            stopped = True

if token_count < expected_tokens or not stopped:
    sys.exit(1)
PY
}

warmup_before_benchmark() {
    local result_dir=$1
    local warmup_dir="$result_dir/pre_bench_warmup"
    local status_file="$warmup_dir/status.txt"

    mkdir -p "$warmup_dir"

    if [ "${PRE_BENCH_WARMUP:-1}" != "1" ]; then
        log_warn "正式测试前warm-up已关闭（PRE_BENCH_WARMUP=0）"
        {
            echo "status=disabled"
            echo "requests=0"
            echo "tokens=0"
            echo "attempts=0"
        } > "$status_file"
        return 0
    fi

    local warmup_requests="${PRE_BENCH_WARMUP_REQUESTS:-$CONCURRENT_TASKS}"
    local warmup_tokens="${PRE_BENCH_WARMUP_TOKENS:-1}"
    local warmup_retries="${PRE_BENCH_WARMUP_RETRIES:-3}"
    local warmup_timeout="${PRE_BENCH_WARMUP_TIMEOUT:-120}"

    log_header "正式测试前warm-up"
    log_info "并发预热: requests=$warmup_requests, tokens=$warmup_tokens, retries=$warmup_retries, timeout=${warmup_timeout}s"

    local attempt
    for attempt in $(seq 1 "$warmup_retries"); do
        local attempt_dir="$warmup_dir/attempt${attempt}"
        local pids=()
        local failed=0

        mkdir -p "$attempt_dir"
        log_info "warm-up尝试 $attempt/$warmup_retries..."

        local i
        for i in $(seq 1 "$warmup_requests"); do
            (
                curl -sN --no-buffer --max-time "$warmup_timeout" "http://$REMOTE_IP:$REMOTE_PORT/completion" \
                    -H "Content-Type: application/json" \
                    -d "{\"prompt\":\"pre benchmark warmup slot $i\",\"n_predict\":$warmup_tokens,\"temperature\":0,\"stream\":true}" \
                    > "$attempt_dir/slot${i}.sse"
            ) &
            pids+=($!)
        done

        local pid
        for pid in "${pids[@]}"; do
            if ! wait "$pid"; then
                failed=1
            fi
        done

        for i in $(seq 1 "$warmup_requests"); do
            if ! validate_warmup_stream "$attempt_dir/slot${i}.sse" "$warmup_tokens"; then
                log_warn "warm-up slot $i 未收到完整流式响应"
                failed=1
            fi
        done

        if [ "$failed" -eq 0 ]; then
            {
                echo "status=ok"
                echo "requests=$warmup_requests"
                echo "tokens=$warmup_tokens"
                echo "attempts=$attempt"
                echo "dir=pre_bench_warmup/attempt${attempt}"
            } > "$status_file"
            log_info "✓ 正式测试前warm-up完成"
            return 0
        fi

        log_warn "warm-up尝试 $attempt/$warmup_retries 失败"
        sleep 2
    done

    {
        echo "status=failed"
        echo "requests=$warmup_requests"
        echo "tokens=$warmup_tokens"
        echo "attempts=$warmup_retries"
        echo "dir=pre_bench_warmup/attempt${warmup_retries}"
    } > "$status_file"

    if [ "${PRE_BENCH_WARMUP_STRICT:-1}" = "1" ]; then
        log_error "正式测试前warm-up失败，终止完整benchmark"
        exit 1
    fi

    log_warn "正式测试前warm-up失败，但 PRE_BENCH_WARMUP_STRICT=0，继续benchmark"
}

stop_llm_server() {
    log_info "停止LLM服务器..."
    remote_kill_by_pattern "llama-server" "-9"
}

run_phase_gate() {
    local result_dir=$1
    local gate_script="$SCRIPT_DIR/phase_gate.py"
    local attempt=1
    local max_retries=${PHASE_GATE_MAX_RETRIES:-5}

    if [ "${PHASE_GATE_ENABLED:-1}" != "1" ]; then
        log_warn "正式测试前相位门禁已关闭（PHASE_GATE_ENABLED=0）"
        return 0
    fi

    while [ "$attempt" -le "$max_retries" ]; do
        local gate_output="$result_dir/phase_gate_attempt${attempt}.json"
        log_header "相位门禁 $attempt/$max_retries"
        log_info "统一释放首请求并检查首 token spread <= ${PHASE_GATE_THRESHOLD_MS}ms"

        set +e
        python3 "$gate_script" \
            --url "http://$REMOTE_IP:$REMOTE_PORT/completion" \
            --prompt "$PROMPT_1" \
            --concurrency "$CONCURRENT_TASKS" \
            --tokens "${PHASE_GATE_TOKENS:-1}" \
            --threshold-ms "${PHASE_GATE_THRESHOLD_MS:-100}" \
            --lead-sec "${SYNC_START_LEAD_SEC:-3}" \
            --timeout-sec "${PHASE_GATE_TIMEOUT_SEC:-60}" \
            --output "$gate_output" > "$result_dir/phase_gate_attempt${attempt}.log"
        local rc=$?
        set -e

        if [ "$rc" -eq 0 ]; then
            cp "$gate_output" "$result_dir/phase_gate.json"
            log_info "✓ 相位门禁通过: $(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["spread_ms"])' "$gate_output")ms"
            return 0
        fi
        if [ "$rc" -ne 2 ]; then
            log_error "相位门禁执行失败，详见 $result_dir/phase_gate_attempt${attempt}.log"
            stop_scheduler "$result_dir"
            stop_llm_server
            return 1
        fi

        log_warn "相位门禁未通过，将重启 server/scheduler 后重试"
        if [ "$attempt" -eq "$max_retries" ]; then
            log_error "相位门禁连续 $max_retries 次未通过"
            stop_scheduler "$result_dir"
            stop_llm_server
            return 1
        fi
        stop_scheduler "$result_dir"
        stop_llm_server
        sleep 2
        start_llm_server
        start_scheduler "$result_dir"
        warmup_before_benchmark "$result_dir"
        pin_llm_workers_affinity "phase_gate_retry${attempt}" "$result_dir"
        attempt=$((attempt + 1))
    done

    return 1
}

# ========================================
# sched_ext调度器管理
# ========================================

SCX_ACTIVE=0
SCX_ACTIVE_LOG=""
SCX_ACTIVE_PATTERN="scx_llm_pin"
CURRENT_RESULT_DIR=""
LEASE_HINT_RUNTIME_PREPARED=0
LEASE_HINT_BPFFS_MOUNTED=0
LEASE_HINT_REMOTE_BACKUP=""
LEASE_HINT_ORIGINAL_SHA=""

deploy_scheduler() {
    local local_bin
    local remote_dir
    local remote_bin
    local build_target

    local_bin="$SCX_LOCAL_BIN"
    remote_dir="$SCX_REMOTE_DIR"
    remote_bin="scx_llm_pin"
    build_target="experiments/scx_llm_pin"

    if [ ! -x "$local_bin" ]; then
        log_error "找不到可执行调度器: $local_bin"
        log_error "请先执行: make -C $build_target BOARD_IP=$REMOTE_IP"
        exit 1
    fi

    log_info "部署调度器到板卡: $remote_dir/$remote_bin"
    ssh_exec "mkdir -p '$remote_dir'"
    scp_upload "$local_bin" "$remote_dir/$remote_bin"
    ssh_exec "chmod +x '$remote_dir/$remote_bin'"
    if is_user_controlled_lease_scheduler; then
        if [ ! -x "$SCX_LEASE_CTL_LOCAL_BIN" ]; then
            log_error "找不到 lease 控制工具: $SCX_LEASE_CTL_LOCAL_BIN"
            exit 1
        fi
        scp_upload "$SCX_LEASE_CTL_LOCAL_BIN" "$remote_dir/rvv_lease_ctl"
        ssh_exec "chmod +x '$remote_dir/rvv_lease_ctl'"
    fi
}

select_worker_tids() {
    local llm_pid=$1
    local limit=${2:-$LLM_THREADS}
    ssh_exec "for t in /proc/$llm_pid/task/*; do tid=\${t##*/}; runtime=\$(awk '{print \$1}' \$t/schedstat 2>/dev/null || echo 0); echo \$runtime \$tid; done | sort -nr | head -n $limit | awk '{if (out) out=out \",\" \$2; else out=\$2} END {print out}'"
}

pin_llm_workers_affinity() {
    local stage=$1
    local result_dir=$2
    local llm_pid
    local tid_list
    local cpus=()
    local tids=()
    local pin_log
    local affinity_setter="taskset"

    if [ "${AFFINITY_PIN_WORKERS:-0}" != "1" ]; then
        return 0
    fi

    llm_pid=$(remote_pid_by_pattern "llama-server")
    if [ -z "$llm_pid" ]; then
        log_error "AFFINITY_PIN_WORKERS=1 但找不到 llama-server PID"
        exit 1
    fi

    if ! ssh_exec "command -v taskset >/dev/null 2>&1"; then
        if ssh_exec "python3 -c 'import os; assert hasattr(os, \"sched_setaffinity\")'" >/dev/null 2>&1; then
            affinity_setter="python3"
        else
            log_error "板端缺少 taskset，且 python3 不支持 os.sched_setaffinity"
            exit 1
        fi
    fi

    mapfile -t cpus < <(expand_cpu_list "$AFFINITY_WORKER_CPUS")
    if [ "${#cpus[@]}" -eq 0 ]; then
        log_error "AFFINITY_WORKER_CPUS 无法解析: $AFFINITY_WORKER_CPUS"
        exit 1
    fi

    tid_list=$(select_worker_tids "$llm_pid" "${AFFINITY_WORKER_COUNT:-$LLM_THREADS}")
    if [ -z "$tid_list" ]; then
        log_error "无法选择 llama-server worker TID"
        exit 1
    fi

    IFS=',' read -r -a tids <<< "$tid_list"
    pin_log="$result_dir/affinity_${stage}.txt"

    log_header "Linux affinity worker pin: $stage"
    log_info "llama_pid=$llm_pid, worker_count=${#tids[@]}, cpus=$AFFINITY_WORKER_CPUS"

    {
        echo "stage=$stage"
        echo "llama_pid=$llm_pid"
        echo "worker_count=${#tids[@]}"
        echo "worker_cpus=$AFFINITY_WORKER_CPUS"
        echo "affinity_setter=$affinity_setter"
        echo "selected_tids=$tid_list"
        echo ""
    } > "$pin_log"

    local i
    for i in "${!tids[@]}"; do
        local tid=${tids[$i]}
        local cpu=${cpus[$((i % ${#cpus[@]}))]}
        log_info "pin TID $tid -> CPU $cpu"
        {
            echo "pin tid=$tid cpu=$cpu"
            if [ "$affinity_setter" = "taskset" ]; then
                ssh_exec "taskset -pc '$cpu' '$tid' 2>&1; awk '/Cpus_allowed_list/ {print}' /proc/'$tid'/status 2>/dev/null; awk '/se.nr_migrations/ {print}' /proc/'$tid'/sched 2>/dev/null; awk '{print \"schedstat_runtime_ns=\" \$1, \"wait_ns=\" \$2, \"timeslices=\" \$3}' /proc/'$tid'/schedstat 2>/dev/null"
            else
                ssh_exec "python3 -c \"import os; os.sched_setaffinity($tid, {$cpu})\" 2>&1 && echo 'pid $tid affinity set to CPU $cpu'; awk '/Cpus_allowed_list/ {print}' /proc/'$tid'/status 2>/dev/null; awk '/se.nr_migrations/ {print}' /proc/'$tid'/sched 2>/dev/null; awk '{print \"schedstat_runtime_ns=\" \$1, \"wait_ns=\" \$2, \"timeslices=\" \$3}' /proc/'$tid'/schedstat 2>/dev/null"
            fi
            echo ""
        } >> "$pin_log"
    done
}

start_scheduler() {
    local result_dir=$1
    local llm_pid
    local scheduler_dir="$SCX_REMOTE_DIR"
    local scheduler_bin="scx_llm_pin"
    local scheduler_pattern="scx_llm_pin"

    if [ "${SCX_ENABLED:-0}" -ne 1 ]; then
        log_info "策略: $BENCHMARK_STRATEGY（不加载 sched_ext 调度器）"
        return 0
    fi

    log_header "启动sched_ext策略: $BENCHMARK_STRATEGY"

    if [ "$SCX_AUTO_DEPLOY" = "1" ]; then
        deploy_scheduler
    fi

    llm_pid=$(remote_pid_by_pattern "llama-server")
    if [ -z "$llm_pid" ]; then
        log_error "无法找到远端 llama-server PID"
        exit 1
    fi

    SCX_ACTIVE_PATTERN="$scheduler_pattern"
    SCX_ACTIVE_LOG="/tmp/${scheduler_bin}_${BENCHMARK_STRATEGY}_$(date +%s).log"

    log_info "清理已有 sched_ext 调度器..."
    remote_kill_by_pattern "scx_llm_pin" "-TERM"
    sleep 1

    local lease_control_args=""
    if is_user_controlled_lease_scheduler; then
        lease_control_args="--user-control '$SCX_LEASE_CONTROL_MODE' --control-dir '$SCX_LEASE_CONTROL_DIR' --user-cpu-map '$SCX_LEASE_USER_CPU_MAP'"
    fi

    log_info "启动策略: loader=$SCX_LOADER_STRATEGY, tgid=$llm_pid, vector_slice=${SCX_VECTOR_SLICE_MS}ms, vector_local_window=${SCX_VECTOR_LOCAL_WINDOW_MS}ms, dirty_threshold=$SCX_DIRTY_THRESHOLD"
    ssh_exec "cd '$scheduler_dir' && nohup ./$scheduler_bin --tgid '$llm_pid' --strategy '$SCX_LOADER_STRATEGY' --vector-slice-ms '$SCX_VECTOR_SLICE_MS' --vector-local-window-ms '$SCX_VECTOR_LOCAL_WINDOW_MS' --dirty-threshold '$SCX_DIRTY_THRESHOLD' $lease_control_args > '$SCX_ACTIVE_LOG' 2>&1 &"
    sleep 3

    if [ -n "$(remote_pid_by_pattern "$scheduler_pattern")" ]; then
        SCX_ACTIVE=1
        log_info "✓ sched_ext策略已启动"
        ssh_exec "cat /sys/kernel/sched_ext/state 2>/dev/null || true" > "$result_dir/sched_ext_state_start.txt"
    else
        log_error "sched_ext策略启动失败，远端日志如下："
        ssh_exec "cat '$SCX_ACTIVE_LOG' 2>/dev/null || true"
        exit 1
    fi
}

stop_scheduler() {
    local result_dir=${1:-}

    if [ "${SCX_ACTIVE:-0}" -ne 1 ]; then
        return 0
    fi

    log_info "停止sched_ext调度器..."
    remote_kill_by_pattern "$SCX_ACTIVE_PATTERN" "-TERM"
    sleep 2

    if [ -n "$result_dir" ] && [ -d "$result_dir" ]; then
        ssh_exec "cat /sys/kernel/sched_ext/state 2>/dev/null || true" > "$result_dir/sched_ext_state_end.txt"
        if [ -n "$SCX_ACTIVE_LOG" ]; then
            sshpass -p "$REMOTE_PASS" scp -o StrictHostKeyChecking=no "$REMOTE_USER@$REMOTE_IP:$SCX_ACTIVE_LOG" "$result_dir/scheduler.log" >/dev/null 2>&1 || true
        fi
    fi

    SCX_ACTIVE=0
}

prepare_lease_hint_runtime() {
    if ! is_user_controlled_lease_scheduler || [ "$LEASE_HINT_RUNTIME_PREPARED" -eq 1 ]; then
        return 0
    fi
    if [ ! -f "$SCX_LEASE_HINT_LOCAL_LIB" ]; then
        log_error "找不到 lease hint 版 libggml-cpu: $SCX_LEASE_HINT_LOCAL_LIB"
        exit 1
    fi

    log_header "准备 dirty lease userspace control runtime"
    stop_llm_server
    ssh_exec "mkdir -p '/sys/fs/bpf' '$SCX_REMOTE_DIR/runtime'"
    if ! ssh_exec "awk '\$2 == \"/sys/fs/bpf\" && \$3 == \"bpf\" { found=1 } END { exit !found }' /proc/mounts"; then
        ssh_exec "mount -t bpf bpf /sys/fs/bpf"
        LEASE_HINT_BPFFS_MOUNTED=1
    fi

    LEASE_HINT_REMOTE_BACKUP="$SCX_REMOTE_DIR/runtime/libggml-cpu.so.0.13.0.original.$$"
    ssh_exec "test -f '$SCX_LEASE_HINT_REMOTE_LIB' && cp -f '$SCX_LEASE_HINT_REMOTE_LIB' '$LEASE_HINT_REMOTE_BACKUP'"
    LEASE_HINT_ORIGINAL_SHA=$(ssh_exec "sha256sum '$LEASE_HINT_REMOTE_BACKUP' | awk '{print \$1}'")
    LEASE_HINT_RUNTIME_PREPARED=1
    scp_upload "$SCX_LEASE_HINT_LOCAL_LIB" "$SCX_REMOTE_DIR/runtime/libggml-cpu.so.0.13.0.lease-hint"
    ssh_exec "cp -f '$SCX_REMOTE_DIR/runtime/libggml-cpu.so.0.13.0.lease-hint' '$SCX_LEASE_HINT_REMOTE_LIB'; sync"

    local expected_sha
    local deployed_sha
    expected_sha=$(sha256sum "$SCX_LEASE_HINT_LOCAL_LIB" | awk '{print $1}')
    deployed_sha=$(ssh_exec "sha256sum '$SCX_LEASE_HINT_REMOTE_LIB' | awk '{print \$1}'")
    if [ "$deployed_sha" != "$expected_sha" ]; then
        log_error "lease hint libggml-cpu 部署哈希不匹配"
        exit 1
    fi
    log_info "✓ lease hint runtime 已部署: $deployed_sha"
}

restore_lease_hint_runtime() {
    if [ "$LEASE_HINT_RUNTIME_PREPARED" -ne 1 ]; then
        return 0
    fi

    log_header "恢复默认 llama.cpp runtime"
    stop_llm_server
    ssh_exec "rm -f '$SCX_LEASE_CONTROL_DIR/config' '$SCX_LEASE_CONTROL_DIR/workers'; rmdir '$SCX_LEASE_CONTROL_DIR' 2>/dev/null || true"
    ssh_exec "cp -f '$LEASE_HINT_REMOTE_BACKUP' '$SCX_LEASE_HINT_REMOTE_LIB'; sync"
    local restored_sha
    restored_sha=$(ssh_exec "sha256sum '$SCX_LEASE_HINT_REMOTE_LIB' | awk '{print \$1}'")
    if [ "$restored_sha" != "$LEASE_HINT_ORIGINAL_SHA" ]; then
        log_error "恢复原始 libggml-cpu 后哈希不匹配"
        return 1
    fi
    ssh_exec "rm -f '$LEASE_HINT_REMOTE_BACKUP'"
    if [ "$LEASE_HINT_BPFFS_MOUNTED" -eq 1 ]; then
        ssh_exec "umount /sys/fs/bpf"
        LEASE_HINT_BPFFS_MOUNTED=0
    fi
    LEASE_HINT_RUNTIME_PREPARED=0
    if ! start_llm_server; then
        log_error "原始 libggml-cpu 已恢复，但默认 llama-server 重启失败"
        return 1
    fi
    log_info "✓ 已恢复原始 libggml-cpu 和默认服务"
}

validate_user_controlled_lease_ready() {
    local result_dir=$1
    if ! is_user_controlled_lease_scheduler; then
        return 0
    fi

    local attempt
    for attempt in $(seq 1 40); do
        local checks=""
        local ith
        for ith in $(seq 0 7); do
            checks="$checks grep -q 'lease_worker team=0 ith=$ith ' '$SCX_ACTIVE_LOG' &&"
        done
        if ssh_exec "${checks%&&}" >/dev/null 2>&1 && \
           ssh_exec "grep -Eq 'User hint valid workers:[[:space:]]+8' '$SCX_ACTIVE_LOG'" && \
           { [ "$SCX_LEASE_CONTROL_MODE" != "observe" ] || \
             ssh_exec "grep -Eq 'User hint observed workers:[[:space:]]+8' '$SCX_ACTIVE_LOG'"; } && \
           { [ "$SCX_LEASE_CONTROL_MODE" != "active" ] || \
             ssh_exec "grep -Eq 'User hint active workers:[[:space:]]+8' '$SCX_ACTIVE_LOG'"; } && \
           { [ "$SCX_LEASE_CONTROL_MODE" != "active" ] || \
             [ "$SCX_LEASE_USER_CPU_MAP" = "auto" ] || \
             ssh_exec "grep -Eq 'User hint active assignments:[[:space:]]+8' '$SCX_ACTIVE_LOG'"; }; then
            ssh_exec "cd '$SCX_REMOTE_DIR' && ./rvv_lease_ctl status --control-dir '$SCX_LEASE_CONTROL_DIR'" \
                > "$result_dir/rvv_lease_ctl_status.txt"
            if ! grep -qx "mode=$SCX_LEASE_CONTROL_MODE" "$result_dir/rvv_lease_ctl_status.txt" ||
               ! grep -qx "cpu_map=$SCX_LEASE_USER_CPU_MAP" "$result_dir/rvv_lease_ctl_status.txt"; then
                log_error "dirty lease 控制状态与请求配置不一致"
                exit 1
            fi
            echo "status=ready" > "$result_dir/lease_control_qualification.txt"
            echo "mode=$SCX_LEASE_CONTROL_MODE" >> "$result_dir/lease_control_qualification.txt"
            echo "attempt=$attempt" >> "$result_dir/lease_control_qualification.txt"
            log_info "✓ dirty lease 用户控制已注册 8 个 worker（$SCX_LEASE_CONTROL_MODE）"
            return 0
        fi
        sleep 0.25
    done

    echo "status=failed" > "$result_dir/lease_control_qualification.txt"
    log_error "dirty lease userspace control qualification 失败"
    ssh_exec "tail -160 '$SCX_ACTIVE_LOG' 2>/dev/null || true"
    exit 1
}

# ========================================
# rvv_sched_cost观测工具管理
# ========================================

RVV_TRACE_ACTIVE=0
RVV_TRACE_LOG=""
RVV_TRACE_JSON=""

deploy_rvv_trace() {
    if [ ! -x "$RVV_LOCAL_BIN" ]; then
        log_error "找不到rvv_sched_cost可执行文件: $RVV_LOCAL_BIN"
        log_error "请先执行: make -C experiments/rvv_sched_cost BOARD_IP=$REMOTE_IP"
        exit 1
    fi

    log_info "部署rvv_sched_cost到板卡: $RVV_REMOTE_DIR"
    ssh_exec "mkdir -p '$RVV_REMOTE_DIR/lib'"
    scp_upload "$RVV_LOCAL_BIN" "$RVV_REMOTE_DIR/rvv_sched_cost"
    ssh_exec "chmod +x '$RVV_REMOTE_DIR/rvv_sched_cost'"

    if [ -f "$RVV_LOCAL_LIBELF" ]; then
        scp_upload "$RVV_LOCAL_LIBELF" "$RVV_REMOTE_DIR/lib/libelf-0.186.so"
        ssh_exec "ln -sf libelf-0.186.so '$RVV_REMOTE_DIR/lib/libelf.so.1'"
    fi
}

start_rvv_trace() {
    local result_dir=$1
    local llm_pid
    local duration
    local switch_out_arg=""
    local switch_reason_arg=""
    local rvv_only_arg=""
    local futex_arg=""
    local futex_detail_arg=""
    local futex_stack_arg=""

    if [ "${RVV_TRACE:-0}" -ne 1 ]; then
        return 0
    fi

    log_header "启动rvv_sched_cost观测"

    if [ "$RVV_TRACE_AUTO_DEPLOY" = "1" ]; then
        deploy_rvv_trace
    fi

    llm_pid=$(remote_pid_by_pattern "llama-server")
    if [ -z "$llm_pid" ]; then
        log_error "无法找到远端 llama-server PID，无法启动rvv_sched_cost"
        exit 1
    fi

    if [ "${FIXED_TOTAL_TOKENS:-1000}" -gt 0 ]; then
        duration=$((${FIXED_REQUEST_TIMEOUT_SEC:-600} + RVV_TRACE_EXTRA_SEC))
    else
        duration=$((TEST_DURATION + RVV_TRACE_EXTRA_SEC))
    fi
    RVV_TRACE_LOG="/tmp/rvv_sched_cost_${BENCHMARK_STRATEGY}_$(date +%s).log"
    RVV_TRACE_JSON="/tmp/rvv_sched_cost_${BENCHMARK_STRATEGY}_$(date +%s).json"
    if [ "${RVV_TRACE_SWITCH_OUT:-1}" = "0" ]; then
        switch_out_arg="--no-switch-out"
    fi
    if [ "${RVV_TRACE_SWITCH_REASON:-1}" = "0" ]; then
        switch_reason_arg="--no-switch-out-state"
    fi
    if [ "${RVV_TRACE_RVV_ONLY:-0}" = "1" ]; then
        rvv_only_arg="--rvv-only"
    fi
    if [ "${RVV_TRACE_FUTEX:-0}" = "1" ]; then
        futex_arg="--futex"
    fi
    if [ "${RVV_TRACE_FUTEX_DETAIL:-0}" = "1" ]; then
        futex_detail_arg="--futex-detail"
    fi
    if [ "${RVV_TRACE_FUTEX_STACK:-0}" = "1" ]; then
        futex_stack_arg="--futex-stack"
    fi

    remote_kill_by_pattern "rvv_sched_cost" "-TERM"
    sleep 1

    ssh_exec "cat /proc/$llm_pid/maps" > "$result_dir/llama_server_maps.txt" 2>/dev/null || true

    log_info "启动rvv_sched_cost: tgid=$llm_pid duration=${duration}s rvv_only=${RVV_TRACE_RVV_ONLY:-0} switch_out=${RVV_TRACE_SWITCH_OUT:-1} switch_reason=${RVV_TRACE_SWITCH_REASON:-1} futex=${RVV_TRACE_FUTEX:-0} futex_detail=${RVV_TRACE_FUTEX_DETAIL:-0} futex_stack=${RVV_TRACE_FUTEX_STACK:-0}"
    ssh_exec "cd '$RVV_REMOTE_DIR' && LD_LIBRARY_PATH='$RVV_REMOTE_DIR/lib' nohup ./rvv_sched_cost --tgid '$llm_pid' --duration '$duration' --json '$RVV_TRACE_JSON' $rvv_only_arg $switch_out_arg $switch_reason_arg $futex_arg $futex_detail_arg $futex_stack_arg > '$RVV_TRACE_LOG' 2>&1 &"
    sleep 2

    if [ -n "$(remote_pid_by_pattern "rvv_sched_cost")" ]; then
        RVV_TRACE_ACTIVE=1
        echo "rvv_sched_cost log: $RVV_TRACE_LOG" > "$result_dir/rvv_trace_info.txt"
        echo "rvv_sched_cost json: $RVV_TRACE_JSON" >> "$result_dir/rvv_trace_info.txt"
        echo "rvv_sched_cost switch_out: ${RVV_TRACE_SWITCH_OUT:-1}" >> "$result_dir/rvv_trace_info.txt"
        echo "rvv_sched_cost switch_reason: ${RVV_TRACE_SWITCH_REASON:-1}" >> "$result_dir/rvv_trace_info.txt"
        echo "rvv_sched_cost rvv_only: ${RVV_TRACE_RVV_ONLY:-0}" >> "$result_dir/rvv_trace_info.txt"
        echo "rvv_sched_cost futex: ${RVV_TRACE_FUTEX:-0}" >> "$result_dir/rvv_trace_info.txt"
        echo "rvv_sched_cost futex_detail: ${RVV_TRACE_FUTEX_DETAIL:-0}" >> "$result_dir/rvv_trace_info.txt"
        echo "rvv_sched_cost futex_stack: ${RVV_TRACE_FUTEX_STACK:-0}" >> "$result_dir/rvv_trace_info.txt"
        log_info "✓ rvv_sched_cost已启动"
    else
        log_error "rvv_sched_cost启动失败，远端日志如下："
        ssh_exec "cat '$RVV_TRACE_LOG' 2>/dev/null || true"
        exit 1
    fi
}

stop_rvv_trace() {
    local result_dir=${1:-}

    if [ "${RVV_TRACE_ACTIVE:-0}" -ne 1 ]; then
        return 0
    fi

    log_info "停止rvv_sched_cost观测..."
    remote_kill_by_pattern "rvv_sched_cost" "-TERM"
    sleep 3

    if [ -n "$result_dir" ] && [ -d "$result_dir" ]; then
        if [ -n "$RVV_TRACE_LOG" ]; then
            sshpass -p "$REMOTE_PASS" scp -o StrictHostKeyChecking=no "$REMOTE_USER@$REMOTE_IP:$RVV_TRACE_LOG" "$result_dir/rvv_sched_cost.log" >/dev/null 2>&1 || true
        fi
        if [ -n "$RVV_TRACE_JSON" ]; then
            sshpass -p "$REMOTE_PASS" scp -o StrictHostKeyChecking=no "$REMOTE_USER@$REMOTE_IP:$RVV_TRACE_JSON" "$result_dir/rvv_sched_cost.json" >/dev/null 2>&1 || true
        fi
    fi

    RVV_TRACE_ACTIVE=0
}

# ========================================
# 单个任务测试
# ========================================

get_task_prompt() {
    local task_id=$1
    local prompt_index=$(( (task_id - 1) % 4 + 1 ))

    case "$prompt_index" in
        1) printf '%s\n' "$PROMPT_1" ;;
        2) printf '%s\n' "$PROMPT_2" ;;
        3) printf '%s\n' "$PROMPT_3" ;;
        4) printf '%s\n' "$PROMPT_4" ;;
    esac
}

run_single_task() {
    local task_id=$1
    local prompt=$2
    local output_file=$3
    local sync_mono_start=${4:-}
    local sync_wall_start=${5:-}
    local requested_tokens=${6:-$TOKENS_PER_REQUEST}
    local fixed_mode=${7:-0}
    local task_timeout=${8:-${REQUEST_TIMEOUT_SEC:-180}}

    log_info "[Task $task_id] 开始测试..."

    python3 - "$task_id" "$prompt" "$output_file" "$REMOTE_IP" "$REMOTE_PORT" \
        "$TEST_DURATION" "$requested_tokens" "$task_timeout" \
        "$sync_mono_start" "$sync_wall_start" "${SYNC_START_SPIN_MS:-2}" \
        "${REQUEST_TEMPERATURE:-0}" "${REQUEST_SEED:-1234}" "${REQUEST_IGNORE_EOS:-1}" \
        "$fixed_mode" <<'PY'
import csv
import json
import subprocess
import sys
import time

task_id = int(sys.argv[1])
prompt = sys.argv[2]
output_file = sys.argv[3]
remote_ip = sys.argv[4]
remote_port = sys.argv[5]
test_duration = float(sys.argv[6])
tokens_per_request = int(sys.argv[7])
request_timeout = float(sys.argv[8])
sync_mono_start = float(sys.argv[9]) if sys.argv[9] else None
sync_wall_start = float(sys.argv[10]) if sys.argv[10] else None
sync_spin_sec = max(0.0, float(sys.argv[11]) / 1000.0)
request_temperature = float(sys.argv[12])
request_seed = int(sys.argv[13])
request_ignore_eos = sys.argv[14] == "1"
fixed_mode = sys.argv[15] == "1"

if sync_mono_start is not None:
    # Sleep until the shared release point, then use a short bounded spin to
    # reduce wakeup jitter without spinning for the whole lead interval.
    while True:
        remaining = sync_mono_start - time.monotonic()
        if remaining <= sync_spin_sec:
            break
        time.sleep(remaining - sync_spin_sec)
    while time.monotonic() < sync_mono_start:
        pass

url = f"http://{remote_ip}:{remote_port}/completion"
csv_path = f"{output_file}.csv"
summary_path = f"{output_file}.summary"

start_time = sync_wall_start if sync_wall_start is not None else time.time()
deadline = start_time + (request_timeout if fixed_mode else test_duration)
first_token_time = None
first_token_timestamp = None
last_token_time = None
total_tokens = 0
request_count = 0

with open(csv_path, "w", newline="") as csv_file:
    writer = csv.writer(csv_file)
    writer.writerow([
        "timestamp",
        "request_num",
        "tokens",
        "generation_ms",
        "ttft_ms",
        "last_token_ms",
        "total_ms",
        "toks_per_sec",
        "completed",
        "stream_cutoff",
    ])

    while time.time() < deadline:
        remaining = deadline - time.time()
        if remaining <= 0:
            break

        req_start = time.time()
        max_time = max(0.001, min(remaining, request_timeout))
        payload = {
            "prompt": prompt,
            "n_predict": tokens_per_request,
            "temperature": request_temperature,
            "seed": request_seed,
            "ignore_eos": request_ignore_eos,
            "stream": True,
        }
        cmd = [
            "curl",
            "-sN",
            "--no-buffer",
            "--max-time",
            f"{max_time:.3f}",
            url,
            "-H",
            "Content-Type: application/json",
            "-d",
            json.dumps(payload, ensure_ascii=False),
        ]

        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
        )

        req_tokens = 0
        req_first_token_ms = None
        req_last_token_ms = None
        generation_ms = 0.0
        completed = 0
        stream_cutoff = 0

        try:
            assert proc.stdout is not None
            for raw_line in proc.stdout:
                now = time.time()
                if now > deadline:
                    stream_cutoff = 1
                    proc.kill()
                    break

                line = raw_line.strip()
                if not line.startswith("data:"):
                    continue

                data = line[5:].strip()
                if not data or data == "[DONE]":
                    continue

                try:
                    chunk = json.loads(data)
                except json.JSONDecodeError:
                    continue

                chunk_tokens = chunk.get("tokens") or []
                if isinstance(chunk_tokens, list) and chunk_tokens:
                    token_count = len(chunk_tokens)
                    req_tokens += token_count
                    total_tokens += token_count
                    req_last_token_ms = (now - req_start) * 1000.0
                    last_token_time = now
                    if req_first_token_ms is None:
                        req_first_token_ms = req_last_token_ms
                    if first_token_time is None:
                        first_token_time = req_first_token_ms
                        first_token_timestamp = now
                        print(f"[INFO] [Task {task_id}] 首字延迟: {first_token_time:.3f}ms", flush=True)

                if chunk.get("stop"):
                    completed = 1
                    timings = chunk.get("timings") or {}
                    generation_ms = float(timings.get("predicted_ms") or 0.0)
        finally:
            try:
                proc.wait(timeout=1)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

        req_end = time.time()
        if req_end >= deadline and not completed:
            stream_cutoff = 1

        if req_tokens > 0 or completed:
            request_count += 1
            total_ms = (req_end - req_start) * 1000.0
            token_ms = req_last_token_ms if req_last_token_ms is not None else total_ms
            toks_per_sec = req_tokens / (token_ms / 1000.0) if token_ms > 0 else 0.0
            writer.writerow([
                f"{time.time():.9f}",
                request_count,
                req_tokens,
                f"{generation_ms:.3f}",
                "" if req_first_token_ms is None else f"{req_first_token_ms:.3f}",
                "" if req_last_token_ms is None else f"{req_last_token_ms:.3f}",
                f"{total_ms:.3f}",
                f"{toks_per_sec:.2f}",
                completed,
                stream_cutoff,
            ])
            csv_file.flush()

        if fixed_mode:
            if not completed or req_tokens != tokens_per_request:
                raise SystemExit(
                    f"fixed-token request incomplete: expected={tokens_per_request} "
                    f"received={req_tokens} completed={completed}"
                )
            break

        if time.time() < deadline:
            time.sleep(0.5)

duration = (last_token_time - start_time) if last_token_time is not None else 0.0
avg_toks = total_tokens / duration if duration > 0 else 0.0
first = "" if first_token_time is None else f"{first_token_time:.3f}"
first_ts = "" if first_token_timestamp is None else f"{first_token_timestamp:.9f}"
last_ts = "" if last_token_time is None else f"{last_token_time:.9f}"

with open(summary_path, "w") as f:
    f.write(f"Task ID: {task_id}\n")
    f.write(f"Prompt: {prompt[:50]}...\n")
    f.write(f"Configured Duration: {test_duration:.3f}s\n")
    f.write(f"Fixed Token Mode: {int(fixed_mode)}\n")
    f.write(f"Requested Tokens: {tokens_per_request}\n")
    f.write(f"Start Timestamp: {start_time:.9f}\n")
    f.write(f"First Token Timestamp: {first_ts}\n")
    f.write(f"Last Token Timestamp: {last_ts}\n")
    f.write(f"Duration: {duration:.3f}s\n")
    f.write(f"Total Requests: {request_count}\n")
    f.write(f"Total Tokens: {total_tokens}\n")
    f.write(f"Average tok/s: {avg_toks:.2f}\n")
    f.write(f"First Token Time: {first}ms\n")

print(f"[INFO] [Task {task_id}] 完成 - {avg_toks:.2f} tok/s, {request_count} 请求", flush=True)
PY
}

# ========================================
# 主测试流程
# ========================================

run_benchmark_once() {
    local result_dir=$1

    log_header "开始基准测试"

    CURRENT_RESULT_DIR="$result_dir"
    mkdir -p "$result_dir"
    mkdir -p "$LOG_DIR"

    log_info "结果目录: $result_dir"

    # 保存测试配置
    cat > "$result_dir/config.txt" << EOF
Benchmark Configuration
=======================
Date: $(date)
Remote IP: $REMOTE_IP
LLM Threads: $LLM_THREADS
LLM Batch Threads: $LLM_THREADS_BATCH
GOMP_SPINCOUNT: ${GOMP_SPINCOUNT:-default}
Parallel Slots: $LLM_PARALLEL
Context Length: $LLM_CONTEXT
Test Duration: ${TEST_DURATION}s
Benchmark Loops: $BENCHMARK_LOOPS
Concurrency Levels: $CONCURRENCY_LEVELS
Current Loop: ${CURRENT_LOOP_INDEX:-1}
Concurrent Tasks: $CONCURRENT_TASKS
Tokens per Request: $TOKENS_PER_REQUEST
Request Timeout: ${REQUEST_TIMEOUT_SEC}s
Fixed Total Tokens: ${FIXED_TOTAL_TOKENS:-1000}
Fixed Request Timeout: ${FIXED_REQUEST_TIMEOUT_SEC:-600}s
Request Temperature: ${REQUEST_TEMPERATURE:-0}
Request Seed: ${REQUEST_SEED:-1234}
Request Ignore EOS: ${REQUEST_IGNORE_EOS:-1}
Synchronized Concurrent Start: ${SYNC_CONCURRENT_START:-1}
Synchronized Start Lead: ${SYNC_START_LEAD_SEC:-3}s
Synchronized Start Spin: ${SYNC_START_SPIN_MS:-2}ms
Phase Gate Enabled: ${PHASE_GATE_ENABLED:-1}
Phase Gate Threshold: ${PHASE_GATE_THRESHOLD_MS:-100}ms
Phase Gate Max Retries: ${PHASE_GATE_MAX_RETRIES:-5}
Phase Gate Tokens: ${PHASE_GATE_TOKENS:-1}
Restart LLM: $RESTART_LLM
Benchmark Strategy: $BENCHMARK_STRATEGY
SCX Enabled: ${SCX_ENABLED:-0}
SCX Loader Strategy: $SCX_LOADER_STRATEGY
SCX Vector Slice ms: $SCX_VECTOR_SLICE_MS
SCX Vector Local Window ms: $SCX_VECTOR_LOCAL_WINDOW_MS
SCX Dirty Threshold: $SCX_DIRTY_THRESHOLD
SCX Lease Control Mode: $SCX_LEASE_CONTROL_MODE
SCX Lease Control Dir: $SCX_LEASE_CONTROL_DIR
SCX Lease User CPU Map: $SCX_LEASE_USER_CPU_MAP
Pre Benchmark Warmup: $PRE_BENCH_WARMUP
Pre Benchmark Warmup Requests: $PRE_BENCH_WARMUP_REQUESTS
Pre Benchmark Warmup Tokens: $PRE_BENCH_WARMUP_TOKENS
Pre Benchmark Warmup Retries: $PRE_BENCH_WARMUP_RETRIES
Pre Benchmark Warmup Timeout: $PRE_BENCH_WARMUP_TIMEOUT
Pre Benchmark Warmup Strict: $PRE_BENCH_WARMUP_STRICT
RVV Trace: $RVV_TRACE
RVV Trace Extra sec: $RVV_TRACE_EXTRA_SEC
RVV Trace Switch Out: $RVV_TRACE_SWITCH_OUT
RVV Trace Switch Reason: $RVV_TRACE_SWITCH_REASON
RVV Trace RVV Only: $RVV_TRACE_RVV_ONLY
RVV Trace Futex: $RVV_TRACE_FUTEX
RVV Trace Futex Detail: $RVV_TRACE_FUTEX_DETAIL
RVV Trace Futex Stack: $RVV_TRACE_FUTEX_STACK
Affinity Pin Workers: $AFFINITY_PIN_WORKERS
Affinity Worker CPUs: $AFFINITY_WORKER_CPUS
Affinity Worker Count: $AFFINITY_WORKER_COUNT
EOF

    # 记录系统信息
    log_info "收集系统信息..."
    ssh_exec "uname -a" > "$result_dir/system_info.txt"
    ssh_exec "cat /proc/cpuinfo | grep -E 'processor|model name|cpu MHz' | head -20" >> "$result_dir/system_info.txt"
    ssh_exec "free -h" >> "$result_dir/system_info.txt"
    ssh_exec "dmesg" > "$result_dir/dmesg_before.log"

    # 启动调度策略（CFS baseline 时为空操作）
    start_scheduler "$result_dir"

    # 正式计时前预热 llama-server 并发 slot，避免首个正式请求承担冷启动成本
    warmup_before_benchmark "$result_dir"

    # Linux 原生 affinity 对照：在正式测试前固定高 runtime worker TID 到指定 CPU
    pin_llm_workers_affinity "pre_benchmark" "$result_dir"

    # 只有首 token 相位通过门禁后，才开始正式计时和 RVV tracing。
    run_phase_gate "$result_dir"
    validate_user_controlled_lease_ready "$result_dir"

    # 启动调度观测，覆盖正式benchmark窗口和请求收尾阶段
    start_rvv_trace "$result_dir"

    # 启动并发任务
    log_header "启动 $CONCURRENT_TASKS 个并发任务"

    local pids=()
    local task_id
    local fixed_mode=0
    local fixed_total=${FIXED_TOTAL_TOKENS:-1000}
    local fixed_base=0
    local fixed_remainder=0
    if [ "$fixed_total" -gt 0 ]; then
        fixed_mode=1
        fixed_base=$((fixed_total / CONCURRENT_TASKS))
        fixed_remainder=$((fixed_total % CONCURRENT_TASKS))
        log_info "固定总token模式: total=$fixed_total, base_per_task=$fixed_base, remainder=$fixed_remainder"
    fi
    local sync_mono_start=""
    local sync_wall_start=""
    if [ "${SYNC_CONCURRENT_START:-1}" = "1" ]; then
        read -r sync_mono_start sync_wall_start < <(python3 - "${SYNC_START_LEAD_SEC:-3}" <<'PY'
import sys
import time

lead = float(sys.argv[1])
now_mono = time.monotonic()
now_wall = time.time()
print(f"{now_mono + lead:.9f} {now_wall + lead:.9f}")
PY
)
        log_info "并发首请求统一释放: monotonic=$sync_mono_start, wall=$sync_wall_start"
    else
        log_info "并发首请求对齐已关闭（SYNC_CONCURRENT_START=0）"
    fi

    for task_id in $(seq 1 "$CONCURRENT_TASKS"); do
        local prompt
        local task_tokens=$TOKENS_PER_REQUEST
        local task_timeout=${REQUEST_TIMEOUT_SEC:-180}
        prompt=$(get_task_prompt "$task_id")
        if [ "$fixed_mode" -eq 1 ]; then
            task_tokens=$fixed_base
            if [ "$task_id" -le "$fixed_remainder" ]; then
                task_tokens=$((task_tokens + 1))
            fi
            task_timeout=${FIXED_REQUEST_TIMEOUT_SEC:-600}
        fi
        (run_single_task "$task_id" "$prompt" "$result_dir/task${task_id}" \
            "$sync_mono_start" "$sync_wall_start" "$task_tokens" "$fixed_mode" "$task_timeout") &
        pids+=($!)
    done

    log_info "所有任务已启动，等待完成..."

    if [ "$fixed_mode" -eq 0 ]; then
        # 固定时长模式保留原有进度显示。
        local elapsed=0
        while [ $elapsed -lt $TEST_DURATION ]; do
            local step=10
            if [ $((TEST_DURATION - elapsed)) -lt $step ]; then
                step=$((TEST_DURATION - elapsed))
            fi
            sleep $step
            elapsed=$((elapsed + step))
            local percent=$((elapsed * 100 / TEST_DURATION))
            echo -ne "\r进度: [$elapsed/${TEST_DURATION}s] ${percent}% "
        done
        echo ""
    fi

    # 等待所有任务完成
    log_info "等待所有任务结束..."
    for pid in "${pids[@]}"; do
        wait $pid
    done

    log_info "✓ 所有任务已完成"

    if is_user_controlled_lease_scheduler; then
        if [ -z "$(remote_pid_by_pattern "$SCX_ACTIVE_PATTERN")" ]; then
            echo "status=failed" > "$result_dir/scheduler_alive_end.txt"
            log_error "$BENCHMARK_STRATEGY 在正式请求结束前退出"
            exit 1
        fi
        echo "status=alive" > "$result_dir/scheduler_alive_end.txt"
        ssh_exec "cat /sys/kernel/sched_ext/state 2>/dev/null || cat /sys/kernel/debug/sched/ext 2>/dev/null || true" >> "$result_dir/scheduler_alive_end.txt"
    fi

    # 停止调度观测并抓取JSON
    stop_rvv_trace "$result_dir"

    # 停止调度器并抓取统计日志
    stop_scheduler "$result_dir"

    ssh_exec "dmesg" > "$result_dir/dmesg_after.log"
    python3 - "$result_dir/dmesg_before.log" "$result_dir/dmesg_after.log" \
        > "$result_dir/dmesg_delta.log" <<'PY'
import sys
from pathlib import Path

before = Path(sys.argv[1]).read_text(errors="replace").splitlines(keepends=True)
after = Path(sys.argv[2]).read_text(errors="replace").splitlines(keepends=True)

if after[:len(before)] == before:
    sys.stdout.writelines(after[len(before):])
else:
    last_before = before[-1] if before else None
    if last_before in after:
        index = len(after) - 1 - after[::-1].index(last_before)
        sys.stdout.writelines(after[index + 1:])
    else:
        sys.stdout.write("# Unable to align dmesg snapshots; full after snapshot follows.\n")
        sys.stdout.writelines(after)
PY

    # 生成汇总报告
    generate_report "$result_dir"

    log_header "测试完成"
    log_info "结果保存在: $result_dir"
    log_info "查看报告: cat $result_dir/report.txt"
}

# ========================================
# 报告生成
# ========================================

write_result_metrics() {
    local result_dir=$1
    local loop_index=${CURRENT_LOOP_INDEX:-1}

    python3 - "$result_dir" "$BENCHMARK_STRATEGY" "$CONCURRENT_TASKS" "$loop_index" > "$result_dir/metrics.env" <<'PY'
import csv
import math
import sys
from pathlib import Path

result_dir = Path(sys.argv[1])
strategy = sys.argv[2]
concurrency = int(sys.argv[3])
loop_index = int(sys.argv[4])

total_tokens = 0
total_requests = 0
ttfts = []
durations = []
start_timestamps = []
first_token_timestamps = []
last_token_timestamps = []

for csv_file in sorted(result_dir.glob("task*.csv")):
    with csv_file.open(newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                total_tokens += int(float(row.get("tokens") or 0))
                total_requests += 1
            except ValueError:
                pass
            raw_ttft = row.get("ttft_ms")
            if raw_ttft not in (None, "", "null"):
                try:
                    ttfts.append(float(raw_ttft))
                except ValueError:
                    pass

for summary_file in sorted(result_dir.glob("task*.summary")):
    for line in summary_file.read_text(errors="ignore").splitlines():
        if line.startswith("Duration:"):
            raw = line.split(":", 1)[1].strip().rstrip("s")
            try:
                durations.append(float(raw))
            except ValueError:
                pass
        elif line.startswith("Start Timestamp:"):
            raw = line.split(":", 1)[1].strip()
            try:
                start_timestamps.append(float(raw))
            except ValueError:
                pass
        elif line.startswith("Last Token Timestamp:"):
            raw = line.split(":", 1)[1].strip()
            if raw:
                try:
                    last_token_timestamps.append(float(raw))
                except ValueError:
                    pass
        elif line.startswith("First Token Timestamp:"):
            raw = line.split(":", 1)[1].strip()
            if raw:
                try:
                    first_token_timestamps.append(float(raw))
                except ValueError:
                    pass

if start_timestamps and last_token_timestamps:
    duration = max(last_token_timestamps) - min(start_timestamps)
else:
    duration = max(durations) if durations else 0.0
tps = (total_tokens / duration) if duration > 0 else 0.0
if first_token_timestamps and last_token_timestamps:
    decode_duration = max(last_token_timestamps) - min(first_token_timestamps)
else:
    decode_duration = 0.0
decode_tokens = max(0, total_tokens - len(first_token_timestamps))
decode_tps = decode_tokens / decode_duration if decode_duration > 0 else 0.0
avg_ttft = (sum(ttfts) / len(ttfts)) if ttfts else 0.0
if ttfts:
    ordered = sorted(ttfts)
    idx = max(0, math.ceil(0.95 * len(ordered)) - 1)
    p95_ttft = ordered[idx]
else:
    p95_ttft = 0.0

print(f"strategy={strategy}")
print(f"concurrency={concurrency}")
print(f"loop={loop_index}")
print(f"duration_sec={duration:.3f}")
print(f"total_requests={total_requests}")
print(f"total_tokens={total_tokens}")
print(f"tps={tps:.3f}")
print(f"decode_duration_sec={decode_duration:.3f}")
print(f"decode_tokens={decode_tokens}")
print(f"decode_tps={decode_tps:.3f}")
print(f"avg_ttft_ms={avg_ttft:.3f}")
print(f"p95_ttft_ms={p95_ttft:.3f}")
print(f"ttft_samples={len(ttfts)}")
PY
}

generate_report() {
    local result_dir=$1
    local report_file="$result_dir/report.txt"

    log_info "生成测试报告..."
    write_result_metrics "$result_dir"

    {
        echo "========================================"
        echo "RVV LLM Benchmark Report"
        echo "========================================"
        echo ""
        echo "测试时间: $(date)"
        echo "配置窗口: ${TEST_DURATION}s"
        echo "并发任务: $CONCURRENT_TASKS"
        echo "策略: $BENCHMARK_STRATEGY"
        echo "Fixed Total Tokens: ${FIXED_TOTAL_TOKENS:-1000}"
        echo "Fixed Request Timeout: ${FIXED_REQUEST_TIMEOUT_SEC:-600}s"
        echo "Request Temperature: ${REQUEST_TEMPERATURE:-0}"
        echo "Request Seed: ${REQUEST_SEED:-1234}"
        echo "Request Ignore EOS: ${REQUEST_IGNORE_EOS:-1}"
        echo "SCX加载策略: $SCX_LOADER_STRATEGY"
        echo "SCX Vector Slice: ${SCX_VECTOR_SLICE_MS}ms"
        echo "SCX Vector Local Window: ${SCX_VECTOR_LOCAL_WINDOW_MS}ms"
        echo "SCX Dirty Threshold: $SCX_DIRTY_THRESHOLD"
        echo "SCX Lease Control Mode: $SCX_LEASE_CONTROL_MODE"
        echo "SCX Lease Control Dir: $SCX_LEASE_CONTROL_DIR"
        echo "SCX Lease User CPU Map: $SCX_LEASE_USER_CPU_MAP"
        echo "Pre Benchmark Warmup: $PRE_BENCH_WARMUP"
        echo "Pre Benchmark Warmup Requests: $PRE_BENCH_WARMUP_REQUESTS"
        echo "Pre Benchmark Warmup Tokens: $PRE_BENCH_WARMUP_TOKENS"
        echo "Pre Benchmark Warmup Retries: $PRE_BENCH_WARMUP_RETRIES"
        echo "Pre Benchmark Warmup Timeout: $PRE_BENCH_WARMUP_TIMEOUT"
        echo "Pre Benchmark Warmup Strict: $PRE_BENCH_WARMUP_STRICT"
        echo "Synchronized Concurrent Start: ${SYNC_CONCURRENT_START:-1}"
        echo "Synchronized Start Lead: ${SYNC_START_LEAD_SEC:-3}s"
        echo "Synchronized Start Spin: ${SYNC_START_SPIN_MS:-2}ms"
        echo "Phase Gate Enabled: ${PHASE_GATE_ENABLED:-1}"
        echo "Phase Gate Threshold: ${PHASE_GATE_THRESHOLD_MS:-100}ms"
        echo "Phase Gate Max Retries: ${PHASE_GATE_MAX_RETRIES:-5}"
        echo "Phase Gate Tokens: ${PHASE_GATE_TOKENS:-1}"
        echo "RVV Trace: $RVV_TRACE"
        echo "RVV Trace Switch Out: $RVV_TRACE_SWITCH_OUT"
        echo "RVV Trace Switch Reason: $RVV_TRACE_SWITCH_REASON"
        echo "RVV Trace RVV Only: $RVV_TRACE_RVV_ONLY"
        echo "RVV Trace Futex: $RVV_TRACE_FUTEX"
        echo "RVV Trace Futex Detail: $RVV_TRACE_FUTEX_DETAIL"
        echo "RVV Trace Futex Stack: $RVV_TRACE_FUTEX_STACK"
        echo ""

        if [ -f "$result_dir/pre_bench_warmup/status.txt" ]; then
            echo "========================================"
            echo "正式测试前warm-up"
            echo "========================================"
            echo ""
            cat "$result_dir/pre_bench_warmup/status.txt"
            echo ""
        fi
        echo "========================================"
        echo "各任务性能"
        echo "========================================"
        echo ""

        for i in $(seq 1 "$CONCURRENT_TASKS"); do
            if [ -f "$result_dir/task${i}.summary" ]; then
                echo "--- Task $i ---"
                cat "$result_dir/task${i}.summary"
                echo ""
            fi
        done

        echo "========================================"
        echo "汇总统计"
        echo "========================================"
        echo ""

        if [ -f "$result_dir/metrics.env" ]; then
            # shellcheck disable=SC1091
            source "$result_dir/metrics.env"
            echo "TPS: ${tps} tok/s"
            echo "Decode TPS: ${decode_tps} tok/s"
            echo "总请求数: $total_requests"
            echo "总Token数: $total_tokens"
            echo "统计时长: ${duration_sec}s"
            echo "Decode Token数: $decode_tokens"
            echo "Decode统计时长: ${decode_duration_sec}s"
            echo "平均TTFT: ${avg_ttft_ms}ms"
            echo "p95TTFT: ${p95_ttft_ms}ms"
            echo "TTFT样本数: $ttft_samples"
        else
            echo "TPS: N/A"
            echo "平均TTFT: N/A"
            echo "p95TTFT: N/A"
        fi
        echo ""

        if [ -f "$result_dir/scheduler.log" ]; then
            echo "========================================"
            echo "sched_ext调度器统计"
            echo "========================================"
            echo ""
            tail -40 "$result_dir/scheduler.log"
            echo ""
        fi

        if [ -f "$result_dir/rvv_sched_cost.json" ]; then
            echo "========================================"
            echo "rvv_sched_cost观测"
            echo "========================================"
            echo ""
            echo "JSON: rvv_sched_cost.json"
            echo "Log: rvv_sched_cost.log"
            echo ""
        fi

        echo "========================================"
        echo "详细数据文件"
        echo "========================================"
        echo ""
        for i in $(seq 1 "$CONCURRENT_TASKS"); do
            echo "Task $i CSV: task${i}.csv"
        done

    } > "$report_file"

    # 输出到终端
    cat "$report_file"
}

generate_suite_report() {
    local suite_dir=$1

    log_info "生成完整测试汇总..."
    python3 - "$suite_dir" > "$suite_dir/summary.txt" <<'PY'
import csv
import math
import sys
from pathlib import Path

suite_dir = Path(sys.argv[1])

def read_env(path):
    data = {}
    for line in path.read_text(errors="ignore").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            data[key] = value
    return data

def p95(values):
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = max(0, math.ceil(0.95 * len(ordered)) - 1)
    return ordered[idx]

def mean(values):
    return sum(values) / len(values) if values else 0.0

def variance(values):
    if len(values) < 2:
        return 0.0
    average = mean(values)
    return sum((value - average) ** 2 for value in values) / (len(values) - 1)

def sd(values):
    return math.sqrt(variance(values))

def read_ttfts(run_dir):
    values = []
    for csv_file in sorted(run_dir.glob("task*.csv")):
        with csv_file.open(newline="") as f:
            reader = csv.DictReader(f)
            for row in reader:
                raw = row.get("ttft_ms")
                if raw not in (None, "", "null"):
                    try:
                        values.append(float(raw))
                    except ValueError:
                        pass
    return values

runs = []
for metrics_file in sorted(suite_dir.glob("*/metrics.env")):
    run_dir = metrics_file.parent
    data = read_env(metrics_file)
    data["result_dir"] = run_dir.name
    data["ttfts"] = read_ttfts(run_dir)
    runs.append(data)

summary_csv = suite_dir / "summary.csv"
with summary_csv.open("w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow([
        "concurrency", "loop", "tps", "decode_tps", "avg_ttft_ms", "p95_ttft_ms",
        "total_requests", "total_tokens", "duration_sec", "result_dir"
    ])
    for run in runs:
        writer.writerow([
            run.get("concurrency", ""),
            run.get("loop", ""),
            run.get("tps", ""),
            run.get("decode_tps", ""),
            run.get("avg_ttft_ms", ""),
            run.get("p95_ttft_ms", ""),
            run.get("total_requests", ""),
            run.get("total_tokens", ""),
            run.get("duration_sec", ""),
            run.get("result_dir", ""),
        ])

groups = {}
for run in runs:
    groups.setdefault(run.get("concurrency", "unknown"), []).append(run)

print("========================================")
print("RVV LLM Benchmark Suite Summary")
print("========================================")
print("")
print(f"Suite: {suite_dir}")
print(f"Runs: {len(runs)}")
print(f"CSV: summary.csv")
print("")
print("Per-concurrency aggregate:")
print("")
print("Concurrency | Runs | E2E TPS(mean+/-sd) | Decode TPS(mean+/-sd) | TTFT(mean+/-sd ms) | p95TTFT(mean+/-sd ms) | Requests")
print("--- | ---: | ---: | ---: | ---: | ---: | ---:")

for concurrency in sorted(groups, key=lambda x: int(x) if x.isdigit() else x):
    items = groups[concurrency]
    tps_values = [float(x.get("tps", 0) or 0) for x in items]
    decode_tps_values = [float(x.get("decode_tps", 0) or 0) for x in items]
    total_tokens = sum(int(float(x.get("total_tokens", 0) or 0)) for x in items)
    total_duration = sum(float(x.get("duration_sec", 0) or 0) for x in items)
    total_requests = sum(int(float(x.get("total_requests", 0) or 0)) for x in items)
    all_ttfts = []
    for item in items:
        all_ttfts.extend(item.get("ttfts", []))
    avg_tps = mean(tps_values)
    run_avg_ttfts = [float(x.get("avg_ttft_ms", 0) or 0) for x in items]
    run_p95_ttfts = [float(x.get("p95_ttft_ms", 0) or 0) for x in items]
    avg_ttft = mean(all_ttfts)
    pooled_p95 = p95(all_ttfts)
    print(
        f"{concurrency} | {len(items)} | {avg_tps:.3f}+/-{sd(tps_values):.3f} | "
        f"{mean(decode_tps_values):.3f}+/-{sd(decode_tps_values):.3f} | "
        f"{mean(run_avg_ttfts):.3f}+/-{sd(run_avg_ttfts):.3f} | "
        f"{mean(run_p95_ttfts):.3f}+/-{sd(run_p95_ttfts):.3f} | {total_requests}"
    )
    print(
        f"  variance: TPS={variance(tps_values):.6f}, "
        f"decode_TPS={variance(decode_tps_values):.6f}, "
        f"avg_ttft_ms={variance(run_avg_ttfts):.3f}, "
        f"p95_ttft_ms={variance(run_p95_ttfts):.3f}; pooled_p95TTFT={pooled_p95:.3f}ms"
    )

print("")
print("Per-run details:")
print("")
for run in runs:
    print(
        f"- c={run.get('concurrency')} loop={run.get('loop')}: "
        f"TPS={float(run.get('tps', 0) or 0):.3f}, "
        f"decode_TPS={float(run.get('decode_tps', 0) or 0):.3f}, "
        f"TTFT={float(run.get('avg_ttft_ms', 0) or 0):.3f}ms, "
        f"p95TTFT={float(run.get('p95_ttft_ms', 0) or 0):.3f}ms, "
        f"requests={run.get('total_requests')}, dir={run.get('result_dir')}"
    )
PY

    cat "$suite_dir/summary.txt"
}

run_benchmark_suite() {
    log_header "开始完整测试套件"

    local timestamp
    local strategy_slug
    local suite_dir
    timestamp=$(date +%Y%m%d_%H%M%S)
    strategy_slug=$(printf '%s' "$BENCHMARK_STRATEGY" | tr -c 'A-Za-z0-9_.-' '_')
    if [ -n "${GOMP_SPINCOUNT:-}" ]; then
        strategy_slug="${strategy_slug}_gomp${GOMP_SPINCOUNT}"
    fi
    if [ "${FIXED_TOTAL_TOKENS:-1000}" -gt 0 ]; then
        strategy_slug="${strategy_slug}_fixed${FIXED_TOTAL_TOKENS}"
    fi
    suite_dir="$RESULTS_DIR/${timestamp}_${strategy_slug}_suite"
    mkdir -p "$suite_dir" "$LOG_DIR"

    cat > "$suite_dir/suite_config.txt" << EOF
Benchmark Suite Configuration
=============================
Date: $(date)
Strategy: $BENCHMARK_STRATEGY
Configured duration per run: ${TEST_DURATION}s
Benchmark loops: $BENCHMARK_LOOPS
Concurrency levels: $CONCURRENCY_LEVELS
Tokens per request: $TOKENS_PER_REQUEST
Request timeout: ${REQUEST_TIMEOUT_SEC}s
Fixed total tokens: ${FIXED_TOTAL_TOKENS:-1000}
Fixed request timeout: ${FIXED_REQUEST_TIMEOUT_SEC:-600}s
Request temperature: ${REQUEST_TEMPERATURE:-0}
Request seed: ${REQUEST_SEED:-1234}
Request ignore EOS: ${REQUEST_IGNORE_EOS:-1}
LLM threads (-t): $LLM_THREADS
LLM batch threads (-tb): $LLM_THREADS_BATCH
GOMP_SPINCOUNT: ${GOMP_SPINCOUNT:-default}
RVV Trace: $RVV_TRACE
RVV Trace Switch Out: $RVV_TRACE_SWITCH_OUT
RVV Trace Switch Reason: $RVV_TRACE_SWITCH_REASON
RVV Trace RVV Only: $RVV_TRACE_RVV_ONLY
RVV Trace Futex: $RVV_TRACE_FUTEX
RVV Trace Futex Detail: $RVV_TRACE_FUTEX_DETAIL
RVV Trace Futex Stack: $RVV_TRACE_FUTEX_STACK
Affinity Pin Workers: $AFFINITY_PIN_WORKERS
Affinity Worker CPUs: $AFFINITY_WORKER_CPUS
Affinity Worker Count: $AFFINITY_WORKER_COUNT
SCX Lease Control Mode: $SCX_LEASE_CONTROL_MODE
SCX Lease Control Dir: $SCX_LEASE_CONTROL_DIR
SCX Lease User CPU Map: $SCX_LEASE_USER_CPU_MAP
EOF

    log_info "套件目录: $suite_dir"
    if [ "${FIXED_TOTAL_TOKENS:-1000}" -gt 0 ]; then
        log_info "并发档位: $CONCURRENCY_LEVELS; 每档轮数: $BENCHMARK_LOOPS; 固定总token: ${FIXED_TOTAL_TOKENS}"
    else
        log_info "并发档位: $CONCURRENCY_LEVELS; 每档轮数: $BENCHMARK_LOOPS; 单轮窗口: ${TEST_DURATION}s"
    fi

    local old_ifs=$IFS
    local levels=()
    IFS=',' read -r -a levels <<< "$CONCURRENCY_LEVELS"
    IFS=$old_ifs

    local concurrency
    local loop
    for concurrency in "${levels[@]}"; do
        CONCURRENT_TASKS="$concurrency"
        LLM_PARALLEL="$concurrency"
        if [ "${PRE_BENCH_WARMUP_REQUESTS_AUTO:-0}" = "1" ]; then
            PRE_BENCH_WARMUP_REQUESTS="$CONCURRENT_TASKS"
        fi

        for loop in $(seq 1 "$BENCHMARK_LOOPS"); do
            CURRENT_LOOP_INDEX="$loop"
            local run_dir="$suite_dir/c${concurrency}_loop${loop}"

            log_header "并发 $concurrency / 轮次 $loop/$BENCHMARK_LOOPS"
            start_llm_server
            run_benchmark_once "$run_dir"
        done
    done

    generate_suite_report "$suite_dir"
    ln -sfn "$suite_dir" "$RESULTS_DIR/latest"

    log_header "测试套件完成"
    log_info "汇总报告: cat $suite_dir/summary.txt"
    log_info "汇总CSV: $suite_dir/summary.csv"
}

# ========================================
# 清理函数
# ========================================

cleanup() {
    local exit_status=$?
    local restore_status=0
    set +e
    log_info "清理环境..."
    stop_rvv_trace "$CURRENT_RESULT_DIR"
    stop_scheduler "$CURRENT_RESULT_DIR"
    # 停止所有后台任务
    jobs -p | xargs -r kill 2>/dev/null || true
    restore_lease_hint_runtime || restore_status=$?
    if [ "$exit_status" -eq 0 ] && [ "$restore_status" -ne 0 ]; then
        exit_status=$restore_status
    fi
    return "$exit_status"
}

trap cleanup EXIT

# ========================================
# 主函数
# ========================================

main() {
    log_header "RVV LLM Benchmark"

    # 解析命令行参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            --duration)
                TEST_DURATION="$2"
                shift 2
                ;;
            --tasks)
                CONCURRENT_TASKS="$2"
                CONCURRENCY_LEVELS="$2"
                shift 2
                ;;
            --concurrency-levels)
                CONCURRENCY_LEVELS="$2"
                shift 2
                ;;
            --loops)
                BENCHMARK_LOOPS="$2"
                shift 2
                ;;
            --tokens)
                TOKENS_PER_REQUEST="$2"
                shift 2
                ;;
            --total-tokens)
                FIXED_TOTAL_TOKENS="$2"
                shift 2
                ;;
            --warmup-requests)
                PRE_BENCH_WARMUP_REQUESTS="$2"
                PRE_BENCH_WARMUP_REQUESTS_AUTO=0
                shift 2
                ;;
            --warmup-tokens)
                PRE_BENCH_WARMUP_TOKENS="$2"
                shift 2
                ;;
            --warmup-retries)
                PRE_BENCH_WARMUP_RETRIES="$2"
                shift 2
                ;;
            --no-warmup)
                PRE_BENCH_WARMUP=0
                shift
                ;;
            --threads)
                LLM_THREADS="$2"
                shift 2
                ;;
            --threads-batch)
                LLM_THREADS_BATCH="$2"
                shift 2
                ;;
            --gomp-spincount)
                GOMP_SPINCOUNT="$2"
                shift 2
                ;;
            --strategy)
                BENCHMARK_STRATEGY="$2"
                shift 2
                ;;
            --vector-slice-ms)
                SCX_VECTOR_SLICE_OVERRIDE_MS="$2"
                shift 2
                ;;
            --vector-local-window-ms)
                SCX_VECTOR_LOCAL_WINDOW_OVERRIDE_MS="$2"
                shift 2
                ;;
            --dirty-threshold)
                SCX_DIRTY_THRESHOLD_OVERRIDE="$2"
                shift 2
                ;;
            --list-strategies)
                list_strategies
                exit 0
                ;;
            --help)
                echo "用法: $0 [选项]"
                echo ""
                echo "选项:"
                echo "  --duration N   固定时长模式窗口（秒，--total-tokens 0 时使用）"
                echo "  --tasks N      只测试单个并发数（兼容旧参数）"
                echo "  --concurrency-levels L  并发档位列表（默认2,4,8）"
                echo "  --loops N      每个并发档位重复轮数（默认1）"
                echo "  --tokens N     每个请求生成token数（默认100）"
                echo "  --total-tokens N  固定总输出token数（默认1000；0恢复固定时长模式）"
                echo "  --threads N    单 token/generation 计算线程数 -t（默认8）"
                echo "  --threads-batch N  多 token/prompt batch 线程数 -tb（默认跟随 --threads）"
                echo "  --gomp-spincount N  GNU libgomp barrier 自旋次数（默认使用板端默认值）"
                echo "  --strategy S   测试策略（默认cfs，使用 --list-strategies 查看）"
                echo "  --vector-slice-ms N  覆盖 RVV dirty vector 时间片（毫秒）"
                echo "  --vector-local-window-ms N  覆盖 RVV local fast-path 窗口（毫秒）"
                echo "  --dirty-threshold N  覆盖 RVV dirty 命中阈值"
                echo "  --warmup-requests N  正式测试前预热请求数（默认跟随当前并发档位）"
                echo "  --warmup-tokens N    每个预热请求生成token数（默认1）"
                echo "  --warmup-retries N   预热失败重试次数（默认3）"
                echo "  --no-warmup          关闭正式测试前预热"
                echo "  --list-strategies  列出可选策略"
                echo "  --help         显示此帮助"
                exit 0
                ;;
            *)
                log_error "未知选项: $1"
                exit 1
                ;;
        esac
    done

    if [ -z "$LLM_THREADS_BATCH" ]; then
        LLM_THREADS_BATCH="$LLM_THREADS"
    fi

    # 创建输出目录
    mkdir -p "$RESULTS_DIR" "$LOG_DIR"

    if [ "${PRE_BENCH_WARMUP_REQUESTS_AUTO:-0}" = "1" ]; then
        PRE_BENCH_WARMUP_REQUESTS="$CONCURRENT_TASKS"
    fi

    require_positive_int "TEST_DURATION" "$TEST_DURATION"
    require_positive_int "CONCURRENT_TASKS" "$CONCURRENT_TASKS"
    require_positive_int "BENCHMARK_LOOPS" "$BENCHMARK_LOOPS"
    require_positive_int "TOKENS_PER_REQUEST" "$TOKENS_PER_REQUEST"
    require_positive_int "REQUEST_TIMEOUT_SEC" "$REQUEST_TIMEOUT_SEC"
    if ! [[ "${FIXED_TOTAL_TOKENS:-1000}" =~ ^[0-9]+$ ]]; then
        log_error "FIXED_TOTAL_TOKENS 必须是非负整数"
        exit 1
    fi
    require_positive_int "FIXED_REQUEST_TIMEOUT_SEC" "${FIXED_REQUEST_TIMEOUT_SEC:-600}"
    if [ "${FIXED_TOTAL_TOKENS:-1000}" -gt 0 ] && [ "${FIXED_TOTAL_TOKENS:-1000}" -lt "$CONCURRENT_TASKS" ]; then
        log_error "FIXED_TOTAL_TOKENS 必须不小于并发任务数"
        exit 1
    fi
    if ! [[ "${REQUEST_TEMPERATURE:-0}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        log_error "REQUEST_TEMPERATURE 必须是非负数字"
        exit 1
    fi
    if ! [[ "${REQUEST_SEED:-1234}" =~ ^[0-9]+$ ]]; then
        log_error "REQUEST_SEED 必须是非负整数"
        exit 1
    fi
    if ! [[ "${REQUEST_IGNORE_EOS:-1}" =~ ^[01]$ ]]; then
        log_error "REQUEST_IGNORE_EOS 必须是 0 或 1"
        exit 1
    fi
    require_positive_int "LLM_THREADS" "$LLM_THREADS"
    require_positive_int "LLM_THREADS_BATCH" "$LLM_THREADS_BATCH"
    if [ -n "${GOMP_SPINCOUNT:-}" ]; then
        require_positive_int "GOMP_SPINCOUNT" "$GOMP_SPINCOUNT"
    fi
    require_positive_int "PRE_BENCH_WARMUP_REQUESTS" "$PRE_BENCH_WARMUP_REQUESTS"
    require_positive_int "PRE_BENCH_WARMUP_TOKENS" "$PRE_BENCH_WARMUP_TOKENS"
    require_positive_int "PRE_BENCH_WARMUP_RETRIES" "$PRE_BENCH_WARMUP_RETRIES"
    require_positive_int "PRE_BENCH_WARMUP_TIMEOUT" "$PRE_BENCH_WARMUP_TIMEOUT"
    require_concurrency_list "CONCURRENCY_LEVELS" "$CONCURRENCY_LEVELS"
    if ! [[ "${SYNC_CONCURRENT_START:-1}" =~ ^[01]$ ]]; then
        log_error "SYNC_CONCURRENT_START 必须是 0 或 1"
        exit 1
    fi
    if ! [[ "${SYNC_START_LEAD_SEC:-3}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        log_error "SYNC_START_LEAD_SEC 必须是非负数字"
        exit 1
    fi
    if ! [[ "${SYNC_START_SPIN_MS:-2}" =~ ^[0-9]+$ ]]; then
        log_error "SYNC_START_SPIN_MS 必须是非负整数"
        exit 1
    fi
    if ! [[ "${PHASE_GATE_ENABLED:-1}" =~ ^[01]$ ]]; then
        log_error "PHASE_GATE_ENABLED 必须是 0 或 1"
        exit 1
    fi
    if ! [[ "${PHASE_GATE_THRESHOLD_MS:-100}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        log_error "PHASE_GATE_THRESHOLD_MS 必须是非负数字"
        exit 1
    fi
    require_positive_int "PHASE_GATE_MAX_RETRIES" "${PHASE_GATE_MAX_RETRIES:-5}"
    require_positive_int "PHASE_GATE_TOKENS" "${PHASE_GATE_TOKENS:-1}"
    require_positive_int "PHASE_GATE_TIMEOUT_SEC" "${PHASE_GATE_TIMEOUT_SEC:-60}"
    apply_strategy_preset
    if [ -n "$SCX_VECTOR_SLICE_OVERRIDE_MS" ]; then
        if ! [[ "$SCX_VECTOR_SLICE_OVERRIDE_MS" =~ ^[0-9]+$ ]]; then
            log_error "SCX_VECTOR_SLICE_OVERRIDE_MS 必须是非负整数，当前值: $SCX_VECTOR_SLICE_OVERRIDE_MS"
            exit 1
        fi
        SCX_VECTOR_SLICE_MS="$SCX_VECTOR_SLICE_OVERRIDE_MS"
    fi
    if [ -n "$SCX_VECTOR_LOCAL_WINDOW_OVERRIDE_MS" ]; then
        if ! [[ "$SCX_VECTOR_LOCAL_WINDOW_OVERRIDE_MS" =~ ^[0-9]+$ ]]; then
            log_error "SCX_VECTOR_LOCAL_WINDOW_OVERRIDE_MS 必须是非负整数，当前值: $SCX_VECTOR_LOCAL_WINDOW_OVERRIDE_MS"
            exit 1
        fi
        SCX_VECTOR_LOCAL_WINDOW_MS="$SCX_VECTOR_LOCAL_WINDOW_OVERRIDE_MS"
    fi
    if [ -n "$SCX_DIRTY_THRESHOLD_OVERRIDE" ]; then
        if ! [[ "$SCX_DIRTY_THRESHOLD_OVERRIDE" =~ ^[0-9]+$ ]] || [ "$SCX_DIRTY_THRESHOLD_OVERRIDE" -eq 0 ]; then
            log_error "SCX_DIRTY_THRESHOLD_OVERRIDE 必须是正整数，当前值: $SCX_DIRTY_THRESHOLD_OVERRIDE"
            exit 1
        fi
        SCX_DIRTY_THRESHOLD="$SCX_DIRTY_THRESHOLD_OVERRIDE"
    fi

    # 执行测试流程
    check_requirements
    prepare_lease_hint_runtime
    run_benchmark_suite

    log_header "全部完成！"
}

# 运行主函数
main "$@"
