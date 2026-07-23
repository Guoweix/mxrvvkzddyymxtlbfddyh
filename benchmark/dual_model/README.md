# Dual-Model Benchmark

该 harness 在同一块 8 核板卡上启动两个独立的 llama-server，支持确定性异步注入
和同步对称负载。

以下示例用环境变量表示板端 llama.cpp、实验 runtime、调度器和控制 map 路径：

```bash
export LLM_PATH="<board-llama-directory>"
export REMOTE_RUNTIME_DIR="<board-experimental-runtime-directory>"
export REMOTE_SCHEDULER_BIN="<board-scheduler-binary>"
export LEASE_HINT_PATH="/sys/fs/bpf/scx_rvv_dirty_lease/workers"
export MODEL_A_PORT="<model-a-port>"
export MODEL_B_PORT="<model-b-port>"
```

调用 `run.sh` 时可通过 `--llm-path "$LLM_PATH"`、`--port-a "$MODEL_A_PORT"` 和
`--port-b "$MODEL_B_PORT"` 显式传入这些环境配置。

## Asynchronous Workload

- 每个 server 默认使用 4 个并行 slot。
- `--concurrency` 可设置总并发为 2、4、8；两个 server 分别使用 1、2、4 个 slot 和请求。
- incumbent 请求按 1 秒间隔到达；全部 incumbent 请求都输出第 4 个 token 后，向另一个 server 注入等量请求。
- injected 请求同样按 1 秒间隔到达。
- 每个请求默认固定输出 256 token，8 个请求共输出 2048 token。
- 默认只运行一个 `A -> B` 方向和一个 loop。该配置显著长于之前的 16-token 五分钟场景。
- 四个基础 prompt 内容不同且没有共同的长前缀。脚本先调用目标模型的
  `/tokenize`，再将每个 prompt 构造成精确 128 input token 的 token 数组。

并发低于 8 时，固定 prompt 文件按顺序取前 N 个。例如总并发 2/4/8 分别在
每个 server 使用 1/2/4 个不同内容的等长 prompt。四个 prompt 的输入长度都为
128 token，因此跨并发结果不再混入 prompt 长度变化；每个 server 使用相同的
prompt 子集，保证两组请求对称。

所有正式请求固定使用：

```text
temperature=0
top_k=1
top_p=1.0
seed=1234
ignore_eos=true
cache_prompt=false
stream=true
```

## Symmetric Workload

`--scenario symmetric` 同时释放 A/B 的匹配请求，默认把全部请求放在同一个绝对
monotonic release point。两侧使用完全相同的 prompt、seed 和输出 token 数；启动
偏差进入有效性门禁，完成偏差只作为调度公平性结果记录。

专用入口和完整指标说明见 [`symmetric/`](symmetric/README.md)：

```bash
cd symmetric
./run.sh --strategy cfs-split --concurrency 8 --loops 3 --output-tokens 256
```

该场景用于测试两个服务全程重叠时的稳态效率。默认异步场景继续用于测量 prefill
注入、TTFT 和非对称尾部，二者结果不应直接混合平均。

脚本会严格检查每个请求的 token 数和注入顺序。输出 token ID 哈希仍会保存并汇总，但自然文本哈希不一致只作为诊断信息，不会使性能轮失效。请求不完整、server 退出或内核日志出现调度 stall 时，该 attempt 不进入有效结果。

## Strategies

共享全部 8 核，每个 server 各 8 个计算线程：

```bash
./run.sh --strategy cfs-shared
```

空间隔离对照，每个 server 使用 4 个独立 CPU 和 4 个计算线程：

```bash
./run.sh --strategy cfs-split
```

快速功能验证（仅该模式会将每请求输出缩短为 16 token，不计入正式结果）：

```bash
./run.sh --strategy cfs-shared --smoke
```

默认测试使用四个不同内容的 128-token prompt。脚本会先验证所有基础文本都不
超过目标长度，然后使用 token 数组精确补齐；无法构造等长输入时直接拒绝运行。
里程碑超时默认保留 600 秒。

