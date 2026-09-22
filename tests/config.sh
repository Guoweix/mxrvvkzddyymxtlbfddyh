#!/bin/bash
# 项目统一配置文件：所有测试脚本与 Makefile 都从这里取值（环境变量优先）。

# ========================================
# 远程机器配置
# ========================================
export REMOTE_IP="${REMOTE_IP:-192.168.3.67}"
export REMOTE_USER="${REMOTE_USER:-root}"
export REMOTE_PASS="${REMOTE_PASS:-bianbu}"
export REMOTE_PORT="${REMOTE_PORT:-9090}"

# ========================================
# 工具链 / 内核源码根目录（其中包含 jupiter-buildroot）
# ========================================
# 说明：本文件里的“相对路径”一律相对仓库根目录（该目录含 src/ 与 tests/）。
# jupiter-buildroot 在仓库上一级；若在别处，改成绝对路径。
export TOOLCHAIN_ROOT="${TOOLCHAIN_ROOT:-..}"

# ========================================
# LLM配置
# ========================================
export LLM_THREADS=8          # 单 token / generation 计算线程数 (-t)
export LLM_THREADS_BATCH="${LLM_THREADS_BATCH:-}" # 多 token / prompt batch 线程数 (-tb)
export LLM_CONTEXT=4096       # 上下文长度 (-c)
export GOMP_SPINCOUNT="${GOMP_SPINCOUNT:-}"       # 留空使用板端默认值
export LLM_PATH="${LLM_PATH:-/mnt/llm/llama}"

# ========================================
# dynamic_threads（主干策略）
# ========================================
# 已应用 0001→0002→llama-dynamic.patch 的 llama.cpp ggml-cpu 目录（编译 phase_probe 用）。
export LLAMA_CPU_SRC="${LLAMA_CPU_SRC:-../experiments/dynamic_threads/llama.cpp/ggml/src/ggml-cpu}"
# 上述 llama.cpp 构建出的 patched runtime（run_benchmark/run_llama_gate 上传用）。
export DT_LLAMA_BIN="${DT_LLAMA_BIN:-../experiments/dynamic_threads/llama.cpp/build-dynamic-riscv/bin}"

# ========================================
# x86_dynamic_threads（x86 迁移）
# ========================================
# 已应用 0001→0002→llama-dynamic 的 x86 llama.cpp ggml-cpu（编译 phase_probe）。
export DT_X86_LLAMA_CPU_SRC="${DT_X86_LLAMA_CPU_SRC:-../experiments/x86_dynamic_threads/llama/ggml/src/ggml-cpu}"
# x86 llama-server 与共享库目录（run_dual_model_x86/run_multi_x86 本机启动）。
export DT_X86_LLAMA_BIN="${DT_X86_LLAMA_BIN:-../experiments/x86_dynamic_threads/llama/build-x86/bin/llama-server}"
export DT_X86_LIBDIR="${DT_X86_LIBDIR:-../experiments/x86_dynamic_threads/llama/build-x86/bin}"
export DT_X86_MODEL="${DT_X86_MODEL:-../riscv-linux/host-share/Llama-3.2-3B-Instruct-Q4_K_M.gguf}"

# ========================================
# rvv_sched_cost 调度观测（独立工具）
# ========================================
export RVV_TRACE="${RVV_TRACE:-0}"
export RVV_TRACE_EXTRA_SEC="${RVV_TRACE_EXTRA_SEC:-180}"
export RVV_TRACE_SWITCH_OUT="${RVV_TRACE_SWITCH_OUT:-1}"
export RVV_TRACE_SWITCH_REASON="${RVV_TRACE_SWITCH_REASON:-1}"
export RVV_TRACE_RVV_ONLY="${RVV_TRACE_RVV_ONLY:-0}"
export RVV_TRACE_FUTEX="${RVV_TRACE_FUTEX:-0}"
export RVV_REMOTE_DIR="${RVV_REMOTE_DIR:-/tmp/rvv_sched_cost}"
