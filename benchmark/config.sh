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
export LLM_THREADS=8          # 计算线程数 (-t)
export LLM_PARALLEL=4         # 并行槽位数 (-np)
export LLM_CONTEXT=4096       # 上下文长度 (-c)
export LLM_PATH="/mnt/llm/llama"
export RESTART_LLM="${RESTART_LLM:-1}"

# ========================================
# sched_ext / eBPF策略配置
# ========================================
# BENCHMARK_STRATEGY 可选：
#   cfs, scx_global, pin_all, pin_0_3_iso_4_7, pin_4_7_iso_0_3,
#   long_slice, local_dsq, local_dsq_50ms, local_dsq_100ms,
#   long_slice_isolated, llm_only_pin, llm_only_local_dsq,
#   llm_only_local_dsq_50ms, llm_workers_pin, llm_workers_local_dsq,
#   rvv_dirty_all, rvv_dirty_local_all, rvv_dirty_6_2, rvv_exclusive_6_2
export BENCHMARK_STRATEGY="${BENCHMARK_STRATEGY:-cfs}"
export SCX_AUTO_DEPLOY="${SCX_AUTO_DEPLOY:-1}"
export SCX_LOCAL_BIN="${SCX_LOCAL_BIN:-../scx_llm_pin/scx_llm_pin}"
export SCX_REMOTE_DIR="${SCX_REMOTE_DIR:-/tmp/scx_llm_pin}"
export SCX_REMOTE_LOG="${SCX_REMOTE_LOG:-/tmp/scx_llm_pin.log}"
export SCX_EXCLUSIVE_LOCAL_BIN="${SCX_EXCLUSIVE_LOCAL_BIN:-../scx_rvv_exclusive/scx_rvv_exclusive}"
export SCX_EXCLUSIVE_REMOTE_DIR="${SCX_EXCLUSIVE_REMOTE_DIR:-/tmp/scx_rvv_exclusive}"
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
export SCX_DIRTY_THRESHOLD="${SCX_DIRTY_THRESHOLD:-2}"
export SCX_FALLBACK_EVERY="${SCX_FALLBACK_EVERY:-0}"

# ========================================
# rvv_sched_cost 调度观测配置
# ========================================
export RVV_TRACE="${RVV_TRACE:-0}"
export RVV_TRACE_AUTO_DEPLOY="${RVV_TRACE_AUTO_DEPLOY:-1}"
export RVV_TRACE_EXTRA_SEC="${RVV_TRACE_EXTRA_SEC:-180}"
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