`--prompts-file PATH` 可为功能 smoke 指定较小的 prompt 集；每个条目必须包含
唯一 `id`、非空 `prompt` 和相同的正整数 `target_tokens`。正式结果必须保留默认
等长文件，suite 会把实际输入复制为 `prompts.json`。

原双向长测仍可显式运行：

```bash
./run.sh --strategy cfs-shared --directions both --loops 3 \
  --concurrency 8 --output-tokens 256 --injection-token 32
```

`external` 模式把两个 PID 作为板端环境变量交给一个外部调度器命令：

```bash
./run.sh --strategy external \
  --external-start-cmd "$REMOTE_SCHEDULER_BIN --tgid-a \"\$MODEL_A_PID\" --tgid-b \"\$MODEL_B_PID\""
```

可选的停止命令会收到 `MODEL_A_PID`、`MODEL_B_PID` 和 `SCHEDULER_PID`。未提供时，harness 对启动命令返回的进程发送 `SIGTERM`。

实验版 server 可以放在独立目录，不必覆盖默认安装。loader 仍来自 `--llm-path`，动态库优先搜索 staged 目录：

```bash
./run.sh --runtime-bin-dir "$REMOTE_RUNTIME_DIR" ...
```

需要 llama.cpp 向 pinned worker map 注册两组 OpenMP worker 时，在两个 server 启动前传入同一路径；客户端会按自身 TGID 选择对应页：

```bash
./run.sh --strategy external \
  --lease-hint-path "$LEASE_HINT_PATH" \
  --external-start-cmd "$REMOTE_SCHEDULER_BIN --tgid \"\$MODEL_A_PID\" --tgid2 \"\$MODEL_B_PID\" \
    --strategy rvv_dirty --user-control active"
```

当前 phase-aware dirty lease 让普通 vector wakeup 回到 VDSQ，在 CPU 需要补充 local
DSQ 时偏向 prefill，并定向抢占同核的另一组 vector worker：

```bash
./run.sh --strategy external \
  --runtime-bin-dir "$REMOTE_RUNTIME_DIR" \
  --lease-hint-path "$LEASE_HINT_PATH" \
  --external-start-cmd "$REMOTE_SCHEDULER_BIN \
    --tgid \"\$MODEL_A_PID\" --tgid2 \"\$MODEL_B_PID\" \
    --strategy rvv_dirty --vector-slice-ms 100 \
    --vector-local-window-ms 500 --user-control active \
    --vector-local-wakeup off --phase-wakeup-preempt on \
    --team-gang prefill-lease --prefill-starvation-ms 1000"
```

`--scheduler-validation auto` 会从 external 命令识别 lease、gang-observe、gang-active 或 phase-lease。每轮要求调度器在 workload 结束时仍存活，并检查 8+8 worker、phase 命中、对应策略实际生效和 team DSQ 收支；phase-lease 还要求定向 wakeup 抢占实际发生、gang quanta/kick 为零且没有 4s vector-DSQ pending。任一失败会使 attempt 无效。非 `scx_llm_pin` external 命令可用 `--scheduler-validation none`，也可显式选择 validation profile。

定向 wakeup 抢占的三轮正式结果与历史 observe/prefill-lease 对照见 `docs/rvv_phase_lease_wakeup_preempt_20260719.md`。

`--injection-hook` 会在 incumbent 四个请求都达到注入 token、injected 请求尚未发送时执行。hook 环境提供 `INCUMBENT_MODEL`、`INJECTED_MODEL`、`INCUMBENT_TEAM` 和 `INJECTED_TEAM`；非零退出或超时会使该 attempt 无效。

当前推荐的双模型 phase-aware dirty lease 命令为：

