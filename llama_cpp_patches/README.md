# llama.cpp sched_ext hint 补丁

本目录保存当前实验使用的 llama.cpp 用户态调度提示补丁。它不修改模型计算公式
或采样逻辑，而是在 llama-server、llama_context 和 GGML CPU backend 之间传递
调度阶段，并把 OpenMP worker 身份和阶段信息发布到 sched_ext 创建的 mmapable
BPF map。

## 补丁文件

`0001-llama-publish-sched_ext-worker-and-phase-hints.patch` 完成以下修改：

- 增加一次性的 `llama_set_sched_hint()` C API，阶段包括 `AUTO`、`IDLE`、
  `PREFILL`、`DECODE` 和 `MIXED`。
- llama-server 根据 continuous-batching 中每个 `batch_view` 的 prompt/decode token
  数量分类，再把提示传给对应的 `llama_decode()`。
- llama_context 将提示传到 GGML CPU backend；每次 graph 结束后恢复 `IDLE`，避免
  过期状态影响后续调度。
- Linux OpenMP backend 注册 8 个 worker 的 `TID/TGID/ith`，并发布 graph、node、
  barrier 和 team phase 状态。
- BPF map 不存在、ABI 不匹配或未启用时自动保持 no-op，推理仍可继续运行。

该 patch 包含当前 llama.cpp 工作树的全部 11 个相关文件修改，共 897 行新增、
5 行删除。

## 上游来源

