# INFERSCHED：面向 RISC-V 的 LLM 推理调度器

**INFERSCHED**（Inference Scheduler）是一套面向 RISC-V 平台的 Linux 调度方案：
基于 `sched_ext`（eBPF）与一个用户态控制器，**指导并编排共置的多个 LLM 推理
进程**（llama.cpp / llama-server）在八核 CPU 上的线程数与核心分配，改善多实例
竞争时的首 token 延迟（TTFT）、解码吞吐（TPS）与尾延迟。

仓库提供一套可复现的实验环境：K1 Linux 6.6 的 sched_ext 补丁、llama.cpp 运行时
补丁、独立的 sched_ext 调度器与用户态控制器，以及 benchmark 与门禁脚本。

## 核心能力

- **K1 sched_ext 内核支持**：将 sched_ext 移植到 K1 厂商 Linux 6.6，并补齐厂商树
  的调度器、cgroup、BTF、libbpf 和驱动构建兼容代码。
- **动态线程与 CPU 授权**：控制器按需为处于活跃期的推理进程授予 worker 宽度与
  CPU 掩码，并通过独立 sched_ext 程序把成员线程放置到授权核心。
- **持续授权（persistent grant）**：稳定期跨 graph 持续持有授权，仅在分配变化时
  协调，避免逐 graph 反复握手；支持异步 GRANT/REVOKE 与安全交接。
- **阶段感知分配**：prefill 优先、decode 保底核心，按 `PREFILL/DECODE/MIXED`
  相位在 graph 边界安全交接。
- **任务身份校验**：以 `start_boottime` 校验成员身份，避免 worker TID 复用导致的
  误放置。
- **多进程共置**：支持最多 8 个独立 llama-server 同时驻留与调度。
- **可重复 benchmark 与门禁**：`run_benchmark` / `run_control_gate` /
  `run_multi_benchmark` 覆盖 CFS、动态授权与多进程场景。

## 项目结构

| 目录 | 内容 |
|---|---|
| [`src/kernel_patches/`](src/kernel_patches/) | K1 Linux 6.6 sched_ext 移植、厂商树适配和可选 RVV lazy restore |
| [`src/llama_cpp_patches/`](src/llama_cpp_patches/) | llama.cpp 补丁链：lease-hint 基础（0001/0002）与动态线程/persistent grant runtime |
| [`src/dynamic_threads/`](src/dynamic_threads/) | 主调度器：sched_ext 策略、用户态控制器、CPU 分配器与 worker 生命周期 |
| [`src/rvv_sched_cost/`](src/rvv_sched_cost/) | RVV、调度延迟和 futex CO-RE eBPF 观测工具 |
| [`tests/dynamic_threads/`](tests/dynamic_threads/) | 主测试：benchmark、控制/llama 门禁、多进程测试、单元测试与共享模块 |
| [`tests/config.sh`](tests/config.sh) | 唯一配置入口（板端连接、工具链根、llama.cpp 路径） |

## 工作方式

<p align="center">
  <a href="docs/images/architecture.svg">
    <img src="docs/images/architecture.svg" width="1200" alt="RVV-aware sched_ext 用户态与 Linux 内核通信架构">
  </a>
</p>

主调度器 `src/dynamic_threads` 由一个用户态控制器 `dt_controller` 与一段独立的
sched_ext eBPF 程序（`control.bpf.c`）组成。`dt_controller` 通过 libbpf 加载并附加
struct_ops，同时在 Unix domain socket（`SOCK_SEQPACKET`）上接受来自 patched
llama-server 的连接；运行时用 `GGML_DYNAMIC_THREADS_SOCKET` 指定该 socket，在
graph 边界请求一次授权（worker 宽度与 CPU 掩码）。控制器仲裁后写回 grant，运行时
把实际的 OpenMP worker TID 上报（`MEMBERS`），控制器写入 `members` BPF map。

