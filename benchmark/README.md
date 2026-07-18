# RVV LLM Benchmark

`experiments/benchmark` 是当前项目的 LLM 推理基准测试入口，用于比较 CFS、sched_ext 调度策略和 RVV dirty 识别策略对 llama.cpp 推理性能的影响。

双 llama-server、异步请求注入场景使用独立入口 [`dual_model/README.md`](dual_model/README.md)。它不会改变本脚本的单实例测试行为。

测试脚本会自动重启板端 `llama-server`，完成正式测试前预热，启动可选 sched_ext 调度器和 RVV 上下文切换观测工具。当前默认套件：每个并发档位单轮 300 秒，按 `2,4,8` 并发请求分别运行，最终汇总 TPS、平均 TTFT 和 p95 TTFT。

## 目录结构

```text
experiments/benchmark/
├── benchmark.sh          # 主测试脚本
├── config.sh             # 默认配置
├── generate_report.py    # 报告生成器
├── compare.sh            # 两个结果目录的简单对比
├── verify.sh             # 本地依赖、配置和连通性检查
├── run_test.sh           # 默认测试套件包装脚本
├── linux_affinity_benchmark.sh # Linux task affinity worker pin 对照
├── quick_test.sh         # 30s 快速测试
├── smoke_test.sh         # 已启动 server 的短请求 smoke test
├── test_single.sh        # 单并发诊断请求
└── results/              # 测试输出
```

相关 sched_ext / eBPF 工具在同级实验目录中：

```text
experiments/scx_llm_pin/
experiments/scx_rvv_exclusive/
experiments/rvv_sched_cost/
experiments/ggml_barrier_count/
```

## 环境要求

本机需要：

```bash
sshpass curl jq bc python3
```

板端默认配置在 `config.sh`：

```bash
REMOTE_IP=192.168.3.67
REMOTE_USER=root
REMOTE_PASS=bianbu
REMOTE_PORT=9090
LLM_THREADS=8
LLM_THREADS_BATCH=        # 留空时跟随 LLM_THREADS
LLM_PARALLEL=4
LLM_CONTEXT=4096
LLM_PATH=/mnt/llm/llama
```

默认模型路径由脚本启动命令使用：

```text
/mnt/llm/llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf
```

默认 benchmark 使用固定总输出模式：并发请求同步发出，并将 `FIXED_TOTAL_TOKENS=1000` 均分到各并发槽位。计时从统一释放点到最后一个 token，另外报告从第一个输出 token 到最后一个 token 的 decode TPS。设置 `--total-tokens 0` 可恢复旧的固定时长连续请求模式。

先检查环境：

```bash
cd /home/gwx/Workdir/riscv-rvv/experiments/benchmark
./verify.sh
```

## 快速开始

运行默认 CFS baseline 套件：

```bash
cd /home/gwx/Workdir/riscv-rvv/experiments/benchmark
./benchmark.sh --strategy cfs
```

运行单个并发档位的 30 秒快速测试：

```bash
./benchmark.sh --strategy cfs --duration 30 --tasks 2 --tokens 16
```

查看可用策略：

```bash
./benchmark.sh --list-strategies
```

查看脚本参数：

```bash
./benchmark.sh --help
```

## 常用命令

运行 RVV dirty 全核心策略，并固定 vector slice 为 50ms：

```bash
RVV_TRACE=1 ./benchmark.sh \
  --strategy rvv_dirty_all \
  --concurrency-levels 2,4,8 \
  --loops 3 \
  --vector-slice-ms 50 \
  --dirty-threshold 2
```

运行 local DSQ 对照：

```bash
./benchmark.sh --strategy local_dsq_50ms
```

运行不加载 sched_ext 的 Linux worker affinity 对照：

```bash
./linux_affinity_benchmark.sh
```

指定测试时长、并发档位、重复次数和 llama 线程数：

```bash
./benchmark.sh --strategy cfs --duration 300 --concurrency-levels 2,4,8 --loops 3 --threads 8
```

分别设置 generation 单 token 线程数 `-t` 和 prompt/multi-token batch 线程数 `-tb`：

```bash
./benchmark.sh --strategy cfs --tasks 1 --threads 6 --threads-batch 8
```

未指定 `--threads-batch` 时，`-tb` 自动跟随 `--threads`，保持 llama.cpp 默认配置语义。

关闭正式测试前预热：

```bash
./benchmark.sh --strategy rvv_dirty_all --no-warmup
```

