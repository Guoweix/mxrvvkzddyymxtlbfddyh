#!/bin/bash
# RISC-V demo 配置：demo 内的二进制 + runtime（相对 demo 根目录）。
# 板端连接按环境修改。

export REMOTE_IP="${REMOTE_IP:-192.168.3.67}"
export REMOTE_USER="${REMOTE_USER:-root}"
export REMOTE_PASS="${REMOTE_PASS:-bianbu}"

# 预编译二进制（dt_controller / 探针，RISC-V）
export DEMO_BIN="${DEMO_BIN:-riscv/bin}"
# patched llama.cpp runtime 目录（含 llama-server 与共享库，RISC-V）
export DT_LLAMA_BIN="${DT_LLAMA_BIN:-riscv/runtime}"
# 板端模型路径
export DT_RISCV_MODEL="${DT_RISCV_MODEL:-/mnt/llm/llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf}"
