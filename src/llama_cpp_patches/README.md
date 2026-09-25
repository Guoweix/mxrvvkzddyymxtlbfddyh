# llama.cpp sched_ext hint 补丁

本目录保存当前实验使用的 llama.cpp 用户态调度提示补丁。它不修改模型计算公式
或采样逻辑，而是在 llama-server、llama_context 和 GGML CPU backend 之间传递
调度阶段，并把 OpenMP worker 身份和阶段信息发布到 sched_ext 创建的 mmapable
BPF map。

## 补丁文件

补丁必须按 **`0001` → `0002` → `llama-dynamic.patch`** 的顺序应用，三者是增量
关系而不是可替换关系：`0001`/`0002` 建立并升级 lease-hint 基础接口（最终 ABI v4），
`llama-dynamic.patch` 在 **已打好 `0001`+`0002` 的源码树**上追加“动态线程 /
persistent grant”runtime。它不能单独应用——其 diff 上下文里已经包含
`scx-barrier-hint-client.*`、`scx-rvv-lease-control.h`（ABI v4）。

`0001-llama-publish-sched_ext-worker-and-phase-hints.patch`（git-am 格式）完成以下修改：

- 增加一次性的 `llama_set_sched_hint()` C API，阶段包括 `AUTO`、`IDLE`、
  `PREFILL`、`DECODE` 和 `MIXED`。
- llama-server 根据 continuous-batching 中每个 `batch_view` 的 prompt/decode token
  数量分类，再把提示传给对应的 `llama_decode()`。
- llama_context 将提示传到 GGML CPU backend；每次 graph 结束后恢复 `IDLE`，避免
  过期状态影响后续调度。
- Linux OpenMP backend 注册 worker 的 `TID/TGID/ith`，并发布 graph、node、
  barrier 和 team phase 状态。
- BPF map 不存在、ABI 不匹配或未启用时自动保持 no-op，推理仍可继续运行。

`0002-ggml-support-variable-sched-ext-worker-membership.patch`（git-am 格式）进一步完成：

- ABI v4 为每个模型预留 32 个 worker 信息槽，并支持最多 8 个模型页。
- K1 第一版仍将每个 graph 的实际 worker 数限制为 1–8。
- 每个 graph 发布实际 OpenMP team 宽度、membership epoch 和注册位图。
- 支持 `-t != -tb`；旧 epoch 中的槽位不会被视为当前 graph 成员。
- 单线程 graph 也发布主线程身份，超限配置只禁用该 graph 的 hint 优化。

`llama-dynamic.patch`（纯 unified diff，非 git-am 格式）在 v4 基础上完成：

- 新增 `dynamic-client.c/.h`、`dynamic-worker-team.c/.h`、`dynamic-grant-client.c/.h`
  与 `dynamic-control.h`，把动态线程宽度与 CPU 授权通过 Unix socket 交给用户态
  controller（`src/dynamic_threads`）。
- 修改 `ggml-cpu.c/.cpp`、`include/llama.h`、`src/llama-context.*`、
  `tools/server/server-context.cpp`、`ggml/src/ggml-cpu/CMakeLists.txt`，接入
  persistent grant 的 begin/end/register/unregister 生命周期。
- 必须先应用 `0001`+`0002`，再用 `git apply` 应用本补丁；`scx-rvv-lease-control.h`
  与 `src/scx_llm_pin/scx_rvv_lease_control.h` 保持逐字节一致。

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
export PATCH_DIR=/path/to/src/llama_cpp_patches
```

从准确基线创建独立分支，先按 git-am 顺序应用 `0001`、`0002`，再以纯 diff 方式
应用 `llama-dynamic.patch`：

```bash
git -C "$LLAMA_SRC" status --short
git -C "$LLAMA_SRC" switch -c sched-ext-hints \
    678d43d7205cffc3a8a026cb60b1de3868cc70a2
git -C "$LLAMA_SRC" am --3way \
    "$PATCH_DIR/0001-llama-publish-sched_ext-worker-and-phase-hints.patch" \
    "$PATCH_DIR/0002-ggml-support-variable-sched-ext-worker-membership.patch"
