# Dual-Model Asynchronous Benchmark

该 harness 在同一块 8 核板卡上启动两个独立的 llama-server，用确定性异步负载测量多模型 CPU 竞争。

## Workload

- 每个 server 默认使用 4 个并行 slot。
- `--concurrency` 可设置总并发为 2、4、8；两个 server 分别使用 1、2、4 个 slot 和请求。
- incumbent 请求按 1 秒间隔到达；全部 incumbent 请求都输出第 4 个 token 后，向另一个 server 注入等量请求。
- injected 请求同样按 1 秒间隔到达。
- 每个请求默认固定输出 256 token，8 个请求共输出 2048 token。
- 默认只运行一个 `A -> B` 方向和一个 loop。该配置显著长于之前的 16-token 五分钟场景。
- 四个固定 prompt 在当前模型上分别为 27、111、326、713 input token。

并发低于 8 时，固定 prompt 文件按顺序取前 N 档。例如总并发 2/4/8
分别使用 short、short+medium、全部四档 prompt。每个 server 使用相同的
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

默认测试仍使用完整四档 prompt。长 prompt 在 4 核对照上可能需要数分钟，因此里程碑超时默认保留 600 秒。

`--prompts-file PATH` 可为功能 smoke 指定较小的 prompt 集；正式结果必须保留默认四档文件，suite 会把实际输入复制为 `prompts.json`。

原双向长测仍可显式运行：

```bash
./run.sh --strategy cfs-shared --directions both --loops 3 \
  --concurrency 8 --output-tokens 256 --injection-token 32
```

`external` 模式把两个 PID 作为板端环境变量交给一个外部调度器命令：

```bash
./run.sh --strategy external \
  --external-start-cmd '/tmp/my_scheduler --tgid-a "$MODEL_A_PID" --tgid-b "$MODEL_B_PID"'
```

可选的停止命令会收到 `MODEL_A_PID`、`MODEL_B_PID` 和 `SCHEDULER_PID`。未提供时，harness 对启动命令返回的进程发送 `SIGTERM`。

实验版 server 可以放在独立目录，不必覆盖默认安装。loader 仍来自 `--llm-path`，动态库优先搜索 staged 目录：

```bash
./run.sh --runtime-bin-dir /tmp/llama-phase ...
```

需要 llama.cpp 向 pinned worker map 注册两组 OpenMP worker 时，在两个 server 启动前传入同一路径；客户端会按自身 TGID 选择对应页：

```bash
./run.sh --strategy external \
  --lease-hint-path /sys/fs/bpf/scx_rvv_dirty_lease/workers \
  --external-start-cmd '/tmp/scx_llm_pin --tgid "$MODEL_A_PID" --tgid2 "$MODEL_B_PID" \
    --strategy rvv_dirty --user-control active'
```

保留 dirty lease slice/window，在自然 lease 边界偏向 prefill，并定向抢占同核的另一组 vector worker：

```bash
./run.sh --strategy external \
  --runtime-bin-dir /tmp/llama-phase \
  --lease-hint-path /sys/fs/bpf/scx_rvv_dirty_lease/workers \
  --external-start-cmd '/tmp/scx_llm_pin/scx_llm_pin \
    --tgid "$MODEL_A_PID" --tgid2 "$MODEL_B_PID" \
    --strategy rvv_dirty --vector-slice-ms 100 \
    --vector-local-window-ms 500 --user-control active \
    --team-gang prefill-lease --prefill-starvation-ms 1000'
```

`--scheduler-validation auto` 会从 external 命令识别 lease、gang-observe、gang-active 或 phase-lease。每轮要求调度器在 workload 结束时仍存活，并检查 8+8 worker、phase 命中、对应策略实际生效和 team DSQ 收支；phase-lease 还要求定向 wakeup 抢占实际发生、gang quanta/kick 为零且没有 4s vector-DSQ pending。任一失败会使 attempt 无效。非 `scx_llm_pin` external 命令可用 `--scheduler-validation none`，也可显式选择 validation profile。

定向 wakeup 抢占的三轮正式结果与历史 observe/prefill-lease 对照见 `docs/rvv_phase_lease_wakeup_preempt_20260719.md`。

`--injection-hook` 会在 incumbent 四个请求都达到注入 token、injected 请求尚未发送时执行。hook 环境提供 `INCUMBENT_MODEL`、`INJECTED_MODEL`、`INCUMBENT_TEAM` 和 `INJECTED_TEAM`；非零退出或超时会使该 attempt 无效。

当前已验收的双模型 dirty lease 命令为：

```bash
./run.sh --strategy external \
  --lease-hint-path /sys/fs/bpf/scx_rvv_dirty_lease/workers \
  --external-start-cmd '/tmp/scx_llm_pin_dual/scx_llm_pin \
    --tgid "$MODEL_A_PID" --tgid2 "$MODEL_B_PID" \
    --strategy rvv_dirty --vector-slice-ms 100 \
    --vector-local-window-ms 500 --dirty-threshold 2 \
    --user-control active --user-cpu-map auto'
```

两轮 CFS 为 `2.40695 +/- 0.00140 TPS`，三轮 active-auto 为 `2.86833 +/- 0.00939 TPS`，平均提升 `19.17%`。注入时在线改为 1000ms window 的正式候选为 `2.85866 TPS`，未超过 500ms active-auto，因此默认不变。完整报告见 `docs/rvv_dirty_lease_dual_model_userspace_20260718.md`。

使用相同 `rvv-barrier` trace 配置重新完成的三轮 CFS/三轮 active-auto 对比为 `2.35419 +/- 0.00634` 和 `2.80468 +/- 0.03187 TPS`，lease 提升 `19.14%`。lease 将通用上下文切换和 RVV save+restore 候选都降低约 90%，但 top-futex summed worker blocked time 增加 34.44%，因此不能把吞吐收益简单归因为 barrier 累计等待减少。详细逐任务 TTFT/TPS 与原始轮次见 `docs/cfs_vs_rvv_dirty_lease_traced_dual_20260718.md`。

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

默认使用端口 9090/9091。如果 9090 上已有项目默认服务，harness 只停止该端口对应的 PID，并在 suite 结束后通过 `/mnt/llm/llama/run-server.sh` 恢复服务。
