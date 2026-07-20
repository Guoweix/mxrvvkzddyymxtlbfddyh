# RVV LLM Benchmark

`experiments/benchmark` 是单个 llama.cpp 服务的性能测试入口，用于比较 CFS、Linux
task affinity 和当前 `scx_llm_pin` sched_ext 策略。双服务异步注入场景使用独立入口：
[`dual_model/README.md`](dual_model/README.md)。

本脚本默认按 `2,4,8` 三个并发档位运行，每档默认 1 轮。默认启用固定总输出 token
模式：每个并发档位的请求共享 `FIXED_TOTAL_TOKENS=1000`，请求完成后按真实 token
时间计算 TPS、TTFT 和 p95 TTFT。

## 目录结构

```text
experiments/benchmark/
├── benchmark.sh                 # 主 benchmark
├── config.sh                    # 默认配置和 prompt
├── verify.sh                    # 本地依赖、配置和连通性检查
├── generate_report.py           # 旧结果报告生成器
├── compare.sh                   # 结果目录对比
├── run_test.sh                  # 默认套件包装器
├── quick_test.sh                # 30 秒/短 token 快速测试
├── smoke_test.sh                # 已启动服务的短请求测试
├── test_single.sh               # 单请求诊断测试
├── linux_affinity_benchmark.sh  # CFS + task affinity 对照
├── barrier_c4/                  # 历史 futex/barrier 实验目录，当前为空
├── dual_model/                  # 双 llama-server 异步实验
├── phase_c1/                    # 历史 c=1 实验结果，不是当前入口
└── results/                     # 输出结果
```

当前 sched_ext 工具：

```text
experiments/scx_llm_pin/
experiments/rvv_sched_cost/
```

## 环境要求

### 本机必需依赖

`verify.sh` 检查的依赖为：

```text
bash
ssh
scp
sshpass
curl
jq
bc
python3
ping
```

其中 `ssh`、`scp` 和 `ping` 通常由系统 OpenSSH/iputils 软件包提供。`benchmark.sh`
还使用常见 POSIX 工具，例如 `awk`、`grep`、`sed`、`sort`、`timeout` 和 `sha256sum`。

检查本机环境：

```bash
cd /home/gwx/Workdir/riscv-rvv/experiments/benchmark
./verify.sh
```

### 板端必需条件

板端需要满足：

1. SSH 可登录，默认配置为 `root@192.168.3.67`，密码和地址可在 `config.sh` 修改。
2. `LLM_PATH` 指向包含 `run-server.sh` 的 llama 运行目录。
3. `run-server.sh` 能启动目标 CPU llama-server 和配置的 GGUF 模型。
4. llama-server 提供健康检查和 completion HTTP 接口。
5. 板端有 8 个可用 CPU；默认调度 CPU 范围为 `0-7`。
6. 板端内核启用了当前 `scx_llm_pin` 所需的 sched_ext/BPF 支持。

默认配置：

```bash
REMOTE_IP=192.168.3.67
REMOTE_USER=root
REMOTE_PORT=9090
LLM_PATH=/mnt/llm/llama
LLM_THREADS=8
LLM_THREADS_BATCH=       # 留空时跟随 LLM_THREADS
LLM_PARALLEL=4
LLM_CONTEXT=4096
```

### 本地工具和可选依赖

普通 CFS、affinity 和 sched_ext 测试需要：

```text
experiments/scx_llm_pin/scx_llm_pin
experiments/scx_llm_pin/rvv_lease_ctl
```

当 `RVV_TRACE=1` 时，还需要：

```text
experiments/rvv_sched_cost/rvv_sched_cost
jupiter-buildroot/output/k1_v2/target/usr/lib/libelf-0.186.so
```

当使用 `rvv_dirty_lease_user_observe` 或 `rvv_dirty_lease_user_active` 时，需要
用户态 lease hint 版 `libggml-cpu.so`。默认路径是：

```text
kbuild/llama.cpp/build-riscv-hint/bin/libggml-cpu.so.0.13.0
```

