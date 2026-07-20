# RISC-V RVV LLM 调度实验平台

本项目面向 Milk-V Jupiter / SpacemiT K1 八核 RISC-V 平台，研究 Linux
`sched_ext` 对 llama.cpp CPU 推理工作负载的调度优化。项目使用 eBPF 调度器识别
执行 RVV 向量计算的 worker，并结合 CPU 分配、vector DSQ、local lease 以及
llama.cpp 提供的 prefill/decode 阶段提示，改善多线程和多模型竞争时的调度行为。

仓库提供一套可复现的实验环境，包括 K1 Linux 6.6 的 sched_ext 补丁、llama.cpp
用户态提示补丁、RVV-aware eBPF 调度器、低开销观测工具和 benchmark。

## 核心能力

- **K1 sched_ext 内核支持**：将 sched_ext 移植到 K1 厂商 Linux 6.6，并补齐厂商树
  的调度器、cgroup、BTF、libbpf 和驱动构建兼容代码。
- **RVV worker 识别**：根据任务的 RISC-V Vector 上下文状态和可配置 dirty 命中
  阈值识别向量计算线程。
- **Dirty lease 调度**：为 RVV worker 分配互不重复的 CPU，使用 per-CPU vector
  DSQ、长时间片和 local lease 减少不必要的迁移与调度相位扰动。
- **用户态调度提示**：llama.cpp 可通过 mmapable BPF map 注册 OpenMP worker，
  并发布 `PREFILL`、`DECODE`、`MIXED` 和 `IDLE` 阶段。
- **调度观测**：采集 RVV save/restore 候选、上下文切换、wake-to-run 延迟、
  futex wait/wake 和 blocked time。
- **可重复 benchmark**：支持 CFS、Linux affinity 和 sched_ext 策略对比，以及
  单 llama-server、双 llama-server 异步竞争场景。

## 项目结构

| 目录 | 内容 |
|---|---|
| [`kernel_patches/`](kernel_patches/) | K1 Linux 6.6 sched_ext 移植、厂商树适配和可选 RVV lazy restore |
| [`llama_cpp_patches/`](llama_cpp_patches/) | llama.cpp OpenMP worker 注册和 prefill/decode phase hint |
| [`scx_llm_pin/`](scx_llm_pin/) | RVV-aware sched_ext 调度器、用户态 loader 和控制工具 |
| [`rvv_sched_cost/`](rvv_sched_cost/) | RVV、调度延迟和 futex CO-RE eBPF 观测工具 |
| [`benchmark/`](benchmark/) | workload、预热、采样、结果校验和报告生成脚本 |

## 工作方式

```text
patched llama-server
    | worker registration + phase hint
    v
/sys/fs/bpf/scx_rvv_dirty_lease/workers
    |
    v
scx_llm_pin loader ---> sched_ext BPF scheduler ---> Linux runqueues / DSQs
    ^                                                   |
    |                                                   v
rvv_lease_ctl                                  RVV OpenMP workers

rvv_sched_cost independently observes sched_switch, wakeup and futex events.
```

`scx_llm_pin` 可以仅使用内核态 RVV dirty 识别，也可以接收 patched llama.cpp 发布的
worker 和 phase 信息。没有加载 sched_ext 调度器时，系统继续使用 Linux 默认调度器；
同一份 llama-server 二进制也可以用于 CFS 对照实验。

## 环境准备

### 硬件与主机依赖

- Milk-V Jupiter 或其他采用 SpacemiT K1 的八核 RISC-V/RVV 1.0 板卡。
- x86-64 Linux 构建主机。
- Git、Repo、Make、CMake、Python 3、Clang、bpftool 构建依赖、OpenSSH 和
  `sshpass`。
- 支持 `-target bpf` 的 Clang。
- 支持 RVV 1.0 intrinsic 和 OpenMP 的 RISC-V GCC 14 交叉工具链，用于构建
  llama.cpp。

