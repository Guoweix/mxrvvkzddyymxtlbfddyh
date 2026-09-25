# scx_llm_pin

`scx_llm_pin` 是面向 8 核 RISC-V llama.cpp 负载的 sched_ext 实验调度器。
当前只保留一种调度策略 `rvv_dirty_local`：

- 根据 `sstatus.VS=Dirty` 识别 RVV vector worker；
- 对 vector worker 使用全 8 核唯一 CPU 分配和 per-team/per-CPU vector DSQ；
- 目标 LLM 的非 vector 辅助线程进入 local DSQ。

调度器接管全部任务，不再提供其它策略轴或消融模式。

## ABI v4：N 模型与可变 worker

运行时支持 1–8 个 llama-server；每进程预留 32 个 worker 槽位，每槽 64 字节，
但实际 decode/prefill 线程数均限制为 1–8，且不超过该模型允许的 CPU 数。
两个线程数可以不同。所有模型线程总数可以超过八。

```bash
./scx_llm_pin \
  --team <pid-A>:8:8 \
  --team <pid-B>:4:4 \
  --team <pid-C>:1:2:0xc0
```

`--team` 格式为 `PID:decode_threads:prefill_threads[:cpu_mask]`，mask 默认
`0xff`，可重复最多八次。重复 PID、超限线程数、非法 CPU 集合在加载前报错。
`--check-config` 只解析并输出配置；正常加载还检查进程 affinity/在线 CPU。
`--tgid/--tgid2` 保留为每模型八线程的兼容入口，不能与 `--team` 混用。

每模型 CPU 顺序在启动时生成：模型内唯一，模型间按已分配数量分散。

ABI v4 的 `config`/`workers` bpffs map 始终创建并 pin 在默认目录
`/sys/fs/bpf/scx_rvv_dirty_lease`。`rvv_lease_ctl status` 输出每模型配置、
注册位图和每槽的 `membership/current`，用于 ABI v4 runtime 的注册校验。
调度器本身不再根据 worker page 覆盖 CPU placement。

## 自动 phase-aware 调度

调度策略在运行时根据 ABI v4 worker page 是否出现有效 phase hint 自动切换，
不需要任何策略/模式开关：

- 没有任何 team 写入有效 `team_hint`（`sequence`/`phase_epoch` 始终为 0）：走
  `rvv_dirty_local` 回退路径——唯一 CPU 绑定 + 长 `vector_slice_ns`。
- 任一 team 出现有效 hint：启用 phase-aware 调度——PREFILL/MIXED 优先消费、
  preferred wakeup 定向抢占（`SCX_ENQ_WAKEUP`）以及 prefill 安全时间片。
  hint 的有效性由 magic/ABI/active/team_id/session_id/worker_limit 校验、
  偶数非零 `sequence`、phase 范围、`expires_ns` 未过期以及 `begin_ns` 在
  `--phase-hint-timeout-ms` 内共同决定。

两种路径共用同一套 per-team/per-CPU vector DSQ、唯一 CPU 绑定和 round-robin
回退消费。

## 当前策略

| 策略 | 行为 |
|---|---|
| `rvv_dirty_local` | 根据 `sstatus.VS=Dirty` 识别 vector worker，使用全 8 核唯一 CPU 分配和 per-team/per-CPU vector DSQ；目标 LLM 的非 vector 辅助线程进入 local DSQ |

推荐参数：100ms vector slice、500ms window 上限、dirty threshold 2。
vector DSQ 之间按 CPU 轮转，空队列直接跳过。普通系统任务走 global 路径。

## 编译和部署

要求内核启用 sched_ext 和 BTF，并使用仓库中的 Buildroot RISC-V 工具链：

```bash
export EXPERIMENTS_ROOT="$(git rev-parse --show-toplevel)"
export BOARD_IP="<board-ip>"
export BOARD_USER="<board-user>"
export BOARD_PASS="<board-password>"

cd "$EXPERIMENTS_ROOT/scx_llm_pin"
make fetch-btf BOARD_IP="$BOARD_IP" BOARD_USER="$BOARD_USER" BOARD_PASS="$BOARD_PASS"
make
make deploy BOARD_IP="$BOARD_IP" BOARD_USER="$BOARD_USER" BOARD_PASS="$BOARD_PASS"
```

也可以由 `benchmark/benchmark.sh` 自动部署已编译的二进制。

## 基本用法

```bash
# 唯一策略，100ms slice、500ms window、dirty threshold 2
./scx_llm_pin --tgid "$LLM_PID" \
  --vector-slice-ms 100 --vector-local-window-ms 500 --dirty-threshold 2
```

主要参数：

```text
--tgid PID
--tgid2 PID
--team PID:T:TB[:MASK]
--vector-slice-ms MS
--vector-local-window-ms MS
--dirty-threshold N
--phase-hint-timeout-ms MS
--prefill-starvation-ms MS
--phase-wakeup-preempt on|off
--duration SEC
```

调度器始终调用 `scx_bpf_switch_all()`。停止 loader 会 detach struct_ops，并让任务回到
内核默认调度器。

## 用户态 Dirty Lease 控制

用户态控制要求带 sched hint 补丁的 llama.cpp。调度器始终创建 ABI v4 控制 map，
在线查看和修改（只保留 slice/window/dirty-threshold）：

```bash
./rvv_lease_ctl status
./rvv_lease_ctl set --slice-ms 120 --window-ms 600 --dirty-threshold 3
./rvv_lease_ctl reset
```

控制 map 默认位于 `/sys/fs/bpf/scx_rvv_dirty_lease`。

## 验证

```bash
make
./scx_llm_pin --help

cd ../benchmark
./benchmark.sh --list-strategies
./benchmark.sh --concurrency-levels 2 --loops 1 \
  --total-tokens 16 --warmup-requests 2 --warmup-tokens 1
```

需要 RVV 上下文切换和 futex 观测时，通过 `benchmark/config.sh` 中的
`RVV_TRACE*` 配置启用 `../rvv_sched_cost/rvv_sched_cost`。

## 故障排查

```bash
cat /sys/kernel/sched_ext/state
cat /sys/kernel/sched_ext/root/ops
dmesg | tail -80
```

若出现 sched_ext watchdog、runnable task stall 或 loader 提前退出，该轮测试无效，
不能进入性能均值。