git -C "$LLAMA_SRC" apply --3way "$PATCH_DIR/llama-dynamic.patch"
```

检查最终源码：

```bash
git -C "$LLAMA_SRC" log -2 --oneline
git -C "$LLAMA_SRC" diff --check
```

说明：`llama-dynamic.patch` 是纯 unified diff（无 `From`/`Subject`），所以用
`git apply` 而不是 `git am`；它假定 `0001`/`0002` 已应用，直接对干净上游应用会
因上下文缺失而失败。

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

## 板端部署

benchmark 不会上传完整 llama.cpp、模型或 patched runtime；运行测试前应先把本节
产物部署到板端。不要只替换 `libggml-cpu`：`llama-server`、`libllama`、GGML 和
server 公共库必须来自同一次构建，否则可能出现 SONAME、符号或 C++ ABI 不匹配。
单模型 benchmark 的 lease 准备流程可以临时替换已经匹配的 `libggml-cpu`，但该
机制只用于同版本实验切换，不能代替首次完整 runtime 部署。

当前构建至少需要同步以下文件，并保留 `.so`、SONAME 和完整版本之间的软链接：

```text
llama-server
libllama.so*
libllama-common.so*
libllama-server-impl.so*
libggml.so*
libggml-base.so*
libggml-cpu.so*
libmtmd.so*
```

模型文件不受本 patch 改动，可以继续复用板端已有 GGUF。推荐把 patched runtime
放入独立版本目录，不覆盖默认 `bin/`，以便 CFS/调度实验使用完全相同的二进制，
也便于快速回退：

```text
<LLAMA_REMOTE>/
├── bin/                         # 板端原有 runtime 和动态加载器
├── bin-phase-aware/             # 本次 patched runtime
│   ├── llama-server
│   ├── libllama.so*
│   ├── libllama-common.so*
│   ├── libllama-server-impl.so*
│   ├── libggml.so*
│   ├── libggml-base.so*
│   ├── libggml-cpu.so*
│   └── libmtmd.so*
└── models/
    └── <model>.gguf
```

以下示例从构建目录创建归档。使用 `tar` 而不是逐个 `scp`，可以保留共享库软链接：

```bash
export BOARD="<user>@<board-host>"
export LLAMA_REMOTE="<board-llama-directory>"
export REMOTE_RUNTIME="$LLAMA_REMOTE/bin-phase-aware"
export RUNTIME_ARCHIVE="$(pwd)/llama-riscv-hint-runtime.tar.gz"

(
    cd "$BUILD_DIR/bin"
    tar -czf "$RUNTIME_ARCHIVE" \
        llama-server \
        libllama.so* libllama-common.so* libllama-server-impl.so* \
        libggml.so* libggml-base.so* libggml-cpu.so* libmtmd.so*
)

ssh "$BOARD" "mkdir -p '$REMOTE_RUNTIME'"
scp "$RUNTIME_ARCHIVE" "$BOARD:/tmp/llama-riscv-hint-runtime.tar.gz"
ssh "$BOARD" \
    "tar -xzf /tmp/llama-riscv-hint-runtime.tar.gz -C '$REMOTE_RUNTIME' && \
     rm -f /tmp/llama-riscv-hint-runtime.tar.gz"
```

动态加载器不由 llama.cpp 构建产生。应使用与交叉编译 sysroot ABI 匹配的板端加载
器；本项目的 benchmark 默认从 `<LLAMA_REMOTE>/bin/` 使用它，并把 patched runtime
放在共享库搜索路径首位：

```bash
export REMOTE_LOADER="$LLAMA_REMOTE/bin/ld-linux-riscv64-lp64d.so.1"
export MODEL_PATH="$LLAMA_REMOTE/models/<model>.gguf"

ssh "$BOARD" \
    "test -x '$REMOTE_LOADER' && test -x '$REMOTE_RUNTIME/llama-server' && \
     test -r '$MODEL_PATH'"

ssh "$BOARD" \
    "'$REMOTE_LOADER' --library-path '$REMOTE_RUNTIME:$LLAMA_REMOTE/bin' \
     '$REMOTE_RUNTIME/llama-server' --version"