以下变量用于描述源码、输出目录和板卡连接信息：

```bash
export PROJECT_ROOT=/path/to/riscv-rvv
export EXPERIMENTS_ROOT="$PROJECT_ROOT/experiments"
export BIANBU_ROOT=/path/to/jupiter-buildroot
export BOARD_IP=<board-ip>
export BOARD_USER=<board-user>
export BOARD_PASS=<board-password>
```

## 从 Buildroot 构建完整实验环境

这一节给出从原厂 Buildroot 到可运行实验环境的关键步骤。补丁来源、许可证、完整
构建选项和回退方法分别记录在
[`kernel_patches/README.md`](kernel_patches/README.md) 和
[`llama_cpp_patches/README.md`](llama_cpp_patches/README.md)。

### 1. 获取 K1 Buildroot 源码

使用 Milk-V Jupiter 的 `k1-bl-v2.1.y` manifest：

```bash
mkdir -p "$BIANBU_ROOT"
cd "$BIANBU_ROOT"

repo init -u https://github.com/milkv-jupiter/manifests.git \
    -b main -m k1-bl-v2.1.y.xml
repo sync -c -j"$(nproc)"

export KERNEL_SRC="$BIANBU_ROOT/bsp-src/linux-6.6"
export BR_EXTERNAL="$BIANBU_ROOT/buildroot-ext"
export BR_OUT="$BIANBU_ROOT/output/k1_v2"
```

Buildroot 配置使用 `buildroot-ext/configs/spacemit_k1_v2_defconfig`，内核源码位于
`bsp-src/linux-6.6`。

### 2. 为原厂内核添加 sched_ext

Linux 6.6 主线不包含 sched_ext；该调度类在 Linux 6.12 才正式合入主线。本项目
使用 sched-ext 项目在主线化之前维护的非主线 `v6.6.4-scx2` 发布树作为移植源，
再适配 K1 厂商 Linux 6.6 的调度器和 cgroup 接口。

内核补丁以 K1 Linux 提交
`46ef2dae4ac50c7e556137e4cbbdcc024921d9e3` 为准确基线。两个补丁必须按编号顺序
应用：

```bash
git -C "$KERNEL_SRC" switch -c sched-ext-rvv \
    46ef2dae4ac50c7e556137e4cbbdcc024921d9e3

git -C "$KERNEL_SRC" am --3way \
    "$EXPERIMENTS_ROOT/kernel_patches/0001-sched_ext-port-sched_ext-from-scx-v6.6.4-scx2.patch" \
    "$EXPERIMENTS_ROOT/kernel_patches/0002-riscv-complete-sched_ext-integration-and-add-lazy-ve.patch"
```

第一个补丁移植 sched-ext `v6.6.4-scx2` 发布树中的 sched_ext 框架；第二个补丁
完成 K1 厂商树适配，并提供可选的 RVV lazy restore 实验功能。

`k1_defconfig` 已启用 `CONFIG_SCHED_CLASS_EXT` 和 BTF。Dirty lease 调度实验不依赖
RVV lazy restore；需要评估 lazy restore 时再启用：

```bash
"$KERNEL_SRC/scripts/config" \
    --file "$KERNEL_SRC/arch/riscv/configs/k1_defconfig" \
    --enable RISCV_ISA_V_USER_OWNER
```

### 3. 构建内核与系统镜像

配置并执行完整 Buildroot 构建：

```bash
make -C "$BIANBU_ROOT/buildroot" \
    O="$BR_OUT" BR2_EXTERNAL="$BR_EXTERNAL" \
    spacemit_k1_v2_defconfig

make -C "$BR_OUT" -j"$(nproc)"
```

应用新的内核补丁后，可清理内核输出并重新生成镜像：

```bash
make -C "$BR_OUT" linux-dirclean
make -C "$BR_OUT" -j"$(nproc)"
```

主要构建产物：