上述文件只对对应功能必需，不应作为 CFS 或普通 `rvv_dirty_all` 测试的通用前置条件。


## 快速开始

运行默认 CFS 套件：

```bash
cd /home/gwx/Workdir/riscv-rvv/experiments/benchmark
./benchmark.sh --strategy cfs
```

列出当前可用 strategy：

```bash
./benchmark.sh --list-strategies
```

运行一个并发档位的短测试：

```bash
./benchmark.sh \
  --strategy cfs \
  --concurrency-levels 2 \
  --loops 1 \
  --total-tokens 32
```

## 当前可用策略

所有 sched_ext 策略都由 `experiments/scx_llm_pin/scx_llm_pin` 实现。

| Strategy | 作用 |
|---|---|
| `cfs` | Linux CFS baseline，不加载 sched_ext |
| `linux_affinity` | CFS 下使用 task affinity 固定高 runtime worker |
| `scx_global` | sched_ext global DSQ |
| `pin_all` | LLM 任务保持 CPU 亲和性 |
| `pin_0_3_iso_4_7` | LLM 使用 CPU0-3，系统任务使用 CPU4-7 |
| `pin_4_7_iso_0_3` | LLM 使用 CPU4-7，系统任务使用 CPU0-3 |
| `long_slice` | 全 CPU，长时间片 |
| `local_dsq` | 优先使用当前 CPU 的 local DSQ |
| `local_dsq_50ms` | local DSQ + 50 ms slice |
| `local_dsq_100ms` | local DSQ + 100 ms slice |
| `long_slice_isolated` | LLM CPU0-3 + 长时间片，系统任务 CPU4-7 |
| `llm_only_pin` | 仅 llama-server 线程进入 sched_ext 并 pin |
| `llm_only_local_dsq` | 仅 llama-server 线程使用 local DSQ |
| `llm_only_local_dsq_50ms` | 仅 llama-server 线程使用 local DSQ + 50 ms slice |
| `llm_workers_pin` | 仅高 runtime worker 进入 sched_ext 并 pin |
| `llm_workers_local_dsq` | 仅高 runtime worker 使用 local DSQ |
| `rvv_dirty_all` | 全核心 RVV dirty 识别和 vector DSQ |
| `rvv_dirty_bind_all` | RVV dirty worker 唯一 CPU 分配和 local DSQ |
| `rvv_dirty_lease_all` | RVV dirty worker 唯一 CPU + lease 窗口 |
| `rvv_dirty_lease_unique_all` | `rvv_dirty_lease_all` 兼容别名 |
| `rvv_dirty_lease_user_observe` | dirty lease + 用户 worker 注册，仅观测 |
| `rvv_dirty_lease_user_active` | dirty lease + 用户 worker/CPU 控制 |
| `rvv_dirty_local_all` | RVV dirty vector 长 slice + 辅助线程 local DSQ |
| `rvv_dirty_6_2` | CPU0-5 激进计算核心，CPU6-7 普通/回退核心 |

## 测试场景与策略适用性

不同测试入口代表不同 workload，strategy 不会自动切换测试场景，必须通过对应脚本
显式选择。

| 场景 | 入口 | 请求模型 | 适用策略 | 主要指标 |
|---|---|---|---|---|
| 单 server 通用性能 | `benchmark.sh` | 一个 llama-server，`-np` 个并发请求 | 当前所有可用 strategy，包括 CFS、affinity、`scx_llm_pin` 的各种策略 | TPS、TTFT、p95 TTFT、RVV trace |
| 单 server + Linux affinity | `linux_affinity_benchmark.sh` | 一个 llama-server，固定高 runtime worker 到 CPU | `linux_affinity` | TPS、TTFT、上下文切换、CPU affinity |
| 单 server futex/barrier 诊断 | 历史 `barrier_c4/` | 一个 llama-server，`-np=4`、4 个并发请求 | 当前目录没有可运行 harness；可用普通 `benchmark.sh` + `RVV_TRACE_FUTEX=1` 做近似采集 | futex blocked waits、blocked time |
| 双 server 异步竞争 | `dual_model/run_benchmark.py` | 两个 llama-server，请求分为 incumbent 和 injected，异步重叠 | 当前主要支持 `cfs-shared` 和 `external` phase-aware dirty lease | Aggregate TPS、每任务 TTFT、p95 TTFT、公平性、runqueue wait、上下文切换 |
| 历史单请求 phase 扫描 | `phase_c1/` | 单请求 c=1，固定 prompt/output，扫描 `-t/-tb` | 历史 CFS、lease、barrier-gang 结果；当前无可运行 harness | Prefill、TTFT、Decode TPS、ITL |

