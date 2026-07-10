#!/bin/bash
# ========================================
# RVV LLM Benchmark - 主测试脚本
# ========================================
# 功能：
# - 启动远程llama-server
# - 并发4个任务测试5分钟
# - 记录首字延迟（TTFT）
# - 记录每个任务的tok/s
# - 生成详细的测试报告
# ========================================

set -e

# 加载配置
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

SCX_VECTOR_SLICE_OVERRIDE_MS=""

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

RESULTS_DIR=$(resolve_local_path "$RESULTS_DIR")
LOG_DIR=$(resolve_local_path "$LOG_DIR")
SCX_LOCAL_BIN=$(resolve_local_path "$SCX_LOCAL_BIN")
SCX_EXCLUSIVE_LOCAL_BIN=$(resolve_local_path "$SCX_EXCLUSIVE_LOCAL_BIN")
RVV_LOCAL_BIN=$(resolve_local_path "$RVV_LOCAL_BIN")
RVV_LOCAL_LIBELF=$(resolve_local_path "$RVV_LOCAL_LIBELF")

is_rvv_exclusive_scheduler() {
    [ "${SCX_LOADER_STRATEGY:-}" = "rvv_exclusive" ]
}

apply_strategy_preset() {
    SCX_ENABLED=1
    SCX_WORKER_ONLY="0"

    case "$BENCHMARK_STRATEGY" in
        cfs|baseline)
            BENCHMARK_STRATEGY="cfs"
            SCX_ENABLED=0
            SCX_SWITCH_ALL="1"
            ;;
        scx_global)
            SCX_LOADER_STRATEGY="global"
            SCX_LLM_CPUS="0-7"
            SCX_SYSTEM_CPUS="0-7"
            SCX_SLICE_MS="0"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_SWITCH_ALL="1"
            ;;
        pin_all)
            SCX_LOADER_STRATEGY="pin"
            SCX_LLM_CPUS="0-7"
            SCX_SYSTEM_CPUS="0-7"
            SCX_SLICE_MS="0"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_SWITCH_ALL="1"
            ;;
        pin_0_3_iso_4_7)
            SCX_LOADER_STRATEGY="pin"
            SCX_LLM_CPUS="0-3"
            SCX_SYSTEM_CPUS="4-7"
            SCX_SLICE_MS="0"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_SWITCH_ALL="1"
            ;;
        pin_4_7_iso_0_3)
            SCX_LOADER_STRATEGY="pin"
            SCX_LLM_CPUS="4-7"
            SCX_SYSTEM_CPUS="0-3"
            SCX_SLICE_MS="0"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_SWITCH_ALL="1"
            ;;
        long_slice)
            SCX_LOADER_STRATEGY="long_slice"
            SCX_LLM_CPUS="0-7"
            SCX_SYSTEM_CPUS="0-7"
            SCX_SLICE_MS="100"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_SWITCH_ALL="1"
            ;;
        local_dsq)
            SCX_LOADER_STRATEGY="local_dsq"
            SCX_LLM_CPUS="0-7"
            SCX_SYSTEM_CPUS="0-7"
            SCX_SLICE_MS="0"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_SWITCH_ALL="1"
            ;;
        local_dsq_50ms)
            SCX_LOADER_STRATEGY="local_dsq"
            SCX_LLM_CPUS="0-7"
            SCX_SYSTEM_CPUS="0-7"
            SCX_SLICE_MS="50"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_SWITCH_ALL="1"
            ;;
        local_dsq_100ms)
            SCX_LOADER_STRATEGY="local_dsq"
            SCX_LLM_CPUS="0-7"
            SCX_SYSTEM_CPUS="0-7"
            SCX_SLICE_MS="100"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_SWITCH_ALL="1"
            ;;
        long_slice_isolated)
            SCX_LOADER_STRATEGY="long_slice"
            SCX_LLM_CPUS="0-3"
            SCX_SYSTEM_CPUS="4-7"
            SCX_SLICE_MS="100"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_SWITCH_ALL="1"
            ;;
        llm_only_pin)
            SCX_LOADER_STRATEGY="pin"
            SCX_LLM_CPUS="0-7"
            SCX_SYSTEM_CPUS="0-7"
            SCX_SLICE_MS="0"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_SWITCH_ALL="0"
            SCX_WORKER_ONLY="0"
            ;;
        llm_only_local_dsq)
            SCX_LOADER_STRATEGY="local_dsq"
            SCX_LLM_CPUS="0-7"
            SCX_SYSTEM_CPUS="0-7"
            SCX_SLICE_MS="0"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_SWITCH_ALL="0"
            SCX_WORKER_ONLY="0"
            ;;
        llm_only_local_dsq_50ms)
            SCX_LOADER_STRATEGY="local_dsq"
            SCX_LLM_CPUS="0-7"
            SCX_SYSTEM_CPUS="0-7"
            SCX_SLICE_MS="50"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_SWITCH_ALL="0"
            SCX_WORKER_ONLY="0"
            ;;
        llm_workers_pin)
            SCX_LOADER_STRATEGY="pin"
            SCX_LLM_CPUS="0-7"
            SCX_SYSTEM_CPUS="0-7"
            SCX_SLICE_MS="0"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_SWITCH_ALL="0"
            SCX_WORKER_ONLY="1"
            ;;
        llm_workers_local_dsq)
            SCX_LOADER_STRATEGY="local_dsq"
            SCX_LLM_CPUS="0-7"
            SCX_SYSTEM_CPUS="0-7"
            SCX_SLICE_MS="0"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_SWITCH_ALL="0"
            SCX_WORKER_ONLY="1"
            ;;
        rvv_dirty_all)
            SCX_LOADER_STRATEGY="rvv_dirty"
            SCX_LLM_CPUS="0-7"
            SCX_SYSTEM_CPUS="0-7"
            SCX_VECTOR_CPUS="0-7"
            SCX_ORDINARY_CPUS="0-7"
            SCX_SLICE_MS="0"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_VECTOR_SLICE_MS="100"
            SCX_ORDINARY_VECTOR_SLICE_MS="50"
            SCX_DIRTY_THRESHOLD="2"
            SCX_FALLBACK_EVERY="0"
            SCX_SWITCH_ALL="1"
            SCX_WORKER_ONLY="0"
            ;;
        rvv_dirty_local_all)
            SCX_LOADER_STRATEGY="rvv_dirty_local"
            SCX_LLM_CPUS="0-7"
            SCX_SYSTEM_CPUS="0-7"
            SCX_VECTOR_CPUS="0-7"
            SCX_ORDINARY_CPUS="0-7"
            SCX_SLICE_MS="0"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_VECTOR_SLICE_MS="100"
            SCX_ORDINARY_VECTOR_SLICE_MS="50"
            SCX_DIRTY_THRESHOLD="2"
            SCX_FALLBACK_EVERY="0"
            SCX_SWITCH_ALL="1"
            SCX_WORKER_ONLY="0"
            ;;
        rvv_dirty_6_2)
            SCX_LOADER_STRATEGY="rvv_dirty"
            SCX_LLM_CPUS="0-7"
            SCX_SYSTEM_CPUS="6-7"
            SCX_VECTOR_CPUS="0-5"
            SCX_ORDINARY_CPUS="6-7"
            SCX_SLICE_MS="0"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_VECTOR_SLICE_MS="200"
            SCX_ORDINARY_VECTOR_SLICE_MS="50"
            SCX_DIRTY_THRESHOLD="2"
            SCX_FALLBACK_EVERY="4"
            SCX_SWITCH_ALL="1"
            SCX_WORKER_ONLY="0"
            ;;
        rvv_exclusive_6_2)
            SCX_LOADER_STRATEGY="rvv_exclusive"
            SCX_LLM_CPUS="0-7"
            SCX_SYSTEM_CPUS="0-1"
            SCX_VECTOR_CPUS="2-7"
            SCX_ORDINARY_CPUS="0-1"
            SCX_SLICE_MS="0"
            SCX_SYSTEM_SLICE_MS="0"
            SCX_VECTOR_SLICE_MS="1000"
            SCX_ORDINARY_VECTOR_SLICE_MS="0"
            SCX_DIRTY_THRESHOLD="2"
            SCX_FALLBACK_EVERY="4"
            SCX_SWITCH_ALL="1"
            SCX_WORKER_ONLY="0"
            ;;
        *)
            log_error "未知策略: $BENCHMARK_STRATEGY"
            echo "可选策略: cfs, scx_global, pin_all, pin_0_3_iso_4_7, pin_4_7_iso_0_3, long_slice, local_dsq, local_dsq_50ms, local_dsq_100ms, long_slice_isolated, llm_only_pin, llm_only_local_dsq, llm_only_local_dsq_50ms, llm_workers_pin, llm_workers_local_dsq, rvv_dirty_all, rvv_dirty_local_all, rvv_dirty_6_2, rvv_exclusive_6_2"
            exit 1
            ;;
    esac
}