比较两个结果目录：

```bash
./compare.sh results/baseline_cfs results/rvv_dirty_all
```

## 命令行参数

| 参数 | 说明 |
| --- | --- |
| `--duration N` | 单轮测试窗口秒数，默认 `300` |
| `--tasks N` | 只测试一个并发档位，兼容旧参数 |
| `--concurrency-levels L` | 并发档位列表，默认 `2,4,8` |
| `--loops N` | 每个并发档位重复轮数，默认 `1` |
| `--tokens N` | 每个请求生成 token 数，默认 `100` |
| `--threads N` | llama.cpp 计算线程数，默认 `8` |
| `--strategy S` | benchmark 策略，默认 `cfs` |
| `--vector-slice-ms N` | 覆盖 RVV dirty vector 时间片 |
| `--vector-local-window-ms N` | 覆盖 RVV dirty vector-local 快速窗口 |
| `--dirty-threshold N` | 覆盖 RVV dirty 命中阈值 |
| `--warmup-requests N` | 正式测试前预热请求数，默认跟随并发数 |
| `--warmup-tokens N` | 每个预热请求生成 token 数，默认 `1` |
| `--warmup-retries N` | 预热失败重试次数，默认 `3` |
| `--no-warmup` | 跳过正式测试前预热 |
| `--list-strategies` | 列出可用策略 |
| `--help` | 显示帮助 |

旧 benchmark 默认让所有并发 worker 在同一个绝对时间点释放首个正式请求，以减少启动相位差。可通过环境变量调整：

```bash
SYNC_CONCURRENT_START=1 SYNC_START_LEAD_SEC=3 SYNC_START_SPIN_MS=2
```

只对齐首个请求；之后每个 worker 独立连续发送请求，不在每轮请求之间增加人为同步。

默认还会在正式计时前执行相位门禁：用同样的并发数同步发出探测请求，比较各请求首 token 时间；若 spread 超过 `PHASE_GATE_THRESHOLD_MS`，当前 server/scheduler 会被重启并重新预热，最多重试 `PHASE_GATE_MAX_RETRIES` 次。相关配置为：

```bash
PHASE_GATE_ENABLED=1 PHASE_GATE_THRESHOLD_MS=100 \
PHASE_GATE_MAX_RETRIES=5 PHASE_GATE_TOKENS=1
```

每轮会保存 `phase_gate_attempt*.json` 和最终的 `phase_gate.json`。只有通过门禁的 server 状态才进入正式统计。

## 策略说明

| 策略 | 说明 |
| --- | --- |
| `cfs` | 默认 CFS baseline，不加载 sched_ext |
| `linux_affinity` | CFS + Linux task affinity worker pin，不加载 sched_ext |
| `scx_global` | 最小 sched_ext 全局队列 |
| `pin_all` | LLM 全 CPU 亲和，保持原 CPU |
| `pin_0_3_iso_4_7` | LLM CPU0-3，系统任务 CPU4-7 |
| `pin_4_7_iso_0_3` | LLM CPU4-7，系统任务 CPU0-3 |
| `long_slice` | LLM 全 CPU，100ms 长时间片 |
| `local_dsq` | LLM 优先当前 CPU 本地 DSQ |
| `local_dsq_50ms` | LLM 本地 DSQ + 50ms 时间片 |
| `local_dsq_100ms` | LLM 本地 DSQ + 100ms 时间片 |
| `long_slice_isolated` | LLM CPU0-3 + 100ms 时间片，系统任务 CPU4-7 |
| `llm_only_pin` | 仅 llama-server 线程进入 SCHED_EXT + pin |
| `llm_only_local_dsq` | 仅 llama-server 线程进入 SCHED_EXT + local DSQ |
| `llm_only_local_dsq_50ms` | 仅 llama-server 线程进入 SCHED_EXT + local DSQ + 50ms |
| `llm_workers_pin` | 仅运行时间最高的计算 worker 进入 SCHED_EXT + pin |
| `llm_workers_local_dsq` | 仅运行时间最高的计算 worker 进入 SCHED_EXT + local DSQ |
| `rvv_dirty_all` | 旧版全核心 RVV Dirty，vector 每次 wakeup 进入 vector DSQ |
| `rvv_dirty_bind_all` | 仅保留 RVV Dirty 识别和 8 个 vector worker 一核一个；vector 使用默认 slice 并直接进入所选 CPU local DSQ |
| `rvv_dirty_lease_all` | 500ms lease，并为 8 个 vector worker 分配互不重复的 CPU |
| `rvv_dirty_lease_unique_all` | `rvv_dirty_lease_all` 的兼容别名 |
| `rvv_dirty_lease_user_observe` | dirty lease 快路径不变；llama.cpp 注册 8 个 worker，内核只观测 |
| `rvv_dirty_lease_user_active` | dirty lease 快路径不变；默认沿用自然唯一分配，可运行时更新 lease 参数并按需启用 `ith -> CPU` 映射 |
| `rvv_dirty_local_all` | RVV Dirty vector 长时间片 + LLM 辅助线程本地 DSQ |
| `rvv_dirty_6_2` | CPU0-5 激进计算核心，CPU6-7 普通/回退核心 |
| `rvv_exclusive_6_2` | CPU2-7 RVV 专用核心，CPU0-1 普通核心 |
| `rvv_lpedf_all` | 全核心 RVV Dirty + Limited-Preemption EDF vtime DSQ |
| `rvv_barrier_gang_active` | llama.cpp 发布 barrier epoch/worker 状态，sched_ext 对 7/8 laggard 和 8/8 release 做有界 boost |