内核调度核心调用 eBPF 的 `select_cpu`、`enqueue`、`dispatch` 和 `running` callbacks。
`enqueue` 时按 TID 查 `members`→`grants` 两张表：命中则把任务投递到目标 CPU 的
per-CPU DSQ，否则落入 global DSQ；`running` 回调事后校验实际运行核。eBPF 仅做
放置，策略与仲裁全部留在用户态控制器。

### Phase-Aware Dirty Lease

<p align="center">
  <a href="docs/images/phase-aware-lease.svg">
    <img src="docs/images/phase-aware-lease.svg" width="1200" alt="Phase-aware lease 中 CPU i 的 DSQ 消费顺序与 vector worker 入队路径">
  </a>
</p>

当前策略在“持续授权（persistent grant）”之上做相位感知分配。控制器为每个处于
活跃期的推理进程维持一份跨 graph 复用的 grant，并在实例之间划分 CPU：默认
prefill 优先、decode 保底至少 1 核，避免解码被 prefill 长时间饿死。相位
（`PREFILL/DECODE/MIXED`）在 graph 边界上报，控制器在边界处安全交接——先撤销不再
需要的核心，再授予新的核心，期间保持成员身份一致。

授权生命周期为 `ACTIVITY → ACQUIRE →（运行时设置 affinity）→ MEMBERS → 计算 →
UNMEMBERS →（恢复 affinity）→ RELEASE`。稳定期不逐 graph 重复握手，仅在分配变化
时触发异步 `GRANT/REVOKE`。eBPF 侧以任务的 `start_boottime` 校验成员身份，防止
worker 线程销毁/重建后数值 TID 复用导致的误放置。

下图展示 per-CPU DSQ 的消费与入队路径：受管 worker 只从自己目标 CPU 的 DSQ 运行；
local 为空时，普通任务仍由 global DSQ 提供服务，保证非推理任务不被饿死。

### 实验数据对比

以下正式 campaign 使用两个 llama-server，各 8 个 worker 共享 CPU0-7；prompt 均
构造为 128 input token，每请求固定输出 256 token。异步场景下 A 侧 incumbent
请求到达 token-4 后，按 1 秒间隔向 B 侧注入等量请求。

c=2/4/8 各重复 3 个有效轮次。Aggregate TPS 为跨轮 mean ± sample SD；injected
TTFT 的 mean、sample SD 与 nearest-rank p95 直接从各轮 TTFT 样本汇总（incumbent
的 TTFT 很小，p95 实际由 injected 决定）。ITL p95 为逐 token 间隔的 p95（跨轮
mean，越小越好）；Token-rate Jain 为两模型 token 速率的 Jain 公平指数（1 表示
完全公平）。策略分别为 Linux 默认 CFS、动态授权（equal 分配）与动态授权 +
相位感知（prefill 优先、decode 保底）。

#### c=2

| 策略 | Aggregate TPS (mean +/- SD) | Injected TTFT (mean +/- SD) | Injected TTFT p95 | ITL p95 | Token-rate Jain |
|---|---:|---:|---:|---:|---:|
| CFS | 1.5878 +/- 0.0142 | 22.495 +/- 0.317 s | 22.495 s | 1435.9 ms | 0.9993 |
| 动态授权（equal） | 2.7472 +/- 0.0007 (+73.0%) | 26.594 +/- 0.052 s (+18.2%) | 26.594 s (+18.2%) | 718.0 ms | 0.9900 |
| 动态授权 + 相位感知 | 2.7705 +/- 0.0102 (+74.5%) | 13.015 +/- 0.039 s (-42.1%) | 13.015 s (-42.1%) | 717.7 ms | 0.9999 |

#### c=4

| 策略 | Aggregate TPS (mean +/- SD) | Injected TTFT (mean +/- SD) | Injected TTFT p95 | ITL p95 | Token-rate Jain |
|---|---:|---:|---:|---:|---:|
| CFS | 1.7621 +/- 0.0715 | 33.023 +/- 0.272 s | 44.169 s | 2823.9 ms | 0.9994 |
| 动态授权（equal） | 2.8515 +/- 0.0007 (+61.8%) | 39.551 +/- 0.045 s (+19.8%) | 52.769 s (+19.5%) | 1331.4 ms | 0.9917 |
| 动态授权 + 相位感知 | 2.7990 +/- 0.0006 (+58.8%) | 19.337 +/- 0.040 s (-41.4%) | 25.601 s (-42.0%) | 1338.9 ms | 1.0000 |