```

如板端基础系统已经提供兼容加载器，也可以把 `REMOTE_LOADER` 指向系统路径；不要
混用不同 libc ABI。可用 `readelf -d` 检查 `NEEDED` 项，并确认每个依赖都能从
`REMOTE_RUNTIME`、板端默认 runtime 或基础系统解析：

```bash
"${CROSS_COMPILE}readelf" -d "$BUILD_DIR/bin/llama-server" | grep NEEDED
ssh "$BOARD" "find '$REMOTE_RUNTIME' -maxdepth 1 -type l -o -type f"
```

部署后可先在不加载 sched_ext 的情况下验证同一构建产物：

```bash
export SERVER_PORT="<unused-port>"

ssh "$BOARD" \
    "unset GGML_SCX_LEASE_HINT_PATH GGML_SCX_GANG_HINT_PATH; \
     nohup '$REMOTE_LOADER' --library-path '$REMOTE_RUNTIME:$LLAMA_REMOTE/bin' \
       '$REMOTE_RUNTIME/llama-server' -m '$MODEL_PATH' --host 0.0.0.0 \
       --port '$SERVER_PORT' -ngl 0 -t 8 -tb 8 -np 4 --cache-ram 0 \
       >/tmp/llama-server-phase-aware.log 2>&1 </dev/null &"

curl --fail "http://<board-host>:$SERVER_PORT/health"
```

benchmark 通过 `--runtime-bin-dir` 选择该目录，不会再次上传 runtime：

```bash
cd "$PATCH_DIR/../../tests/benchmark"
python3 benchmark.py \
    --scenario dual-sync \
    --strategy cfs \
    --concurrency 4 --loops 1 \
    --runtime-bin-dir "$REMOTE_RUNTIME" \
    --model-a "$MODEL_PATH" --model-b "$MODEL_PATH" \
    --port-a "<model-a-port>" --port-b "<model-b-port>"
```

正式启用 dirty lease 后，再检查 server 日志中的
`GGML_SCX_LEASE_HINT connected`，并使用 `rvv_lease_ctl status` 验证两个 team 的
worker 和 phase。若部署验证失败，停止测试 server，并把 `--runtime-bin-dir` 切回
原 runtime；独立目录部署不需要覆盖或恢复默认库。

## Dirty Lease 接口

当前 `scx_llm_pin` 主路径使用 dirty-lease ABI v4。llama-server 启动时必须把
`GGML_SCX_LEASE_HINT_PATH` 指向调度器 pin 的 `workers` map：

```bash
export GGML_SCX_LEASE_HINT_PATH=/sys/fs/bpf/scx_rvv_dirty_lease/workers
llama-server -m /path/to/model.gguf -t 4 -tb 8 -np 4
```

推荐启动顺序：

1. 带上述环境变量启动 llama-server；此时 map 尚不存在也不会导致启动失败。
2. 取得 server TGID 后启动 `scx_llm_pin --user-control observe` 或 `active`，由
   loader 创建并 pin map。
3. llama.cpp 在后续 graph 开始时重试连接；连接成功后 stderr 会出现
   `GGML_SCX_LEASE_HINT connected`。
4. 使用 `rvv_lease_ctl status` 检查当前 membership 的 worker 数、TGID、team 和
   phase。prefill/decode 使用不同线程数时，注册位图应随 graph 宽度变化。

N 模型场景中，调度器按 TGID 建立最多八页 worker 数据；每个进程会按自身 TGID
选择对应页。每页虽然预留 32 个信息槽，当前 K1 配置仍只接受 1–8 个活跃 worker。
`scx-rvv-lease-control.h` 必须与
`src/scx_llm_pin/scx_rvv_lease_control.h` 保持逐字节一致。

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
- `observe/active` 模式下日志出现 lease map connected，当前 graph 的 worker 注册完整。
- `rvv_lease_ctl status` 中 phase 能在 `PREFILL/DECODE/MIXED/IDLE` 之间变化。
- 调度器退出或 map 失效后推理继续运行，不发生 server crash。
- 调度测试期间没有 sched_ext watchdog、stall、BUG 或 Oops。