`rvv_dirty_lease_all` 的唯一 CPU 分配不是普通 affinity 对照。线程达到 RVV dirty 阈值后，会原子占用一个尚未分配的 CPU，并在后续 wakeup 中继续选择该 CPU。旧 lease 只保留 `prev_cpu`，偶尔会把两个 OpenMP worker 留在同一核；此时 barrier 的 7 个等待者会频繁耗尽自旋并进入 futex，形成百万级 wakeup/dispatch storm。当前所有启用 vector-local window 的 lease 都强制使用唯一 CPU 分配，不再提供旧行为入口。

`rvv_dirty_bind_all` 是唯一 CPU 分配的消融对照。它不启用 vector DSQ、500ms local window 或自定义 vector slice；识别后的 vector worker 只按固定 CPU 进入 local DSQ，其他 LLM 和系统任务仍走 global DSQ。

## 关键配置

`config.sh` 中常改的变量：

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `BENCHMARK_STRATEGY` | `cfs` | 默认策略 |
| `TEST_DURATION` | `300` | 单轮测试窗口秒数 |
| `BENCHMARK_LOOPS` | `1` | 每个并发档位重复轮数 |
| `CONCURRENCY_LEVELS` | `2,4,8` | 并发档位列表 |
| `TOKENS_PER_REQUEST` | `100` | 每个请求生成 token 数 |
| `REQUEST_TIMEOUT_SEC` | `180` | 单请求 curl 超时 |
| `FIXED_TOTAL_TOKENS` | `1000` | 固定总输出 token；`0` 表示固定时长模式 |
| `FIXED_REQUEST_TIMEOUT_SEC` | `600` | 固定 token 模式的单请求超时 |
| `REQUEST_TEMPERATURE` | `0` | 正式请求采样温度；默认使用确定性贪心解码 |
| `REQUEST_SEED` | `1234` | 正式请求固定随机种子 |
| `REQUEST_IGNORE_EOS` | `1` | 忽略 EOS，完整请求生成指定 token 数 |
| `RESTART_LLM` | `1` | 每轮测试前重启 llama-server |
| `SCX_AUTO_DEPLOY` | `1` | 自动部署 sched_ext loader 到板端 |
| `SCX_VECTOR_SLICE_MS` | `0` | RVV dirty vector 时间片，策略 preset 可覆盖 |
| `SCX_VECTOR_LOCAL_WINDOW_MS` | `500` | vector DSQ 放行后 V 线程继续走 local DSQ 的窗口；旧版 `rvv_dirty_all` preset 设为 `0` |
| `SCX_DIRTY_THRESHOLD` | `2` | RVV dirty 命中阈值，策略 preset 可覆盖 |
| `SCX_LPEDF_SEGMENT_MS` | `50` | LP-EDF vector limited-preemption segment |
| `SCX_LPEDF_PERIOD_MS` | `50` | LP-EDF vector virtual release period |
| `SCX_LPEDF_DEADLINE_MS` | `50` | LP-EDF vector relative deadline |
| `SCX_GANG_HINT_BOOST_US` | `250` | active barrier hint 的短时 local-head boost |
| `SCX_GANG_HINT_LAGGARD_WAITERS` | `7` | 触发最后一个 worker laggard 检查的 barrier 到达数 |
| `SCX_LEASE_CONTROL_MODE` | `off` | dirty lease 用户控制模式；策略 preset 设为 observe/active |
| `SCX_LEASE_CONTROL_DIR` | `/sys/fs/bpf/scx_rvv_dirty_lease` | pinned config 和 worker map 目录 |
| `SCX_LEASE_USER_CPU_MAP` | `auto` | `auto` 保留 dirty lease 自然唯一分配；也可传 8 个唯一 CPU，显式指定 OpenMP `ith=0..7` 的 home CPU |
| `RVV_TRACE` | `0` | 是否启动 RVV 上下文切换观测 |
| `RVV_TRACE_EXTRA_SEC` | `180` | trace 额外采集时间 |
| `RVV_TRACE_SWITCH_OUT` | `1` | 是否统计 switch-out / RVV save 候选；设为 `0` 时传递 `--no-switch-out` |
| `RVV_TRACE_SWITCH_REASON` | `1` | 是否统计 V 线程切出理由；设为 `0` 时保留 save/restore 计数但跳过 out-state |
| `RVV_TRACE_RVV_ONLY` | `0` | 为 `1` 时只累计全局 RVV save/restore 候选，不采样 wakeup、时延、线程或 CPU 细节 |
| `RVV_TRACE_FUTEX` | `0` | 是否额外统计目标进程 futex wait/wake 系统调用；设为 `1` 时传递 `--futex` |
| `RVV_TRACE_FUTEX_DETAIL` | `0` | 是否额外统计 futex 地址 + TID 维度；设为 `1` 时传递 `--futex-detail` |
| `RVV_TRACE_FUTEX_STACK` | `0` | 是否额外记录 futex 地址对应的用户态调用栈 IP；设为 `1` 时传递 `--futex-stack`，并保存 `llama_server_maps.txt` |