#### c=8

| 策略 | Aggregate TPS (mean +/- SD) | Injected TTFT (mean +/- SD) | Injected TTFT p95 | ITL p95 | Token-rate Jain |
|---|---:|---:|---:|---:|---:|
| CFS | 3.2988 +/- 0.0109 | 73.076 +/- 0.171 s | 90.610 s | 2550.5 ms | 0.9960 |
| 动态授权（equal） | 4.3140 +/- 0.0040 (+30.8%) | 80.528 +/- 0.080 s (+10.2%) | 99.918 s (+10.3%) | 1619.8 ms | 0.9779 |
| 动态授权 + 相位感知 | 4.3023 +/- 0.0020 (+30.4%) | 41.125 +/- 0.037 s (-43.7%) | 51.380 s (-43.3%) | 1638.7 ms | 1.0000 |

`equal` 指动态授权按实例均分 CPU、不做相位优先；相位感知策略在 prefill 阶段优先
分配 CPU，并为 decode 保底至少 1 核。相对 CFS，相位感知策略在 c=2/4/8 下分别
提升 Aggregate TPS 约 74.5%、58.8% 和 30.4%，将 injected TTFT p95 分别降低约
42.1%、42.0% 和 43.3%，并把 ITL p95 从 1435.9 / 2823.9 / 2550.5 ms 降到约
717.7 / 1338.9 / 1638.7 ms（约减半）。仅做 equal 分配虽同样提升 TPS 与降低 ITL，
却会抬高 injected TTFT（c=2/4/8 分别 +18.2%、+19.5%、+10.3%），且其 token-rate
Jain（0.978–0.992）略低于 CFS；相位感知则三者兼顾：TTFT 大幅下降、ITL 减半、
Jain 接近 1.000，说明相位感知是同时改善吞吐、延迟与公平性的关键。

## 环境准备

### 硬件与主机依赖

- Milk-V Jupiter 或其他采用 SpacemiT K1 的八核 RISC-V/RVV 1.0 板卡，运行包含
  sched_ext 的内核（见下文内核补丁）。
- x86-64 Linux 构建主机。
- Git、Repo、Make、CMake、Python 3、OpenSSH 与 `sshpass`。
- 支持 `-target bpf` 的 Clang，以及 host `bpftool`（随 Buildroot 构建）。
- 支持 RVV 1.0 intrinsic 和 OpenMP 的 RISC-V GCC 交叉工具链，用于构建 llama.cpp。
- 板端需已挂载 bpffs：`mount -t bpf bpf /sys/fs/bpf`（重启后需重新挂载）。

所有路径与板端连接信息集中在 [`tests/config.sh`](tests/config.sh)（相对路径以
仓库根目录为基准）：

```bash
# tests/config.sh 中按环境修改
TOOLCHAIN_ROOT=..                 # 含 jupiter-buildroot 的目录（相对仓库根）
LLAMA_CPU_SRC=../...              # 已打补丁的 llama.cpp ggml-cpu 目录
DT_LLAMA_BIN=../...               # 该 llama.cpp 构建出的 runtime bin
REMOTE_IP=192.168.3.67            # 板端 IP / 用户 / 密码
REMOTE_USER=root
REMOTE_PASS=bianbu
```

Makefile 与测试脚本都从该文件读取配置，通常无需再设置环境变量。

## 从 Buildroot 构建完整实验环境

这一节给出从原厂 Buildroot 到可运行实验环境的关键步骤（命令均在仓库根目录执行）。
补丁来源、许可证、完整构建选项和回退方法分别记录在
[`src/kernel_patches/README.md`](src/kernel_patches/README.md) 和
[`src/llama_cpp_patches/README.md`](src/llama_cpp_patches/README.md)。