典型调用方式：

```bash
# 单 server，测试任意当前可用 strategy
./benchmark.sh --strategy rvv_dirty_lease_all --concurrency-levels 4 --loops 3

# 单 server，Linux affinity 对照
./linux_affinity_benchmark.sh --tasks 4 --loops 3

# 单 server，普通 workload 增加 futex 诊断
RVV_TRACE=1 RVV_TRACE_FUTEX=1 \
  ./benchmark.sh --strategy rvv_dirty_lease_all --concurrency-levels 4

# 双 server 异步场景
cd dual_model
./run.sh --strategy cfs-shared --concurrency 4 --loops 3
```

`dual_model` 的 `external` strategy 需要通过 `--external-start-cmd` 显式提供调度器
启动命令；它不会读取 `benchmark.sh --strategy rvv_dirty_lease_all` 的 strategy preset。
同样，`benchmark.sh` 中的单 server strategy 也不会自动改用双 server workload。

推荐基线和当前主要策略：

```bash
# CFS
./benchmark.sh --strategy cfs --concurrency-levels 2,4,8 --loops 3

# RVV dirty lease
RVV_TRACE=1 ./benchmark.sh \
  --strategy rvv_dirty_lease_all \
  --concurrency-levels 2,4,8 \
  --loops 3 \
  --vector-slice-ms 50 \
  --dirty-threshold 2
```

## 任务和计时

主 benchmark 使用 `config.sh` 中的 4 个固定 prompt。每个并发档位都启动相同数量的
请求：

| 并发档位 | 请求数 |
|---:|---:|
| 2 | 2 |
| 4 | 4 |
| 8 | 8 |

正式请求默认使用：

```text
temperature=0
seed=1234
ignore_eos=true
stream=true
cache_prompt=false
```

固定总 token 模式下，所有请求在统一释放时间附近启动，`FIXED_TOTAL_TOKENS` 按请求数
均分。TPS 使用实际收到的 token 数和真实 wall time 计算，不使用固定的 300 秒作为
TPS 分母。设置 `--total-tokens 0` 才会恢复固定时长模式。

正式测试前会执行 warmup。warmup 不计入正式指标；关闭方式：

```bash
./benchmark.sh --strategy cfs --no-warmup
```

默认 phase gate 会检查并发请求首 token 的相位差。如果超出阈值，脚本会重启服务、
重新 warmup，并在限定次数内重试。

## 常用参数

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `--duration N` | 300 | 固定时长模式的单轮秒数 |
| `--concurrency-levels L` | `2,4,8` | 并发档位列表 |
| `--tasks N` | 兼容参数 | 只测试单个并发档位 |
| `--loops N` | 1 | 每个档位重复次数 |
| `--tokens N` | 100 | 固定时长模式的每请求 token 数 |
| `--total-tokens N` | 1000 | 固定总 token 模式；0 表示固定时长 |
| `--threads N` | 8 | llama.cpp `-t` |
| `--threads-batch N` | 跟随 `-t` | llama.cpp `-tb` |
| `--gomp-spincount N` | 板端默认 | GNU libgomp barrier 自旋次数 |
| `--strategy S` | cfs | 调度策略 |
| `--vector-slice-ms N` | 策略默认 | RVV vector slice 覆盖值 |
| `--vector-local-window-ms N` | 策略默认 | vector local window 覆盖值 |
| `--dirty-threshold N` | 策略默认 | RVV dirty 命中阈值覆盖值 |
| `--warmup-requests N` | 跟随并发 | 正式测试前 warmup 请求数 |
| `--warmup-tokens N` | 1 | 每个 warmup 请求 token 数 |
| `--no-warmup` | - | 关闭正式测试前 warmup |