移动到 `experiments/benchmark` 后，默认本地工具路径为：

```bash
SCX_LOCAL_BIN=../scx_llm_pin/scx_llm_pin
SCX_EXCLUSIVE_LOCAL_BIN=../scx_rvv_exclusive/scx_rvv_exclusive
SCX_LPEDF_LOCAL_BIN=../scx_rvv_lpedf/scx_rvv_lpedf
RVV_LOCAL_BIN=../rvv_sched_cost/rvv_sched_cost
RVV_LOCAL_LIBELF=../../jupiter-buildroot/output/k1_v2/target/usr/lib/libelf-0.186.so
```

## 测试流程

每个 suite 会按 `CONCURRENCY_LEVELS x BENCHMARK_LOOPS` 执行多轮。每个单轮大致执行：

1. 检查本机依赖和 SSH 连通性。
2. 按配置停止旧 `llama-server` 并重新启动。
3. 等待 server readiness 和模型 warm-up。
4. 启动 sched_ext 策略，`cfs` 策略会跳过这一步。
5. 执行正式测试前并发 warm-up。
6. 可选启动 `rvv_sched_cost`。
7. 按当前并发数运行默认任务直到测试窗口结束。
8. 拉取 scheduler / RVV trace 日志并生成报告。
9. 清理 sched_ext loader 和 trace 进程。

## 测试任务

默认 4 个 prompt 覆盖不同负载：

| 任务 | 类型 | 目标 |
| --- | --- | --- |
| Task 1 | RVV 技术解释 | 知识密集型推理 |
| Task 2 | Python 二叉搜索树 | 代码生成和结构化输出 |
| Task 3 | 平均速度题 | 多步骤推理 |
| Task 4 | AI 研究员短篇 | 长文本生成 |

## 输出结果

完整套件目录形如：

```text
results/YYYYMMDD_HHMMSS_<strategy>_suite/
├── suite_config.txt
├── summary.csv
├── summary.txt
├── c2_loop1/
├── c4_loop1/
└── c8_loop1/
```

每个单轮结果目录形如：

```text
results/YYYYMMDD_HHMMSS_<strategy>_suite/c4_loop1/
├── config.txt
├── metrics.env
├── system_info.txt
├── sched_ext_state_start.txt
├── sched_ext_state_end.txt
├── scheduler.log
├── task1.csv
├── task1.summary
├── task2.csv
├── task2.summary
├── task3.csv
├── task3.summary
├── task4.csv
├── task4.summary
├── report.txt
├── rvv_trace_info.txt
├── rvv_sched_cost.log
└── rvv_sched_cost.json
```

`rvv_sched_cost.*` 只在 `RVV_TRACE=1` 时生成。

