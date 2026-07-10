# RVV LLM Benchmark

`experiments/benchmark` 是当前项目的 LLM 推理基准测试入口，用于比较 CFS、sched_ext 调度策略和 RVV dirty 识别策略对 llama.cpp 推理性能的影响。

测试脚本会自动重启板端 `llama-server`，完成正式测试前预热，启动可选 sched_ext 调度器和 RVV 上下文切换观测工具，并生成每轮测试结果。

## 目录结构

```text
experiments/benchmark/
├── benchmark.sh          # 主测试脚本
├── config.sh             # 默认配置
├── generate_report.py    # 报告生成器
├── compare.sh            # 两个结果目录的简单对比
├── verify.sh             # 本地依赖、配置和连通性检查
├── run_test.sh           # 默认 300s 测试包装脚本
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
LLM_PARALLEL=4
LLM_CONTEXT=4096
LLM_PATH=/mnt/llm/llama
```

默认模型路径由脚本启动命令使用：

```text
/mnt/llm/llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf
```

先检查环境：

```bash
cd /home/gwx/Workdir/riscv-rvv/experiments/benchmark
./verify.sh
```

## 快速开始

运行 CFS baseline：

```bash
cd /home/gwx/Workdir/riscv-rvv/experiments/benchmark
./benchmark.sh --strategy cfs
```

运行 30 秒快速测试：

```bash
./benchmark.sh --strategy cfs --duration 30
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
  --vector-slice-ms 50 \
  --dirty-threshold 2
```

运行 local DSQ 对照：

```bash
./benchmark.sh --strategy local_dsq_50ms
```

指定测试时长、并发数和 llama 线程数：

```bash
./benchmark.sh --strategy cfs --duration 300 --tasks 4 --threads 8
```

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
| `--duration N` | 测试窗口秒数，默认 `300` |
| `--tasks N` | 并发任务数，默认 `4` |
| `--threads N` | llama.cpp 计算线程数，默认 `8` |
| `--strategy S` | benchmark 策略，默认 `cfs` |
| `--vector-slice-ms N` | 覆盖 RVV dirty vector 时间片 |
| `--dirty-threshold N` | 覆盖 RVV dirty 命中阈值 |
| `--warmup-requests N` | 正式测试前预热请求数，默认跟随并发数 |
| `--warmup-tokens N` | 每个预热请求生成 token 数，默认 `1` |
| `--warmup-retries N` | 预热失败重试次数，默认 `3` |
| `--no-warmup` | 跳过正式测试前预热 |
| `--list-strategies` | 列出可用策略 |
| `--help` | 显示帮助 |

## 策略说明

| 策略 | 说明 |
| --- | --- |
| `cfs` | 默认 CFS baseline，不加载 sched_ext |
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
| `rvv_dirty_all` | 全核心 RVV Dirty 识别，vector 线程长时间片 |
| `rvv_dirty_local_all` | RVV Dirty vector 长时间片 + LLM 辅助线程本地 DSQ |
| `rvv_dirty_6_2` | CPU0-5 激进计算核心，CPU6-7 普通/回退核心 |
| `rvv_exclusive_6_2` | CPU2-7 RVV 专用核心，CPU0-1 普通核心 |

## 关键配置

`config.sh` 中常改的变量：

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `BENCHMARK_STRATEGY` | `cfs` | 默认策略 |
| `TEST_DURATION` | `300` | 测试窗口秒数 |
| `CONCURRENT_TASKS` | `4` | 并发任务数 |
| `TOKENS_PER_REQUEST` | `100` | 每个请求生成 token 数 |
| `RESTART_LLM` | `1` | 每轮测试前重启 llama-server |
| `SCX_AUTO_DEPLOY` | `1` | 自动部署 sched_ext loader 到板端 |
| `SCX_VECTOR_SLICE_MS` | `0` | RVV dirty vector 时间片，策略 preset 可覆盖 |
| `SCX_DIRTY_THRESHOLD` | `2` | RVV dirty 命中阈值，策略 preset 可覆盖 |
| `RVV_TRACE` | `0` | 是否启动 RVV 上下文切换观测 |
| `RVV_TRACE_EXTRA_SEC` | `180` | trace 额外采集时间 |

移动到 `experiments/benchmark` 后，默认本地工具路径为：

```bash
SCX_LOCAL_BIN=../scx_llm_pin/scx_llm_pin
SCX_EXCLUSIVE_LOCAL_BIN=../scx_rvv_exclusive/scx_rvv_exclusive
RVV_LOCAL_BIN=../rvv_sched_cost/rvv_sched_cost
RVV_LOCAL_LIBELF=../../jupiter-buildroot/output/k1_v2/target/usr/lib/libelf-0.186.so
```

## 测试流程

每轮 `benchmark.sh` 大致执行：

1. 检查本机依赖和 SSH 连通性。
2. 按配置停止旧 `llama-server` 并重新启动。
3. 等待 server readiness 和模型 warm-up。
4. 启动 sched_ext 策略，`cfs` 策略会跳过这一步。
5. 执行正式测试前并发 warm-up。
6. 可选启动 `rvv_sched_cost`。
7. 运行 4 个默认任务直到测试窗口结束。
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

每轮结果目录形如：

```text
results/YYYYMMDD_HHMMSS_<strategy>_/
├── config.txt
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

CSV 字段：

```csv
timestamp,request_num,tokens,generation_ms,total_ms,toks_per_sec
```

主要关注：

| 指标 | 含义 |
| --- | --- |
| `总吞吐量` | 全部任务总 tok/s |
| `平均首字延迟` | 4 个任务首个请求 TTFT 的平均值 |
| `generation_ms` | llama.cpp 预测阶段耗时 |
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
jq '.vector, .lost_ts_updates' results/latest/rvv_sched_cost.json
```

注意：当前 RISC-V Linux 路径没有 lazy vector context handling；在本项目分析口径里，RVV save/restore candidate 基本可按实际 RVV 上下文切换次数理解。

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
