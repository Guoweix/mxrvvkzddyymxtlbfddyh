#!/bin/bash
# Benchmark配置文件

# ========================================
# 远程机器配置
# ========================================
export REMOTE_IP="192.168.3.67"
export REMOTE_USER="root"
export REMOTE_PASS="bianbu"
export REMOTE_PORT="9090"

# ========================================
# LLM配置
# ========================================
export LLM_THREADS=8          # 单 token / generation 计算线程数 (-t)
# 留空时在参数解析后跟随 LLM_THREADS；也可通过环境变量或 --threads-batch 单独覆盖。
export LLM_THREADS_BATCH="${LLM_THREADS_BATCH:-}" # 多 token / prompt batch 线程数 (-tb)
export LLM_PARALLEL=4         # 并行槽位数 (-np)
export LLM_CONTEXT=4096       # 上下文长度 (-c)
# GNU libgomp barrier spin count；留空表示使用板端默认值。
export GOMP_SPINCOUNT="${GOMP_SPINCOUNT:-}"
export LLM_PATH="${LLM_PATH:-/mnt/llm/llama}"
export RESTART_LLM="${RESTART_LLM:-1}"

# ========================================
# sched_ext / eBPF策略配置
# ========================================
# BENCHMARK_STRATEGY 可选：
#   cfs, scx_global, pin_all, pin_0_3_iso_4_7, pin_4_7_iso_0_3,
#   long_slice, local_dsq, local_dsq_50ms, local_dsq_100ms,
#   long_slice_isolated, llm_only_pin, llm_only_local_dsq,
#   llm_only_local_dsq_50ms, llm_workers_pin, llm_workers_local_dsq,
#   rvv_dirty_all, rvv_dirty_bind_all, rvv_dirty_lease_all, rvv_dirty_lease_unique_all,
#   rvv_dirty_lease_user_observe, rvv_dirty_lease_user_active,
#   rvv_dirty_local_all, rvv_dirty_6_2, rvv_exclusive_6_2,
#   rvv_lpedf_all, rvv_barrier_gang_active,
#   linux_affinity
export BENCHMARK_STRATEGY="${BENCHMARK_STRATEGY:-cfs}"
export SCX_AUTO_DEPLOY="${SCX_AUTO_DEPLOY:-1}"
export SCX_LOCAL_BIN="${SCX_LOCAL_BIN:-../scx_llm_pin/scx_llm_pin}"
export SCX_LEASE_CTL_LOCAL_BIN="${SCX_LEASE_CTL_LOCAL_BIN:-../scx_llm_pin/rvv_lease_ctl}"
export SCX_REMOTE_DIR="${SCX_REMOTE_DIR:-/tmp/scx_llm_pin}"
export SCX_REMOTE_LOG="${SCX_REMOTE_LOG:-/tmp/scx_llm_pin.log}"
export SCX_EXCLUSIVE_LOCAL_BIN="${SCX_EXCLUSIVE_LOCAL_BIN:-../scx_rvv_exclusive/scx_rvv_exclusive}"
export SCX_EXCLUSIVE_REMOTE_DIR="${SCX_EXCLUSIVE_REMOTE_DIR:-/tmp/scx_rvv_exclusive}"
export SCX_LPEDF_LOCAL_BIN="${SCX_LPEDF_LOCAL_BIN:-../scx_rvv_lpedf/scx_rvv_lpedf}"
export SCX_LPEDF_REMOTE_DIR="${SCX_LPEDF_REMOTE_DIR:-/tmp/scx_rvv_lpedf}"
export SCX_GANG_LOCAL_BIN="${SCX_GANG_LOCAL_BIN:-../scx_rvv_barrier_gang/scx_rvv_barrier_gang}"
export SCX_GANG_REMOTE_DIR="${SCX_GANG_REMOTE_DIR:-/tmp/scx_rvv_barrier_gang}"
export SCX_LOADER_STRATEGY="${SCX_LOADER_STRATEGY:-pin}"
export SCX_LLM_CPUS="${SCX_LLM_CPUS:-0-7}"
export SCX_SYSTEM_CPUS="${SCX_SYSTEM_CPUS:-0-7}"
export SCX_SLICE_MS="${SCX_SLICE_MS:-0}"
export SCX_SYSTEM_SLICE_MS="${SCX_SYSTEM_SLICE_MS:-0}"
export SCX_SWITCH_ALL="${SCX_SWITCH_ALL:-1}"
export SCX_WORKER_ONLY="${SCX_WORKER_ONLY:-0}"
export SCX_PARALLEL_WARMUP="${SCX_PARALLEL_WARMUP:-1}"
export SCX_WARMUP_TOKENS="${SCX_WARMUP_TOKENS:-1}"
export SCX_VECTOR_CPUS="${SCX_VECTOR_CPUS:-0-7}"
export SCX_ORDINARY_CPUS="${SCX_ORDINARY_CPUS:-0-7}"
export SCX_VECTOR_SLICE_MS="${SCX_VECTOR_SLICE_MS:-0}"
export SCX_ORDINARY_VECTOR_SLICE_MS="${SCX_ORDINARY_VECTOR_SLICE_MS:-0}"
export SCX_VECTOR_LOCAL_WINDOW_MS="${SCX_VECTOR_LOCAL_WINDOW_MS:-500}"
export SCX_DIRTY_THRESHOLD="${SCX_DIRTY_THRESHOLD:-2}"
export SCX_FALLBACK_EVERY="${SCX_FALLBACK_EVERY:-0}"
export SCX_LPEDF_SEGMENT_MS="${SCX_LPEDF_SEGMENT_MS:-50}"
export SCX_LPEDF_PERIOD_MS="${SCX_LPEDF_PERIOD_MS:-50}"
export SCX_LPEDF_DEADLINE_MS="${SCX_LPEDF_DEADLINE_MS:-50}"
export SCX_GANG_WAKEUP_WINDOW_US="${SCX_GANG_WAKEUP_WINDOW_US:-200}"
export SCX_GANG_MIN_WAKEUPS="${SCX_GANG_MIN_WAKEUPS:-2}"
export SCX_GANG_BOOST_US="${SCX_GANG_BOOST_US:-500}"
export SCX_GANG_COOLDOWN_US="${SCX_GANG_COOLDOWN_US:-500}"
export SCX_GANG_SERVICE_WINDOW_US="${SCX_GANG_SERVICE_WINDOW_US:-1000}"
export SCX_GANG_LAGGARD_THRESHOLD_US="${SCX_GANG_LAGGARD_THRESHOLD_US:-100}"
export SCX_GANG_HINT_MODE="${SCX_GANG_HINT_MODE:-active}"
export SCX_GANG_HINT_MAP="${SCX_GANG_HINT_MAP:-/sys/fs/bpf/scx_rvv_barrier_gang/hints}"
export SCX_GANG_HINT_BOOST_US="${SCX_GANG_HINT_BOOST_US:-250}"
export SCX_GANG_HINT_LAGGARD_WAITERS="${SCX_GANG_HINT_LAGGARD_WAITERS:-7}"
export SCX_GANG_HINT_LOCAL_LIB="${SCX_GANG_HINT_LOCAL_LIB:-../../kbuild/llama.cpp/build-riscv-hint/bin/libggml-cpu.so.0.13.0}"
export SCX_GANG_HINT_REMOTE_LIB="${SCX_GANG_HINT_REMOTE_LIB:-$LLM_PATH/bin/libggml-cpu.so.0.13.0}"
export SCX_LEASE_CONTROL_MODE="${SCX_LEASE_CONTROL_MODE:-off}"
export SCX_LEASE_CONTROL_DIR="${SCX_LEASE_CONTROL_DIR:-/sys/fs/bpf/scx_rvv_dirty_lease}"
export SCX_LEASE_USER_CPU_MAP="${SCX_LEASE_USER_CPU_MAP:-auto}"
export SCX_LEASE_HINT_LOCAL_LIB="${SCX_LEASE_HINT_LOCAL_LIB:-../../kbuild/llama.cpp/build-riscv-hint/bin/libggml-cpu.so.0.13.0}"
export SCX_LEASE_HINT_REMOTE_LIB="${SCX_LEASE_HINT_REMOTE_LIB:-$LLM_PATH/bin/libggml-cpu.so.0.13.0}"