## RVV trace

默认不启动高开销 RVV trace。需要 RVV save/restore 候选统计时：

```bash
RVV_TRACE=1 ./benchmark.sh --strategy rvv_dirty_all
```

可关闭切出理由采样，保留 save/restore 统计：

```bash
RVV_TRACE=1 RVV_TRACE_SWITCH_REASON=0 \
  ./benchmark.sh --strategy rvv_dirty_all
```

可选 futex 统计：

```bash
RVV_TRACE=1 RVV_TRACE_FUTEX=1 \
  ./benchmark.sh --strategy rvv_dirty_lease_all
```

`RVV_TRACE=1` 时，结果目录会包含 `rvv_sched_cost.log`、`rvv_sched_cost.json` 和
`rvv_trace_info.txt`。不启用 trace 时，RVV 和 futex 细项不会生成。

## 测试流程

每一轮主要执行：

1. 检查板卡和残留进程。
2. 启动或重启 llama-server。
3. 执行 warmup。
4. 加载 `scx_llm_pin`，CFS/affinity 跳过 sched_ext。
5. 可选启动 `rvv_sched_cost`。
6. 运行固定 token 或固定时长 workload。
7. 收集 trace、scheduler log、dmesg 和结果文件。
8. 停止调度器、恢复服务并生成报告。

## 输出文件

单个 suite 通常包含：

```text
results/YYYYMMDD_HHMMSS_<strategy>_suite/
├── suite_config.txt
├── summary.csv
├── summary.txt
└── c<concurrency>_loop<round>/
    ├── config.txt
    ├── metrics.env
    ├── system_info.txt
    ├── scheduler.log
    ├── rvv_sched_cost.json       # RVV_TRACE=1 时生成
    ├── rvv_sched_cost.log        # RVV_TRACE=1 时生成
    ├── task*.csv
    └── report.txt
```

重点指标：

- `TPS`：实际收到 token 数除以真实测试 wall time。
- `TTFT`：请求开始到首个流式 token 的时间。
- `p95TTFT`：请求 TTFT 的 p95。
- `v_save_candidates` / `v_restore_candidates`：RVV trace 中的候选计数。
- `futex_wait_blocked_*`：启用 futex trace 后的 futex 阻塞统计。

比较两个结果目录：

```bash
./compare.sh results/<baseline> results/<candidate>
```

## 独立实验

`barrier_c4/` 曾用于专门比较 CFS 和 dirty lease 的 futex blocked time，但当前目录
没有 harness、README 或可执行脚本，因此现在不能直接运行。历史 barrier 数据和脚本
不属于当前 benchmark 调用链。

`phase_c1/` 保存历史 c=1、`-t/-tb` 扫描和 barrier-gang 实验结果。它没有参与当前
默认测试，也不应与当前 `dual_model` 结果直接合并。

## 故障排查

```bash
ping 192.168.3.67
sshpass -p bianbu ssh root@192.168.3.67 "echo ok"
sshpass -p bianbu ssh root@192.168.3.67 \
  "ps w | grep -E 'llama-server|scx_llm_pin|rvv_sched_cost' | grep -v grep || true"
sshpass -p bianbu ssh root@192.168.3.67 "dmesg | tail -80"
```

如果 dmesg 出现 sched_ext watchdog、scheduler error、runnable task stall 或 OOM，
该轮不应进入性能均值。检查服务日志：

```bash
sshpass -p bianbu ssh root@192.168.3.67 \
  "tail -100 /tmp/llama-server.log"
```