```text
$BR_OUT/images/Image
$BR_OUT/images/bianbu-linux-k1_v2-sdcard.img
$BR_OUT/build/linux-custom/vmlinux
$BR_OUT/host/bin/riscv64-unknown-linux-gnu-gcc
```

`vmlinux` 需要包含 `.BTF`，用于生成 CO-RE eBPF 程序使用的 `vmlinux.h`。部署
`Image` 时需要同时部署同一次构建生成的内核模块和 rootfs。

### 4. 应用并构建 llama.cpp 补丁

llama.cpp 补丁以官方提交
`678d43d7205cffc3a8a026cb60b1de3868cc70a2` 为准确基线：

```bash
export LLAMA_SRC=/path/to/llama.cpp

git clone https://github.com/ggerganov/llama.cpp.git "$LLAMA_SRC"
git -C "$LLAMA_SRC" switch -c sched-ext-hints \
    678d43d7205cffc3a8a026cb60b1de3868cc70a2

git -C "$LLAMA_SRC" am --3way \
    "$EXPERIMENTS_ROOT/llama_cpp_patches/0001-llama-publish-sched_ext-worker-and-phase-hints.patch"
```

使用 GCC 14、RVV 和 OpenMP 构建 llama-server：

```bash
export RISCV_CC=/path/to/riscv64-linux-gnu-gcc-14
export RISCV_CXX=/path/to/riscv64-linux-gnu-g++-14
export LLAMA_BUILD=/path/to/build/llama-riscv-hint

cmake -S "$LLAMA_SRC" -B "$LLAMA_BUILD" \
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

cmake --build "$LLAMA_BUILD" --target llama-server -j"$(nproc)"
```

部署时需要使用同一次构建生成的 `llama-server`、`libllama`、`libggml` 和
`libggml-cpu`。

### 5. 编译 sched_ext/eBPF 扩展

先在 Buildroot staging 中准备目标端 libbpf、libelf 和 zlib：

```bash
make -C "$BR_OUT" libbpf

export STAGING_DIR="$BR_OUT/host/riscv64-buildroot-linux-gnu/sysroot"
test -f "$STAGING_DIR/usr/lib/libbpf.a"
test -f "$STAGING_DIR/usr/lib/libelf.a"
test -f "$BR_OUT/build/linux-custom/vmlinux"
```

构建过程使用 host Clang 生成 BPF object，使用 host bpftool 生成 skeleton，并使用
Buildroot RISC-V 工具链链接板端 loader。

编译 RVV-aware sched_ext 调度器：

```bash
cd "$EXPERIMENTS_ROOT/scx_llm_pin"
make clean
make -j"$(nproc)" \
    KERNEL_SRC="$KERNEL_SRC" \
    BUILDROOT_OUT="$BR_OUT" \
    VMLINUX_BTF="$BR_OUT/build/linux-custom/vmlinux" \
    CLANG=clang
```

编译调度与 RVV 观测工具：

```bash
cd "$EXPERIMENTS_ROOT/rvv_sched_cost"
make clean
make -j"$(nproc)" \
    KERNEL_SRC="$KERNEL_SRC" \
    BUILDROOT_OUT="$BR_OUT" \
    VMLINUX_BTF="$BR_OUT/build/linux-custom/vmlinux" \
    CLANG=clang
```

更换内核后，需要使用新内核的 `vmlinux` 重新生成 `vmlinux.h`、BPF object 和
skeleton。

### 6. 部署并运行调度器

启动包含 sched_ext 补丁的内核后，将调度器部署到板卡：

```bash
make -C "$EXPERIMENTS_ROOT/scx_llm_pin" deploy \
    BOARD_IP="$BOARD_IP" \
    BOARD_USER="$BOARD_USER" \
    BOARD_PASS="$BOARD_PASS"
```

启动 patched llama-server，并指定 worker/phase hint map：