# ========================================
# rvv_sched_cost 调度观测配置
# ========================================
export RVV_TRACE="${RVV_TRACE:-0}"
export RVV_TRACE_AUTO_DEPLOY="${RVV_TRACE_AUTO_DEPLOY:-1}"
export RVV_TRACE_EXTRA_SEC="${RVV_TRACE_EXTRA_SEC:-180}"
export RVV_TRACE_SWITCH_OUT="${RVV_TRACE_SWITCH_OUT:-1}"
export RVV_TRACE_SWITCH_REASON="${RVV_TRACE_SWITCH_REASON:-1}"
export RVV_TRACE_RVV_ONLY="${RVV_TRACE_RVV_ONLY:-0}"
export RVV_TRACE_FUTEX="${RVV_TRACE_FUTEX:-0}"
export RVV_TRACE_FUTEX_DETAIL="${RVV_TRACE_FUTEX_DETAIL:-0}"
export RVV_TRACE_FUTEX_STACK="${RVV_TRACE_FUTEX_STACK:-0}"
export RVV_LOCAL_BIN="${RVV_LOCAL_BIN:-../rvv_sched_cost/rvv_sched_cost}"
export RVV_LOCAL_LIBELF="${RVV_LOCAL_LIBELF:-../../jupiter-buildroot/output/k1_v2/target/usr/lib/libelf-0.186.so}"
export RVV_REMOTE_DIR="${RVV_REMOTE_DIR:-/tmp/rvv_sched_cost}"