### 1. 获取 K1 Buildroot 源码

使用 Milk-V Jupiter 的 `k1-bl-v2.1.y` manifest：

```bash
export BIANBU_ROOT=/path/to/jupiter-buildroot
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
    src/kernel_patches/0001-sched_ext-port-sched_ext-from-scx-v6.6.4-scx2.patch \
    src/kernel_patches/0002-riscv-complete-sched_ext-integration-and-add-lazy-ve.patch
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
`678d43d7205cffc3a8a026cb60b1de3868cc70a2` 为准确基线，并按
`0001 → 0002 → llama-dynamic.patch` 顺序应用：`0001`/`0002` 建立并升级
lease-hint 基础接口（最终 ABI v4），`llama-dynamic.patch` 在其上追加动态线程 /
persistent grant runtime（`GGML_DYNAMIC_THREADS_SOCKET`）。

```bash
export LLAMA_SRC=/path/to/llama.cpp

git clone https://github.com/ggerganov/llama.cpp.git "$LLAMA_SRC"
git -C "$LLAMA_SRC" switch -c sched-ext-hints \
    678d43d7205cffc3a8a026cb60b1de3868cc70a2

git -C "$LLAMA_SRC" am --3way \
    src/llama_cpp_patches/0001-llama-publish-sched_ext-worker-and-phase-hints.patch \
    src/llama_cpp_patches/0002-ggml-support-variable-sched-ext-worker-membership.patch
git -C "$LLAMA_SRC" apply --3way src/llama_cpp_patches/llama-dynamic.patch
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

构建过程使用 host Clang 生成 BPF object、host bpftool 生成 skeleton，并使用
Buildroot RISC-V 工具链链接板端控制器与探针。工具链根与 patched llama.cpp 目录
由 [`tests/config.sh`](tests/config.sh) 提供（`TOOLCHAIN_ROOT` / `LLAMA_CPU_SRC`）。

编译主调度器（`make` 会自动从板端抓取 BTF 生成 `vmlinux.h`）：

```bash
make -C src/dynamic_threads all     # dt_controller 与探针
make -C src/dynamic_threads host    # host 端单元测试二进制
```

编译调度与 RVV 观测工具：

```bash
make -C src/rvv_sched_cost all
```

如需离线构建，可先把板端 `/sys/kernel/btf/vmlinux` 放到
`src/dynamic_threads/build/vmlinux.btf` 再执行 `make`。更换内核后需用新内核的
BTF 重新生成 `vmlinux.h`、BPF object 与 skeleton。

### 6. 部署并运行调度器

启动包含 sched_ext 补丁的内核后，运行主测试脚本即可自动完成“构建 → 上传
`dt_controller`/探针 → 启动控制器 → 运行 OpenMP 探针 → 校验 → 清理”：

```bash
cd tests/dynamic_threads
python3 run_control_gate.py \
    --grant-lifetime persistent \
    --allocation-policy prefill-first \
    --decode-wait-ms 0 --decode-floor-on-prefill \
    --iterations 200
```

控制器 `dt_controller` 通过 `--socket <path>` 监听 Unix socket，以 `--mode` 选择
`dynamic`/`fixed4`/`split4` 等策略，以 `--grant-lifetime graph|persistent` 选择授权
是否跨 graph 复用。运行时（patched llama-server）用
`GGML_DYNAMIC_THREADS_SOCKET=<path>` 连接该 socket，并在 graph 边界请求授权。

检查 sched_ext 状态与内核日志：

```bash
cat /sys/kernel/debug/sched/ext
dmesg | tail -80
```

调度器运行期间，内核日志中不应出现 sched_ext watchdog、stall、BUG 或 Oops；
门禁脚本会校验 `wrong_cpu=0`、授权不重叠、成员身份与内核干净。

## Benchmark

主 benchmark 位于
[`tests/dynamic_threads/run_benchmark.py`](tests/dynamic_threads/run_benchmark.py)，
以动态授权调度器为主体，按 case × scenario × concurrency × loop 组织实验，并自动
上传 patched runtime 与 `dt_controller`、执行预热、采集指标、校验内核日志。配置
统一来自 [`tests/config.sh`](tests/config.sh)，通常无需再设环境变量。