list_strategies() {
    cat << EOF
可选策略:
  cfs                    默认 CFS baseline，不加载 sched_ext
  scx_global             最小 sched_ext 全局队列
  pin_all                LLM 全 CPU 亲和，保持原 CPU
  pin_0_3_iso_4_7        LLM CPU0-3，系统任务 CPU4-7
  pin_4_7_iso_0_3        LLM CPU4-7，系统任务 CPU0-3
  long_slice             LLM 全 CPU，100ms 长时间片
  local_dsq              LLM 优先当前 CPU 本地 DSQ
  local_dsq_50ms         LLM 本地 DSQ + 50ms 时间片
  local_dsq_100ms        LLM 本地 DSQ + 100ms 时间片
  long_slice_isolated    LLM CPU0-3 + 100ms 长时间片，系统任务 CPU4-7
  llm_only_pin           仅 llama-server 线程进入 SCHED_EXT + pin
  llm_only_local_dsq     仅 llama-server 线程进入 SCHED_EXT + local DSQ
  llm_only_local_dsq_50ms 仅 llama-server 线程进入 SCHED_EXT + local DSQ + 50ms
  llm_workers_pin        仅运行时间最高的计算worker进入 SCHED_EXT + pin
  llm_workers_local_dsq  仅运行时间最高的计算worker进入 SCHED_EXT + local DSQ
  rvv_dirty_all          全核心 RVV Dirty 识别，vector 线程长时间片
  rvv_dirty_local_all    RVV Dirty vector 长时间片 + LLM 辅助线程本地 DSQ
  rvv_dirty_6_2          CPU0-5 激进计算核心，CPU6-7 普通/回退核心
  rvv_exclusive_6_2      CPU2-7 RVV专用核心，vector 使用长时间片、本地DSQ；CPU0-1 普通核心
EOF
}