正式测试请求使用 `stream=true`，脚本按 SSE chunk 中的 `tokens` 数组统计实际收到的 token。窗口结束时正在进行的请求会被截断，已经收到的 token 计入 TPS，未收到的 token 不计入。TPS 的时间分母使用本轮最早 task 开始到最后一个实际收到 token 的 wall time，而不是硬编码的配置窗口秒数。

CSV 字段：

```csv
timestamp,request_num,tokens,generation_ms,ttft_ms,last_token_ms,total_ms,toks_per_sec,completed,stream_cutoff
```

主要关注：

| 指标 | 含义 |
| --- | --- |
| `TPS` | 实际收到的 token / 从本轮开始到最后一个 token 到达的真实 wall time |
| `平均TTFT` | 流式请求首个 token 到达的 wall time 平均值 |
| `p95TTFT` | 流式请求首个 token 到达 wall time 的 p95 |
| `last_token_ms` | 该请求最后一个 token 到达相对请求开始的 wall time |
| `generation_ms` | llama.cpp 预测阶段耗时；被窗口截断的请求记为 `0` |
| `completed` | 该流式请求是否收到 stop chunk |
| `stream_cutoff` | 该流式请求是否因测试窗口结束被截断 |
| `RVV dirty samples` | 调度器采样到 dirty VS 的次数 |
| `RVV vector threads` | 被识别为 vector 的线程数 |
| `v_save_candidates` / `v_restore_candidates` | RVV 上下文保存/恢复候选次数 |
| `lost_ts_updates` | trace 时间戳丢失计数，应为 0 |

## RVV Trace

开启 RVV 上下文切换观测：

```bash
RVV_TRACE=1 ./benchmark.sh --strategy rvv_dirty_all
```

trace 工具会部署并运行 `experiments/rvv_sched_cost/rvv_sched_cost`，输出 JSON 到结果目录。常用提取命令：

```bash
find results/latest -name rvv_sched_cost.json -print -quit | xargs -r jq '.vector, .lost_ts_updates'
```

长测中如需降低 trace 开销且仍保留 RVV save/restore 候选计数，可关闭切出理由统计：

```bash
RVV_TRACE=1 RVV_TRACE_SWITCH_REASON=0 ./benchmark.sh --strategy rvv_dirty_all
```

注意：当前 RISC-V Linux 路径没有 lazy vector context handling；在本项目分析口径里，RVV save/restore candidate 基本可按实际 RVV 上下文切换次数理解。

## np=4 Decode Barrier Blocked-Time

`benchmark/barrier_c4/run_compare.sh` 是独立的 barrier 对照测试。它固定
`-np 4`、并发 4、`-t 8 -tb 8`，使用 32-token 短 prompt 和较长流式输出，比较
CFS、稳定版 `rvv_dirty_lease` 或当前 `rvv_barrier_gang` 的 OpenMP 主导 futex
blocked time。该脚本使用
`rvv_sched_cost --futex-only` 的 syscall enter/exit 配对，不使用会在当前内核触发
`SIGILL` 的 barrier uprobe。完整口径和命令见 `benchmark/barrier_c4/README.md`。

```bash
cd benchmark/barrier_c4
./run_compare.sh
```

## 故障排除

检查板端连通性：

```bash
ping 192.168.3.67
sshpass -p bianbu ssh root@192.168.3.67 "echo ok"
```

查看 server：

```bash
sshpass -p bianbu ssh root@192.168.3.67 \
  "ps w | grep llama-server | grep -v grep || true; tail -80 /tmp/llama-server.log"
```

查看 sched_ext watchdog：

```bash
sshpass -p bianbu ssh root@192.168.3.67 "dmesg | tail -80"
```

清理残留调度器或 trace：

```bash
sshpass -p bianbu ssh root@192.168.3.67 \
  "ps w | grep -E 'scx_llm_pin|scx_rvv_exclusive|rvv_sched_cost' | grep -v grep || true"
```

如果出现 `sched_ext: BPF scheduler ... errored, disabling`，该轮结果需要标记为被 watchdog 污染，不能作为完整策略结果。

## 维护说明

历史上的 `QUICKSTART.md`、`QUICK_REFERENCE.md`、`BENCHMARK_SPEC.md`、`COMPLETION_REPORT.md`、`SUMMARY.txt` 和 `使用说明.txt` 已整合进本 README。后续更新 benchmark 用法时只维护这一份文档。