- 项目：llama.cpp
- 上游仓库：<https://github.com/ggerganov/llama.cpp>
- 准确基线：[`678d43d7205cffc3a8a026cb60b1de3868cc70a2`](https://github.com/ggerganov/llama.cpp/commit/678d43d7205cffc3a8a026cb60b1de3868cc70a2)
- 基线提交说明：`ci : move more CPU jobs to self-hosted runners (#23715)`
- 应用 patch 后的参考 tree：`f560bb73bc657ec728e9d6a980ea1e20c9c7f378`

llama.cpp 上游持续更新，当前 master 已不再停留在该提交。建议使用上述准确基线；
应用到更新版本时需要人工检查 server batching、`llama_decode()` 和 GGML CPU backend
接口变化，不能只以 patch 无冲突作为兼容性证明。

## 开源许可证

llama.cpp 源码根目录 `LICENSE` 使用 MIT License，版权声明为：

```text
Copyright (c) 2023-2026 The ggml authors
```

本补丁遵循原项目许可证边界：

- 对现有 llama.cpp 文件的修改继续使用 MIT License。
- 新增的 `scx-barrier-hint-client.c/.h` 标记为
  `SPDX-License-Identifier: MIT`。
- `scx-rvv-barrier-hint.h` 和 `scx-rvv-lease-control.h` 需要同时供 MIT 用户态代码
  与 GPL eBPF 调度器使用，因此标记为 `GPL-2.0 OR MIT`；在 llama.cpp 用户态构建
  中可按 MIT 条款使用。
- 分发修改后的 llama.cpp 源码或二进制时，应保留上游 MIT 版权和许可文本，以及
  新增文件中的 SPDX 标识。

## 应用补丁

设置 llama.cpp 源码和本补丁目录：

```bash
export LLAMA_SRC=/path/to/llama.cpp
export PATCH_DIR=/path/to/experiments/llama_cpp_patches
```

从准确基线创建独立分支并应用：

```bash
git -C "$LLAMA_SRC" status --short
git -C "$LLAMA_SRC" switch -c sched-ext-hints \
    678d43d7205cffc3a8a026cb60b1de3868cc70a2
git -C "$LLAMA_SRC" am --3way \
    "$PATCH_DIR/0001-llama-publish-sched_ext-worker-and-phase-hints.patch"
```

检查最终源码：

```bash
git -C "$LLAMA_SRC" log -1 --oneline
git -C "$LLAMA_SRC" diff --check HEAD~1..HEAD
test "$(git -C "$LLAMA_SRC" rev-parse 'HEAD^{tree}')" = \
    f560bb73bc657ec728e9d6a980ea1e20c9c7f378
```

出现冲突时可以修复后继续，或者取消应用：

```bash
git -C "$LLAMA_SRC" am --continue
# 或者
git -C "$LLAMA_SRC" am --abort
```

## RISC-V 构建

该接口在 Linux + OpenMP 构建中生效。其他平台会编译 no-op stub，不访问 BPF map。
K1 RVV 构建需要支持 RVV 1.0 intrinsic 和 OpenMP 的 RISC-V 交叉编译器；本项目
使用 GCC 14 验证。

```bash
export RISCV_CC=/path/to/bin/riscv64-linux-gnu-gcc-14
export RISCV_CXX=/path/to/bin/riscv64-linux-gnu-g++-14
export BUILD_DIR=/path/to/build/llama-riscv-hint

cmake -S "$LLAMA_SRC" -B "$BUILD_DIR" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=riscv64 \
    -DCMAKE_C_COMPILER="$RISCV_CC" \
    -DCMAKE_CXX_COMPILER="$RISCV_CXX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DGGML_OPENMP=ON \
    -DGGML_RVV=ON \
    -DGGML_NATIVE=OFF \
    -DGGML_CPU_KLEIDIAI=OFF \
    -DCMAKE_C_FLAGS="-march=rv64gcv_zfh_zvfh_zba_zicbop -mabi=lp64d -fno-tree-vectorize -fno-tree-loop-vectorize" \
    -DCMAKE_CXX_FLAGS="-march=rv64gcv_zfh_zvfh_zba_zicbop -mabi=lp64d -fno-tree-vectorize -fno-tree-loop-vectorize" \
    -DCMAKE_EXE_LINKER_FLAGS="-latomic"

cmake --build "$BUILD_DIR" --target llama-server -j"$(nproc)"
```

若交叉编译器位于容器中，应把 `LLAMA_SRC` 挂载到容器内，再在容器路径执行相同
CMake 配置。不要复用由另一个绝对源码路径生成的 `CMakeCache.txt`。

部署时应同步匹配版本的 `llama-server`、`libllama`、`libggml` 和
`libggml-cpu`，不能只替换一个 ABI 不匹配的共享库。

## Dirty Lease 接口

当前 `scx_llm_pin` 主路径使用 dirty-lease ABI v3。llama-server 启动时必须把
`GGML_SCX_LEASE_HINT_PATH` 指向调度器 pin 的 `workers` map：

```bash
export GGML_SCX_LEASE_HINT_PATH=/sys/fs/bpf/scx_rvv_dirty_lease/workers
llama-server -m /path/to/model.gguf -t 8 -tb 8 -np 4
```

推荐启动顺序：

1. 带上述环境变量启动 llama-server；此时 map 尚不存在也不会导致启动失败。
2. 取得 server TGID 后启动 `scx_llm_pin --user-control observe` 或 `active`，由
   loader 创建并 pin map。
3. llama.cpp 在后续 graph 开始时重试连接；连接成功后 stderr 会出现
   `GGML_SCX_LEASE_HINT connected`。
4. 使用 `rvv_lease_ctl status` 检查 8 个 worker、TGID、team 和 phase。

双 llama-server 场景中，调度器为两个 TGID 建立两页 worker 数据；每个进程会按
自身 TGID 选择对应页。`scx-rvv-lease-control.h` 必须与
`experiments/scx_llm_pin/scx_rvv_lease_control.h` 保持逐字节一致。

## Barrier Hint 兼容接口

patch 还保留早期 barrier-gang 观测使用的 `GGML_SCX_GANG_HINT_PATH` 接口。只有
显式设置该环境变量时，llama.cpp 才会连接对应 map 并发布逐 barrier 状态。

当前精简后的 `scx_llm_pin` 不创建这张旧 map，dirty lease 和 phase-aware 策略也
不依赖它。因此正常测试应保持 `GGML_SCX_GANG_HINT_PATH` 未设置；此时该路径不会
执行 BPF map open 或 mmap。

## 无调度器运行

不设置以下两个环境变量即可关闭全部 mmap hint：

```bash
unset GGML_SCX_LEASE_HINT_PATH
unset GGML_SCX_GANG_HINT_PATH
```

公共 `llama_set_sched_hint()` API 和 server phase 分类仍会执行，但 CPU backend 不会
访问 BPF map。这样可以使用同一个构建产物运行 CFS 对照，避免为每种调度策略维护
不同的 llama-server 二进制。

## 验证清单

- patch 能从准确基线通过 `git am --3way` 应用，最终 tree 与参考值一致。
- `cmake --build ... --target llama-server` 完成，OpenMP 与 RVV 均启用。
- llama-server 在不设置 hint 环境变量时能正常完成请求。
- `observe/active` 模式下日志出现 lease map connected，8 个 worker 注册完整。
- `rvv_lease_ctl status` 中 phase 能在 `PREFILL/DECODE/MIXED/IDLE` 之间变化。
- 调度器退出或 map 失效后推理继续运行，不发生 server crash。
- 调度测试期间没有 sched_ext watchdog、stall、BUG 或 Oops。