# ========================================
# 环境检查
# ========================================

check_requirements() {
    log_header "检查环境依赖"

    local missing=0

    # 检查必需的命令
    for cmd in sshpass curl jq bc; do
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

    log_info "配置: -t $LLM_THREADS -np $LLM_PARALLEL -c $LLM_CONTEXT"

    if [ "${RESTART_LLM:-1}" = "1" ]; then
        # 停止现有服务器
        log_info "停止现有llama-server..."
        remote_kill_by_pattern "llama-server" "-9"
        sleep 2

        # 启动服务器
        log_info "启动llama-server..."
        ssh_exec "cd $LLM_PATH && THREADS=$LLM_THREADS PARALLEL=$LLM_PARALLEL CTX=$LLM_CONTEXT nohup ./run-server.sh > /tmp/llama-server.log 2>&1 &"
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
                curl -s --max-time "$warmup_timeout" "http://$REMOTE_IP:$REMOTE_PORT/completion" \
                    -H "Content-Type: application/json" \
                    -d "{\"prompt\":\"pre benchmark warmup slot $i\",\"n_predict\":$warmup_tokens,\"temperature\":0,\"stream\":false}" \
                    > "$attempt_dir/slot${i}.json"
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
            if ! jq -e 'has("tokens_predicted")' "$attempt_dir/slot${i}.json" >/dev/null 2>&1; then
                log_warn "warm-up slot $i 未返回 tokens_predicted"
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

# ========================================
# sched_ext调度器管理
# ========================================

SCX_ACTIVE=0
SCX_ACTIVE_LOG=""
SCX_ACTIVE_PATTERN="scx_llm_pin"
CURRENT_RESULT_DIR=""

deploy_scheduler() {
    local local_bin
    local remote_dir
    local remote_bin
    local build_target

    if is_rvv_exclusive_scheduler; then
        local_bin="$SCX_EXCLUSIVE_LOCAL_BIN"
        remote_dir="$SCX_EXCLUSIVE_REMOTE_DIR"
        remote_bin="scx_rvv_exclusive"
        build_target="experiments/scx_rvv_exclusive"
    else
        local_bin="$SCX_LOCAL_BIN"
        remote_dir="$SCX_REMOTE_DIR"
        remote_bin="scx_llm_pin"
        build_target="experiments/scx_llm_pin"
    fi

    if [ ! -x "$local_bin" ]; then
        log_error "找不到可执行调度器: $local_bin"
        log_error "请先执行: make -C $build_target BOARD_IP=$REMOTE_IP"
        exit 1
    fi

    log_info "部署调度器到板卡: $remote_dir/$remote_bin"
    ssh_exec "mkdir -p '$remote_dir'"
    scp_upload "$local_bin" "$remote_dir/$remote_bin"
    ssh_exec "chmod +x '$remote_dir/$remote_bin'"
}

select_worker_tids() {
    local llm_pid=$1
    ssh_exec "for t in /proc/$llm_pid/task/*; do tid=\${t##*/}; runtime=\$(awk '{print \$1}' \$t/schedstat 2>/dev/null || echo 0); echo \$runtime \$tid; done | sort -nr | head -n $LLM_THREADS | awk '{if (out) out=out \",\" \$2; else out=\$2} END {print out}'"
}

start_scheduler() {
    local result_dir=$1
    local llm_pid
    local scheduler_dir="$SCX_REMOTE_DIR"
    local scheduler_bin="scx_llm_pin"
    local scheduler_pattern="scx_llm_pin"

    if [ "${SCX_ENABLED:-0}" -ne 1 ]; then
        log_info "策略: cfs（不加载 sched_ext 调度器）"
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

    if is_rvv_exclusive_scheduler; then
        scheduler_dir="$SCX_EXCLUSIVE_REMOTE_DIR"
        scheduler_bin="scx_rvv_exclusive"
        scheduler_pattern="scx_rvv_exclusive"
    fi

    SCX_ACTIVE_PATTERN="$scheduler_pattern"
    SCX_ACTIVE_LOG="/tmp/${scheduler_bin}_${BENCHMARK_STRATEGY}_$(date +%s).log"

    log_info "清理已有 sched_ext 调度器..."
    remote_kill_by_pattern "scx_llm_pin" "-TERM"
    remote_kill_by_pattern "scx_rvv_exclusive" "-TERM"
    sleep 1

    if is_rvv_exclusive_scheduler; then
        local switch_all_arg=""
        if [ "${SCX_SWITCH_ALL:-1}" = "0" ]; then
            switch_all_arg="--no-switch-all"
        fi

        log_info "启动策略: loader=$SCX_LOADER_STRATEGY, tgid=$llm_pid, compute_cpus=$SCX_VECTOR_CPUS, ordinary_cpus=$SCX_ORDINARY_CPUS, compute_slice=${SCX_VECTOR_SLICE_MS}ms, ordinary_vector_slice=${SCX_ORDINARY_VECTOR_SLICE_MS}ms, dirty_threshold=${SCX_DIRTY_THRESHOLD}, ordinary_every=${SCX_FALLBACK_EVERY}, switch_all=${SCX_SWITCH_ALL}"
        ssh_exec "cd '$scheduler_dir' && nohup ./$scheduler_bin --tgid '$llm_pid' --compute-cpus '$SCX_VECTOR_CPUS' --ordinary-cpus '$SCX_ORDINARY_CPUS' --compute-slice-ms '$SCX_VECTOR_SLICE_MS' --ordinary-vector-slice-ms '$SCX_ORDINARY_VECTOR_SLICE_MS' --system-slice-ms '$SCX_SYSTEM_SLICE_MS' --dirty-threshold '$SCX_DIRTY_THRESHOLD' --ordinary-every '$SCX_FALLBACK_EVERY' $switch_all_arg > '$SCX_ACTIVE_LOG' 2>&1 &"
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
        return 0
    fi

    local llm_only_arg=""
    local tid_list_arg=""
    if [ "${SCX_SWITCH_ALL:-1}" = "0" ]; then
        llm_only_arg="--llm-only"
        if [ "${SCX_WORKER_ONLY:-0}" = "1" ]; then
            local tid_list
            tid_list=$(select_worker_tids "$llm_pid")
            if [ -z "$tid_list" ]; then
                log_error "worker-only策略无法选择TID"
                exit 1
            fi
            tid_list_arg="--tid-list '$tid_list'"
            echo "$tid_list" > "$result_dir/scx_worker_tids.txt"
            log_info "worker-only TIDs: $tid_list"
        fi
    fi

    log_info "启动策略: loader=$SCX_LOADER_STRATEGY, tgid=$llm_pid, llm_cpus=$SCX_LLM_CPUS, system_cpus=$SCX_SYSTEM_CPUS, vector_cpus=$SCX_VECTOR_CPUS, ordinary_cpus=$SCX_ORDINARY_CPUS, slice=${SCX_SLICE_MS}ms, vector_slice=${SCX_VECTOR_SLICE_MS}ms, ordinary_vector_slice=${SCX_ORDINARY_VECTOR_SLICE_MS}ms, switch_all=${SCX_SWITCH_ALL}, worker_only=${SCX_WORKER_ONLY}"
    ssh_exec "cd '$scheduler_dir' && nohup ./$scheduler_bin --tgid '$llm_pid' --strategy '$SCX_LOADER_STRATEGY' --llm-cpus '$SCX_LLM_CPUS' --system-cpus '$SCX_SYSTEM_CPUS' --vector-cpus '$SCX_VECTOR_CPUS' --ordinary-cpus '$SCX_ORDINARY_CPUS' --slice-ms '$SCX_SLICE_MS' --system-slice-ms '$SCX_SYSTEM_SLICE_MS' --vector-slice-ms '$SCX_VECTOR_SLICE_MS' --ordinary-vector-slice-ms '$SCX_ORDINARY_VECTOR_SLICE_MS' --dirty-threshold '$SCX_DIRTY_THRESHOLD' --fallback-every '$SCX_FALLBACK_EVERY' $llm_only_arg $tid_list_arg > '$SCX_ACTIVE_LOG' 2>&1 &"
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

    duration=$((TEST_DURATION + RVV_TRACE_EXTRA_SEC))
    RVV_TRACE_LOG="/tmp/rvv_sched_cost_${BENCHMARK_STRATEGY}_$(date +%s).log"
    RVV_TRACE_JSON="/tmp/rvv_sched_cost_${BENCHMARK_STRATEGY}_$(date +%s).json"

    remote_kill_by_pattern "rvv_sched_cost" "-TERM"
    sleep 1

    log_info "启动rvv_sched_cost: tgid=$llm_pid duration=${duration}s"
    ssh_exec "cd '$RVV_REMOTE_DIR' && LD_LIBRARY_PATH='$RVV_REMOTE_DIR/lib' nohup ./rvv_sched_cost --tgid '$llm_pid' --duration '$duration' --json '$RVV_TRACE_JSON' > '$RVV_TRACE_LOG' 2>&1 &"
    sleep 2

    if [ -n "$(remote_pid_by_pattern "rvv_sched_cost")" ]; then
        RVV_TRACE_ACTIVE=1
        echo "rvv_sched_cost log: $RVV_TRACE_LOG" > "$result_dir/rvv_trace_info.txt"
        echo "rvv_sched_cost json: $RVV_TRACE_JSON" >> "$result_dir/rvv_trace_info.txt"
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

    local start_time=$(date +%s.%N)
    local first_token_time=""
    local total_tokens=0
    local request_count=0

    log_info "[Task $task_id] 开始测试..."

    # 创建CSV文件并写入头部
    echo "timestamp,request_num,tokens,generation_ms,total_ms,toks_per_sec" > "$output_file.csv"

    # 记录开始时间
    local test_end_time=$(($(date +%s) + TEST_DURATION))

    while [ $(date +%s) -lt $test_end_time ]; do
        local req_start=$(date +%s.%N)

        # 发送请求并记录响应
        local response=$(curl -s "http://$REMOTE_IP:$REMOTE_PORT/completion" \
            -H "Content-Type: application/json" \
            -d "{
                \"prompt\": \"$prompt\",
                \"n_predict\": $TOKENS_PER_REQUEST,
                \"temperature\": 0.7,
                \"stream\": false
            }")

        local req_end=$(date +%s.%N)

        # 解析响应
        if echo "$response" | jq -e '.tokens_predicted' > /dev/null 2>&1; then
            local tokens=$(echo "$response" | jq -r '.tokens_predicted // 0')
            local generation_time=$(echo "$response" | jq -r '.timings.predicted_ms // 0')

            total_tokens=$((total_tokens + tokens))
            request_count=$((request_count + 1))

            # 记录首字延迟（第一个请求）
            if [ -z "$first_token_time" ]; then
                local ttft=$(echo "$response" | jq -r '.timings.prompt_ms // 0')
                first_token_time=$ttft
                log_info "[Task $task_id] 首字延迟: ${ttft}ms"
            fi

            # 计算此请求的 tok/s
            local toks_per_sec=$(echo "scale=2; if ($generation_time > 0) $tokens * 1000 / $generation_time else 0" | bc)

            # 记录这次请求
            local request_time=$(echo "$req_end - $req_start" | bc)
            echo "$(date +%s.%N),$request_count,$tokens,$generation_time,$request_time,$toks_per_sec" >> "$output_file.csv"

        else
            log_warn "[Task $task_id] 请求失败: $response"
        fi

        # 短暂休息避免过载
        sleep 0.5
    done

    local end_time=$(date +%s.%N)
    local total_time=$(echo "$end_time - $start_time" | bc)
    local avg_toks=$(echo "scale=2; $total_tokens / $total_time" | bc)

    # 生成任务总结
    cat > "$output_file.summary" << EOF
Task ID: $task_id
Prompt: ${prompt:0:50}...
Duration: ${total_time}s
Total Requests: $request_count
Total Tokens: $total_tokens
Average tok/s: $avg_toks
First Token Time: ${first_token_time}ms
EOF

    log_info "[Task $task_id] 完成 - $avg_toks tok/s, $request_count 请求"
}

# ========================================
# 主测试流程
# ========================================

run_benchmark() {
    log_header "开始基准测试"

    # 创建输出目录
    local timestamp=$(date +%Y%m%d_%H%M%S)
    local strategy_slug
    strategy_slug=$(echo "$BENCHMARK_STRATEGY" | tr -c 'A-Za-z0-9_.-' '_')
    local result_dir="$RESULTS_DIR/${timestamp}_${strategy_slug}"
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
Parallel Slots: $LLM_PARALLEL
Context Length: $LLM_CONTEXT
Test Duration: ${TEST_DURATION}s
Concurrent Tasks: $CONCURRENT_TASKS
Tokens per Request: $TOKENS_PER_REQUEST
Restart LLM: $RESTART_LLM
Benchmark Strategy: $BENCHMARK_STRATEGY
SCX Enabled: ${SCX_ENABLED:-0}
SCX Loader Strategy: $SCX_LOADER_STRATEGY
SCX LLM CPUs: $SCX_LLM_CPUS
SCX System CPUs: $SCX_SYSTEM_CPUS
SCX LLM Slice ms: $SCX_SLICE_MS
SCX System Slice ms: $SCX_SYSTEM_SLICE_MS
SCX Switch All: $SCX_SWITCH_ALL
SCX Worker Only: $SCX_WORKER_ONLY
SCX Parallel Warmup: $SCX_PARALLEL_WARMUP
SCX Warmup Tokens: $SCX_WARMUP_TOKENS
SCX Vector CPUs: $SCX_VECTOR_CPUS
SCX Ordinary CPUs: $SCX_ORDINARY_CPUS
SCX Vector Slice ms: $SCX_VECTOR_SLICE_MS
SCX Ordinary Vector Slice ms: $SCX_ORDINARY_VECTOR_SLICE_MS
SCX Dirty Threshold: $SCX_DIRTY_THRESHOLD
SCX Fallback Every: $SCX_FALLBACK_EVERY
Pre Benchmark Warmup: $PRE_BENCH_WARMUP
Pre Benchmark Warmup Requests: $PRE_BENCH_WARMUP_REQUESTS
Pre Benchmark Warmup Tokens: $PRE_BENCH_WARMUP_TOKENS
Pre Benchmark Warmup Retries: $PRE_BENCH_WARMUP_RETRIES
Pre Benchmark Warmup Timeout: $PRE_BENCH_WARMUP_TIMEOUT
Pre Benchmark Warmup Strict: $PRE_BENCH_WARMUP_STRICT
RVV Trace: $RVV_TRACE
RVV Trace Extra sec: $RVV_TRACE_EXTRA_SEC
EOF

    # 记录系统信息
    log_info "收集系统信息..."
    ssh_exec "uname -a" > "$result_dir/system_info.txt"
    ssh_exec "cat /proc/cpuinfo | grep -E 'processor|model name|cpu MHz' | head -20" >> "$result_dir/system_info.txt"
    ssh_exec "free -h" >> "$result_dir/system_info.txt"

    # 启动调度策略（CFS baseline 时为空操作）
    start_scheduler "$result_dir"

    # 正式计时前预热 llama-server 并发 slot，避免首个正式请求承担冷启动成本
    warmup_before_benchmark "$result_dir"

    # 启动调度观测，覆盖正式benchmark窗口和请求收尾阶段
    start_rvv_trace "$result_dir"

    # 启动并发任务
    log_header "启动 $CONCURRENT_TASKS 个并发任务"

    local pids=()
    local task_id

    for task_id in $(seq 1 "$CONCURRENT_TASKS"); do
        local prompt
        prompt=$(get_task_prompt "$task_id")
        (run_single_task "$task_id" "$prompt" "$result_dir/task${task_id}") &
        pids+=($!)
    done

    log_info "所有任务已启动，等待完成..."

    # 显示进度
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

    # 等待所有任务完成
    log_info "等待所有任务结束..."
    for pid in "${pids[@]}"; do
        wait $pid
    done

    log_info "✓ 所有任务已完成"

    # 停止调度观测并抓取JSON
    stop_rvv_trace "$result_dir"

    # 停止调度器并抓取统计日志
    stop_scheduler "$result_dir"

    # 生成汇总报告
    generate_report "$result_dir"

    log_header "测试完成"
    log_info "结果保存在: $result_dir"
    log_info "查看报告: cat $result_dir/report.txt"
}

# ========================================
# 报告生成
# ========================================

generate_report() {
    local result_dir=$1
    local report_file="$result_dir/report.txt"

    log_info "生成测试报告..."

    {
        echo "========================================"
        echo "RVV LLM Benchmark Report"
        echo "========================================"
        echo ""
        echo "测试时间: $(date)"
        echo "测试时长: ${TEST_DURATION}s"
        echo "并发任务: $CONCURRENT_TASKS"
        echo "策略: $BENCHMARK_STRATEGY"
        echo "SCX加载策略: $SCX_LOADER_STRATEGY"
        echo "SCX LLM CPUs: $SCX_LLM_CPUS"
        echo "SCX System CPUs: $SCX_SYSTEM_CPUS"
        echo "SCX LLM Slice: ${SCX_SLICE_MS}ms"
        echo "SCX Switch All: $SCX_SWITCH_ALL"
        echo "SCX Worker Only: $SCX_WORKER_ONLY"
        echo "SCX Parallel Warmup: $SCX_PARALLEL_WARMUP"
        echo "SCX Warmup Tokens: $SCX_WARMUP_TOKENS"
        echo "SCX Vector CPUs: $SCX_VECTOR_CPUS"
        echo "SCX Ordinary CPUs: $SCX_ORDINARY_CPUS"
        echo "SCX Vector Slice: ${SCX_VECTOR_SLICE_MS}ms"
        echo "SCX Ordinary Vector Slice: ${SCX_ORDINARY_VECTOR_SLICE_MS}ms"
        echo "SCX Dirty Threshold: $SCX_DIRTY_THRESHOLD"
        echo "SCX Fallback Every: $SCX_FALLBACK_EVERY"
        echo "Pre Benchmark Warmup: $PRE_BENCH_WARMUP"
        echo "Pre Benchmark Warmup Requests: $PRE_BENCH_WARMUP_REQUESTS"
        echo "Pre Benchmark Warmup Tokens: $PRE_BENCH_WARMUP_TOKENS"
        echo "Pre Benchmark Warmup Retries: $PRE_BENCH_WARMUP_RETRIES"
        echo "Pre Benchmark Warmup Timeout: $PRE_BENCH_WARMUP_TIMEOUT"
        echo "Pre Benchmark Warmup Strict: $PRE_BENCH_WARMUP_STRICT"
        echo "RVV Trace: $RVV_TRACE"
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

        # 计算总吞吐量
        local total_toks=0
        local total_reqs=0
        local total_ttft=0
        local ttft_count=0

        for i in $(seq 1 "$CONCURRENT_TASKS"); do
            if [ -f "$result_dir/task${i}.summary" ]; then
                local toks=$(grep "Average tok/s" "$result_dir/task${i}.summary" | awk '{print $3}')
                local reqs=$(grep "Total Requests" "$result_dir/task${i}.summary" | awk '{print $3}')
                local ttft=$(grep "First Token Time" "$result_dir/task${i}.summary" | awk '{print $4}' | sed 's/ms//')

                toks=${toks:-0}
                reqs=${reqs:-0}
                total_toks=$(echo "$total_toks + $toks" | bc)
                total_reqs=$((total_reqs + reqs))
                if [ -n "$ttft" ]; then
                    total_ttft=$(echo "$total_ttft + $ttft" | bc)
                    ttft_count=$((ttft_count + 1))
                fi
            fi
        done

        local avg_ttft="N/A"
        if [ "$ttft_count" -gt 0 ]; then
            avg_ttft=$(echo "scale=2; $total_ttft / $ttft_count" | bc)
        fi

        echo "总吞吐量: $total_toks tok/s"
        echo "总请求数: $total_reqs"
        if [ "$avg_ttft" = "N/A" ]; then
            echo "平均首字延迟: N/A"
        else
            echo "平均首字延迟: ${avg_ttft}ms"
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

# ========================================
# 清理函数
# ========================================

cleanup() {
    log_info "清理环境..."
    stop_rvv_trace "$CURRENT_RESULT_DIR"
    stop_scheduler "$CURRENT_RESULT_DIR"
    # 停止所有后台任务
    jobs -p | xargs -r kill 2>/dev/null || true
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
            --strategy)
                BENCHMARK_STRATEGY="$2"
                shift 2
                ;;
            --vector-slice-ms)
                SCX_VECTOR_SLICE_OVERRIDE_MS="$2"
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
                echo "  --duration N   测试时长（秒，默认300）"
                echo "  --tasks N      并发任务数（默认4）"
                echo "  --threads N    LLM计算线程数（默认8）"
                echo "  --strategy S   测试策略（默认cfs，使用 --list-strategies 查看）"
                echo "  --vector-slice-ms N  覆盖 RVV dirty vector 时间片（毫秒）"
                echo "  --dirty-threshold N  覆盖 RVV dirty 命中阈值"
                echo "  --warmup-requests N  正式测试前预热请求数（默认跟随 --tasks）"
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

    # 创建输出目录
    mkdir -p "$RESULTS_DIR" "$LOG_DIR"

    if [ "${PRE_BENCH_WARMUP_REQUESTS_AUTO:-0}" = "1" ]; then
        PRE_BENCH_WARMUP_REQUESTS="$CONCURRENT_TASKS"
    fi

    require_positive_int "TEST_DURATION" "$TEST_DURATION"
    require_positive_int "CONCURRENT_TASKS" "$CONCURRENT_TASKS"
    require_positive_int "LLM_THREADS" "$LLM_THREADS"
    require_positive_int "PRE_BENCH_WARMUP_REQUESTS" "$PRE_BENCH_WARMUP_REQUESTS"
    require_positive_int "PRE_BENCH_WARMUP_TOKENS" "$PRE_BENCH_WARMUP_TOKENS"
    require_positive_int "PRE_BENCH_WARMUP_RETRIES" "$PRE_BENCH_WARMUP_RETRIES"
    require_positive_int "PRE_BENCH_WARMUP_TIMEOUT" "$PRE_BENCH_WARMUP_TIMEOUT"

    apply_strategy_preset
    if [ -n "$SCX_VECTOR_SLICE_OVERRIDE_MS" ]; then
        if ! [[ "$SCX_VECTOR_SLICE_OVERRIDE_MS" =~ ^[0-9]+$ ]]; then
            log_error "SCX_VECTOR_SLICE_OVERRIDE_MS 必须是非负整数，当前值: $SCX_VECTOR_SLICE_OVERRIDE_MS"
            exit 1
        fi
        SCX_VECTOR_SLICE_MS="$SCX_VECTOR_SLICE_OVERRIDE_MS"
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
    start_llm_server
    run_benchmark

    log_header "全部完成！"
}

# 运行主函数
main "$@"