先跑一次小规模短测确认链路：

```bash
cd tests/dynamic_threads
python3 run_benchmark.py \
    --cases cfs_baseline,scx_dynamic_persistent_phase \
    --scenarios asynchronous \
    --concurrencies 2 --loops 1 --output-tokens 32
```

可用 case 包括 `cfs_baseline`、`scx_dynamic`、`scx_dynamic_persistent`、
`scx_dynamic_persistent_phase` 以及 `..._reserve1` 等相位感知变体。多进程场景用
[`run_multi_benchmark.py`](tests/dynamic_threads/run_multi_benchmark.py)（1–8 个
独立 llama-server），控制面门禁用
[`run_control_gate.py`](tests/dynamic_threads/run_control_gate.py)。指标口径与结果
文件结构见 [`docs/dynamic_threads/README.md`](docs/dynamic_threads/README.md)。

### 双模型 c=4 正式对比

以下样例复现“实验数据对比”表中的 `c=4` 工作负载：两个 llama-server 各 8 个
worker 共享 CPU0-7，A/B 各承载 2 个请求，A 侧请求输出第 4 个 token 后注入 B 侧
请求，prompt 均为 128 input token，每请求固定输出 256 token。板端路径已由
`tests/config.sh` 提供，直接运行：

```bash
cd tests/dynamic_threads

# Linux 默认 CFS/EEVDF
python3 run_benchmark.py \
    --cases cfs_baseline \
    --scenarios asynchronous \
    --concurrencies 4 --loops 3 \
    --output-tokens 256 --injection-token 4

# 动态授权 + 相位感知（prefill 优先、decode 保底）
python3 run_benchmark.py \
    --cases scx_dynamic_persistent_phase \
    --scenarios asynchronous \
    --concurrencies 4 --loops 3 \
    --output-tokens 256 --injection-token 4
```

每次运行自动执行请求完整性、注入顺序、频率、温度、内核日志与服务存活检查；
相位感知 case 还会校验授权不重叠、成员身份（`start_boottime`）、`wrong_cpu=0`
与相位交接。运行结束后可在结果目录的 `campaign.json` 查看各轮 TPS、injected
TTFT、p95 与门禁结果。

## 文档

- [`src/kernel_patches/README.md`](src/kernel_patches/README.md)：内核补丁来源、准确
  基线、构建、RVV lazy restore 和回退。
- [`src/llama_cpp_patches/README.md`](src/llama_cpp_patches/README.md)：llama.cpp
  补丁链（0001 → 0002 → llama-dynamic）、ABI、交叉编译与无调度器运行方式。
- [`docs/dynamic_threads/README.md`](docs/dynamic_threads/README.md)：主调度器的策略、
  参数、用户态控制器与故障排查。
- [`src/rvv_sched_cost/README.md`](src/rvv_sched_cost/README.md)：RVV、调度和 futex
  指标定义。
- [`tests/dynamic_threads/`](tests/dynamic_threads/)：主 benchmark、控制/llama 门禁、
  多进程测试与单元测试；配置见 [`tests/config.sh`](tests/config.sh)。

## 上游项目与许可证

- K1 Buildroot manifest：<https://github.com/milkv-jupiter/manifests>
- sched_ext 内核发布：<https://github.com/sched-ext/scx-kernel-releases>
- llama.cpp：<https://github.com/ggerganov/llama.cpp>

内核补丁遵循 Linux 内核 GPL-2.0-only 许可证。llama.cpp 修改遵循上游 MIT
License，共享 ABI 头文件使用 `GPL-2.0 OR MIT`。`dynamic_threads` 的 eBPF 调度器
（`control.bpf.c`）使用 GPL-2.0，用户态控制器与 `rvv_sched_cost` 各文件的许可证以
其 SPDX 标识为准。补丁来源、版权和许可证边界见对应补丁目录的 README。
