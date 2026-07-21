# Dual-Model Symmetric Benchmark

该场景用于测量两个持续满载、工作量完全对称的 `llama-server` 在 8 核 CPU 上的
稳态调度效率。它与父目录默认的异步注入场景相互独立，不替换后者。

## Workload

- A/B 使用相同模型、prompt 子集、seed 和每请求输出 token 数。
- 所有匹配的 A/B 请求使用同一个绝对 monotonic release time。
- 默认 `arrival-gap=0`，因此该轮全部请求同时释放。
- 每轮检查 A/B prompt token 数相同、实际输出完整，并要求请求启动偏差不超过
  `--max-start-skew-ms`，默认 25ms。
- 完成时间不作为有效性门禁。报告记录 A/B 总完成偏差和每对 prompt 的完成偏差，
  避免只接受调度结果恰好对称的样本。

主要指标包括 aggregate TPS、A/B E2E TPS、A/B completion skew、重叠时间占比、
8 核 LLM CPU 利用率、runqueue wait、上下文切换和 Jain fairness。

## Usage

CFS 8+8 共享核心：

```bash
./run.sh --strategy cfs-shared --concurrency 8 --loops 3 --output-tokens 256
```

CFS 4+4 静态隔离：

```bash
./run.sh --strategy cfs-split --concurrency 8 --loops 3 --output-tokens 256
```

phase-aware dirty lease 8+8：

```bash
./run.sh --strategy external \
  --concurrency 8 --loops 3 --output-tokens 256 \
  --runtime-bin-dir "$REMOTE_RUNTIME_DIR" \
  --lease-hint-path "$LEASE_HINT_PATH" \
  --external-start-cmd "$REMOTE_SCHEDULER_BIN \
    --tgid \"\$MODEL_A_PID\" --tgid2 \"\$MODEL_B_PID\" \
    --strategy rvv_dirty --vector-slice-ms 100 \
    --vector-local-window-ms 500 --dirty-threshold 2 \
    --vector-local-wakeup off --phase-wakeup-preempt on \
    --user-control active --user-cpu-map auto \
    --team-gang prefill-lease --prefill-starvation-ms 1000"
```

`--directions both` 可交换 server 启动顺序并各运行一轮；请求仍然同步释放，可用于检查
启动顺序偏差。功能 smoke 可增加 `--smoke`，仅 smoke 会把每请求输出改为 16 token。