```bash
./run.sh --strategy external \
  --lease-hint-path "$LEASE_HINT_PATH" \
  --external-start-cmd "$REMOTE_SCHEDULER_BIN \
    --tgid \"\$MODEL_A_PID\" --tgid2 \"\$MODEL_B_PID\" \
    --strategy rvv_dirty --vector-slice-ms 100 \
    --vector-local-window-ms 500 --dirty-threshold 2 \
    --vector-local-wakeup off --phase-wakeup-preempt on \
    --user-control active --user-cpu-map auto \
    --team-gang prefill-lease --prefill-starvation-ms 1000"
```

## Equal-Length TTFT p95 Campaign

`run_ttft_p95_campaign.sh` 是当前板卡环境的一次性完整对比入口，不接受参数。它固定
使用四个 128-token prompt、256 output token、A→B token-4 注入和 standard
metrics，运行 CFS、Dirty Lease Observe、Phase-Aware Dirty Lease 的 c=2/4/8
矩阵。

每轮 injected TTFT 样本数为 c/2。脚本为每个策略/并发单元收集 60 个样本，因此
c=2/4/8 分别运行 60/30/15 轮。结果写入
`results/ttft_p95_equal128/`；成功单元使用 `.done` 标记，重新执行时自动跳过，失败
单元保留日志并在下次执行时重试。

后台启动：

```bash
tmux new-session -d -s ttft-p95 \
  'cd /path/to/experiments/benchmark/dual_model && \
   ./run_ttft_p95_campaign.sh > ttft-p95.log 2>&1'
```

查看状态：

```bash
tail -f ttft-p95.log
cat results/ttft_p95_equal128/status.tsv
```

c=4 消融中，关闭 local-wakeup fast path 后 TPS 与原 phase-aware 路径持平，
injected mean TTFT 从 `10.856s` 降为 `9.169s`，preferred-team run match 从
`51.18%` 提升到 `61.69%`。该结果目前是单轮机制验证；正式方差复测完成前不应
把小幅差异视为稳定收益。报告见
[`phase-aware-dirty-lease-c4-ablation.md`](../../docs/reports/phase-aware-dirty-lease-c4-ablation.md)。

## Metrics

默认 `--metrics standard` 记录：

- TTFT、E2E latency、decode TPS 和逐 token ITL。
- injected TTFT 与 pooled p95 TTFT。
- incumbent 注入前/后的 p50、p95 ITL。
- overlap 期间两模型 TPS 和 Jain fairness。
- aggregate TPS。
- 每个 PID/TID 的 runtime、runqueue wait、timeslice、context switch 和 migration。
- CPU 频率、温度、可用内存、内核日志和 sched_ext 状态。

默认有效性门限为最低频率 `1600000 kHz`、最高温度 `85000 mC`、最低可用内存 `1048576 KiB`，均可通过同名命令行参数覆盖。超限 attempt 会被丢弃并自动重试。

`--metrics rvv` 会额外为两个 TGID 启动独立的 `rvv_sched_cost --rvv-only`。

`--metrics rvv-barrier` 使用低扰动的 `--rvv-only --futex-time` 组合，在同一轮中记录两模型的 RVV save/restore 候选和 futex blocked time。报告同时给出全部 futex 统计及每个模型 wait 次数最多的 futex 地址作为 libgomp barrier proxy。该 proxy 是 summed worker blocked time，不是 barrier 关键路径，并且不包含用户态自旋阶段。

两种 trace profile 都用于诊断，不能与 `standard` 性能结果直接合并。trace JSON 缺失、TGID 不匹配、RVV 计数为空、futex profile 为空或 timestamp map 丢失都会使 attempt 无效。

结果目录包含：

```text
config.json
prompts.json
system_info.txt
run*/events.jsonl
run*/workload.json
run*/sampler.jsonl
run*/scheduler.log
run*/run_metrics.json
summary.csv
summary.json
report.md
```

两个服务端口由 `--port-a` / `--port-b` 指定。如果目标端口已有项目默认服务，harness
只停止对应端口的 PID，并在 suite 结束后通过 `$LLM_PATH/run-server.sh` 恢复服务。