# ========================================
# 测试配置
# ========================================
export TEST_DURATION="${TEST_DURATION:-300}"       # 单轮测试窗口（秒，默认5分钟）
export BENCHMARK_LOOPS="${BENCHMARK_LOOPS:-1}"     # 每个并发档位重复轮数
export CONCURRENCY_LEVELS="${CONCURRENCY_LEVELS:-2,4,8}" # 逗号分隔，例如 2,4,8
export CONCURRENT_TASKS="${CONCURRENT_TASKS:-4}"   # 兼容旧参数；suite 会按 CONCURRENCY_LEVELS 覆盖
export TOKENS_PER_REQUEST="${TOKENS_PER_REQUEST:-100}" # 每个请求生成的token数
export REQUEST_TIMEOUT_SEC="${REQUEST_TIMEOUT_SEC:-180}" # 单请求curl超时
export FIXED_TOTAL_TOKENS="${FIXED_TOTAL_TOKENS:-1000}" # 非零时启用固定总token模式
export FIXED_REQUEST_TIMEOUT_SEC="${FIXED_REQUEST_TIMEOUT_SEC:-600}"
export REQUEST_TEMPERATURE="${REQUEST_TEMPERATURE:-0}"
export REQUEST_SEED="${REQUEST_SEED:-1234}"
export REQUEST_IGNORE_EOS="${REQUEST_IGNORE_EOS:-1}"
# 并发 worker 在统一绝对时间点发出首个正式请求；后续请求保持自然连续。
export SYNC_CONCURRENT_START="${SYNC_CONCURRENT_START:-1}"
export SYNC_START_LEAD_SEC="${SYNC_START_LEAD_SEC:-3}"
export SYNC_START_SPIN_MS="${SYNC_START_SPIN_MS:-2}"
export PHASE_GATE_ENABLED="${PHASE_GATE_ENABLED:-1}"
export PHASE_GATE_THRESHOLD_MS="${PHASE_GATE_THRESHOLD_MS:-100}"
export PHASE_GATE_MAX_RETRIES="${PHASE_GATE_MAX_RETRIES:-5}"
export PHASE_GATE_TOKENS="${PHASE_GATE_TOKENS:-1}"
export PHASE_GATE_TIMEOUT_SEC="${PHASE_GATE_TIMEOUT_SEC:-60}"

# ========================================
# Linux affinity worker pin 配置
# ========================================
# 不使用 sched_ext，仅通过 taskset 固定 llama.cpp 计算 worker 的 CPU 亲和性。
export AFFINITY_PIN_WORKERS="${AFFINITY_PIN_WORKERS:-0}"
export AFFINITY_WORKER_CPUS="${AFFINITY_WORKER_CPUS:-0-7}"
export AFFINITY_WORKER_COUNT="${AFFINITY_WORKER_COUNT:-$LLM_THREADS}"

# ========================================
# 正式测试前预热配置
# ========================================
# 预热在调度器启动后、正式计时窗口开始前执行，不计入benchmark结果。
export PRE_BENCH_WARMUP="${PRE_BENCH_WARMUP:-1}"
if [ -z "${PRE_BENCH_WARMUP_REQUESTS+x}" ]; then
    export PRE_BENCH_WARMUP_REQUESTS="$CONCURRENT_TASKS"
    export PRE_BENCH_WARMUP_REQUESTS_AUTO=1
else
    export PRE_BENCH_WARMUP_REQUESTS
    export PRE_BENCH_WARMUP_REQUESTS_AUTO=0
fi
export PRE_BENCH_WARMUP_TOKENS="${PRE_BENCH_WARMUP_TOKENS:-${SCX_WARMUP_TOKENS:-1}}"
export PRE_BENCH_WARMUP_RETRIES="${PRE_BENCH_WARMUP_RETRIES:-3}"
export PRE_BENCH_WARMUP_TIMEOUT="${PRE_BENCH_WARMUP_TIMEOUT:-120}"
export PRE_BENCH_WARMUP_STRICT="${PRE_BENCH_WARMUP_STRICT:-1}"

# ========================================
# 输出配置
# ========================================
export RESULTS_DIR="./results"
export LOG_DIR="./logs"

# ========================================
# 测试Prompts（4个不同的任务）
# ========================================
# 每个prompt有不同的特点，测试不同场景

# Task 1: 技术解释（计算密集）
export PROMPT_1="Explain the RISC-V Vector Extension (RVV) in detail, including its purpose, key features, and how it differs from other SIMD instruction sets."

# Task 2: 代码生成（逻辑密集）
export PROMPT_2="Write a complete Python implementation of a binary search tree with insert, delete, search, and traverse methods. Include comprehensive docstrings."

# Task 3: 数学计算（推理密集）
export PROMPT_3="Solve this step by step: If a car travels at 60 km/h for 2.5 hours, then at 80 km/h for 1.5 hours, what is the average speed for the entire journey?"

# Task 4: 创意写作（生成密集）
export PROMPT_4="Write a short story about an AI researcher who discovers that their neural network has become self-aware. Make it suspenseful and thought-provoking."

# ========================================
# 颜色输出（用于美化日志）
# ========================================
export COLOR_RESET="\033[0m"
export COLOR_RED="\033[31m"
export COLOR_GREEN="\033[32m"
export COLOR_YELLOW="\033[33m"
export COLOR_BLUE="\033[34m"
export COLOR_MAGENTA="\033[35m"
export COLOR_CYAN="\033[36m"
export COLOR_BOLD="\033[1m"