```bash
export GGML_SCX_LEASE_HINT_PATH=/sys/fs/bpf/scx_rvv_dirty_lease/workers

/path/to/llama-server \
    -m /path/to/model.gguf \
    -t 8 -tb 8 -np 4
```

取得 llama-server 的 TGID 后，以 dirty lease 和用户态控制模式启动调度器：

```bash
/tmp/scx_llm_pin/scx_llm_pin \
    --tgid <llama-server-tgid> \
    --strategy rvv_dirty \
    --vector-slice-ms 100 \
    --vector-local-window-ms 500 \
    --dirty-threshold 2 \
    --user-control active \
    --user-cpu-map auto
```

检查 sched_ext 状态、调度器名称和 worker 注册：

```bash
cat /sys/kernel/sched_ext/state
cat /sys/kernel/sched_ext/root/ops
/tmp/scx_llm_pin/rvv_lease_ctl status
dmesg | tail -80
```

`rvv_lease_ctl status` 应显示完整的 worker TID、OpenMP `ith`、CPU 映射和 phase。
调度器运行期间，内核日志中不应出现 sched_ext watchdog、stall、BUG 或 Oops。

## Benchmark

单 llama-server benchmark 通过统一入口选择 CFS、Linux affinity 或 sched_ext
策略：

```bash
export REMOTE_IP="$BOARD_IP"
export REMOTE_USER="$BOARD_USER"
export REMOTE_PASS="$BOARD_PASS"

cd "$EXPERIMENTS_ROOT/benchmark"
./benchmark.sh --list-strategies
```

先运行短测试确认服务、调度器和采样链路：

```bash
./benchmark.sh \
    --strategy cfs \
    --concurrency-levels 2 \
    --loops 1 \
    --total-tokens 32

./benchmark.sh \
    --strategy rvv_dirty_lease_user_active \
    --concurrency-levels 2 \
    --loops 1 \
    --total-tokens 32
```

正式对比可以增加并发档位、循环次数和 RVV trace：

```bash
RVV_TRACE=1 ./benchmark.sh \
    --strategy rvv_dirty_lease_all \
    --concurrency-levels 2,4,8 \
    --loops 3 \
    --total-tokens 1000
```

双 llama-server 异步请求场景位于 [`benchmark/dual_model/`](benchmark/dual_model/)，
用于评估两个独立模型共享八个 CPU 时的 aggregate TPS、逐任务 TTFT、公平性、
runqueue wait 和上下文切换。测试参数、指标口径和结果文件结构见
[`benchmark/README.md`](benchmark/README.md)。

## 文档

- [`kernel_patches/README.md`](kernel_patches/README.md)：内核补丁来源、准确基线、
  构建、RVV lazy restore 和回退。
- [`llama_cpp_patches/README.md`](llama_cpp_patches/README.md)：llama.cpp hint API、
  ABI、交叉编译和无调度器运行方式。
- [`scx_llm_pin/README.md`](scx_llm_pin/README.md)：调度策略、参数、用户态控制和
  故障排查。
- [`rvv_sched_cost/README.md`](rvv_sched_cost/README.md)：RVV、调度和 futex 指标
  定义。
- [`benchmark/README.md`](benchmark/README.md)：测试场景、预热、指标、有效性
  检查和报告生成。

## 上游项目与许可证

- K1 Buildroot manifest：<https://github.com/milkv-jupiter/manifests>
- sched_ext 内核发布：<https://github.com/sched-ext/scx-kernel-releases>
- llama.cpp：<https://github.com/ggerganov/llama.cpp>

内核补丁遵循 Linux 内核 GPL-2.0-only 许可证。llama.cpp 修改遵循上游 MIT
License，共享 ABI 头文件使用 `GPL-2.0 OR MIT`。`scx_llm_pin` BPF 调度器和 loader
使用 GPL-2.0，`rvv_sched_cost` 各文件的许可证以其 SPDX 标识为准。补丁来源、版权
和许可证边界见对应补丁目录的 README。
