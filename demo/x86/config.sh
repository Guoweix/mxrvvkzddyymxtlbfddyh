#!/bin/bash
# x86 demo 配置：指向 demo 内的二进制与 runtime（相对 demo 根目录）。
# 唯一需要按环境修改的是模型路径 DT_X86_MODEL。

# 预编译二进制（dt_controller / 探针）
export DEMO_BIN="${DEMO_BIN:-x86/bin}"
# patched llama.cpp runtime（llama-server + 共享库）
export DT_X86_LLAMA_BIN="${DT_X86_LLAMA_BIN:-x86/runtime/llama-server}"
export DT_X86_LIBDIR="${DT_X86_LIBDIR:-x86/runtime}"
# 模型 GGUF（需自备；可改为绝对路径）
export DT_X86_MODEL="${DT_X86_MODEL:-x86/model.gguf}"
