# scx_llm_pin

`scx_llm_pin` 是面向 8 核 RISC-V llama.cpp 负载的 sched_ext 实验调度器。
当前只保留 global、RVV dirty 和 RVV dirty local 三种代码路径。调度器接管全部
任务，不再提供旧 CPU 隔离、固定 CPU mask、LLM-only、long slice 或 6+2 fallback
策略。

## ABI v4：N 模型与可变 worker（当前版本）

运行时支持 1–8 个 llama-server；每进程预留 32 个 worker 槽位，每槽 64 字节，
但实际 decode/prefill 线程数均限制为 1–8，且不超过该模型允许的 CPU 数。
两个线程数可以不同。所有模型线程总数可以超过八。

```bash
./scx_llm_pin \
  --team <pid-A>:8:8 \
  --team <pid-B>:4:4 \
  --team <pid-C>:1:2:0xc0 \
  --strategy rvv_dirty --user-control active \
  --team-gang prefill-lease --vector-local-wakeup off
```

`--team` 格式为 `PID:decode_threads:prefill_threads[:cpu_mask]`，mask 默认
`0xff`，可重复最多八次。重复 PID、超限线程数、非法 CPU 集合在加载前报错。
`--check-config` 只解析并输出配置；正常加载还检查进程 affinity/在线 CPU。
`--tgid/--tgid2` 保留为每模型八线程的兼容入口，不能与 `--team` 混用。

每模型 CPU 顺序在启动时生成：模型内唯一，模型间按已分配数量分散。活跃 worker
按 `ith` 映射到该顺序；每个实际 OpenMP graph 发布新的 membership epoch 和注册
位图，旧 epoch 的 worker 不参与当前成员校验，支持 `-t != -tb` 和单线程 graph。
运行中不完整的成员集合通过普通调度路径继续推进；超限 hint 不会获得 vector 优化。
`vector_threads` 统计已识别且尚未撤销的线程，当前参与数以 `team_phase.workers` 为准。

N-team 的每核 VDSQ 选择：超时 pending 兜底、PREFILL/MIXED 队列轮转、所有队列
轮转；空队列直接跳过。定向抢占仅针对另一 team 的非优先阶段 vector worker，
同类 prefill team 之间不互相抢占。该轮转不保证不同线程数模型获得相同总 CPU 时间。
普通任务仍走 global 路径，防饥饿阈值不是严格的 wall-time 等待上界。
旧 `always` gang 消融模式仍仅支持两个模型；`observe/prefill-lease` 支持 N 模型。

推荐默认 `vector-local-wakeup=off`。模型集合与 CPU 顺序启动时确定，不提供在线增删
模型；进程替换后需要重新加载 TGID 配置。退出的任务通过 disable 回调清理分类记录。

当前 ABI 为 **v4**，需要按 `llama_cpp_patches/README.md` 应用后续 0002 补丁。
旧 v3 runtime 会拒绝 hint 连接，不能作为新版本的正式测试 runtime。
`rvv_lease_ctl status` 输出每模型配置、注册位图和每槽的 `membership/current`，
`--prefer-team off|0..7` 可指定已配置模型；旧全局显式 CPU map 必须适用于全部模型。
N 模型测试入口见 [`../benchmark/multi_model/`](../benchmark/multi_model/)。

## 当前策略

| Loader strategy | 行为 |
|---|---|
| `global` | 所有任务进入 sched_ext global DSQ，作为最小 sched_ext 对照 |
| `rvv_dirty` | 根据 `sstatus.VS=Dirty` 识别 vector worker，使用全 8 核唯一 CPU 分配和 per-team/per-CPU vector DSQ |
| `rvv_dirty_local` | vector worker 与 `rvv_dirty` 相同；目标 LLM 的非 vector 辅助线程进入 local DSQ |

当前推荐的 phase-aware 路径使用 100ms vector slice、500ms window 上限、dirty
threshold 2，并显式设置 `--vector-local-wakeup off`。完成启动识别后，普通 vector
wakeup 重新进入对应 VDSQ，不使用 team-local fast path；500ms 参数仍作为可在线
控制的窗口上限保留。`team` 和 `exclusive` 用于 local-wakeup 消融实验。

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
# 最小 global DSQ
./scx_llm_pin --tgid "$LLM_PID" --strategy global

# RVV dirty，不启用 lease window
./scx_llm_pin --tgid "$LLM_PID" --strategy rvv_dirty \
  --vector-slice-ms 100 --vector-local-window-ms 0 --dirty-threshold 2

# RVV dirty，普通 vector wakeup 回到 VDSQ
./scx_llm_pin --tgid "$LLM_PID" --strategy rvv_dirty \
  --vector-slice-ms 100 --vector-local-window-ms 500 \
  --vector-local-wakeup off --dirty-threshold 2

# 同一路径，目标 LLM 的非 vector 辅助线程走 local DSQ
./scx_llm_pin --tgid "$LLM_PID" --strategy rvv_dirty_local \
  --vector-slice-ms 100 --vector-local-window-ms 500 \
  --vector-local-wakeup off --dirty-threshold 2
```

主要参数：

```text
--tgid PID
--tgid2 PID
--strategy global|rvv_dirty|rvv_dirty_local
--vector-slice-ms MS
--vector-local-window-ms MS
--vector-local-wakeup team|off|exclusive
--dirty-threshold N
--phase-wakeup-preempt on|off
--duration SEC
```

调度器始终调用 `scx_bpf_switch_all()`。停止 loader 会 detach struct_ops，并让任务回到
内核默认调度器。

## 用户态 Dirty Lease 控制

用户态控制要求带 sched hint 补丁的 llama.cpp。单模型可选择 observe 或 active：

```bash
./scx_llm_pin --tgid "$LLM_PID" --strategy rvv_dirty \
  --vector-slice-ms 100 --vector-local-window-ms 500 \
  --vector-local-wakeup off \
  --user-control observe

./scx_llm_pin --tgid "$LLM_PID" --strategy rvv_dirty \
  --vector-slice-ms 100 --vector-local-window-ms 500 \
  --vector-local-wakeup off \
  --user-control active --user-cpu-map auto
```

- `off`：不创建控制 map，只使用 RVV dirty 识别。
- `observe`：读取和校验 OpenMP worker 注册，不按 `ith` 覆盖 CPU placement。
- `active` + `auto`：允许在线参数控制，使用启动时生成的组内唯一 CPU 顺序。
- `active` + 显式 map：按 8 个唯一 CPU 的顺序覆盖 `ith -> CPU`。

控制 map 默认位于 `/sys/fs/bpf/scx_rvv_dirty_lease`。在线查看和修改：

```bash
./rvv_lease_ctl status
./rvv_lease_ctl set --slice-ms 120 --window-ms 600 --dirty-threshold 3
./rvv_lease_ctl set --cpu-map 7,6,5,4,3,2,1,0
./rvv_lease_ctl reset
```

## N 模型 Phase Hint

通过旧兼容参数 `--tgid/--tgid2` 启动时，两组模型仍按每组 8 个 worker 配置，并
发布 `IDLE`、`PREFILL`、`DECODE` 或 `MIXED` phase。N 模型或不同线程数使用前文的
重复 `--team` 参数。可用模式：

```text
--team-gang off
--team-gang observe
--team-gang always
--team-gang prefill-lease
--team-gang pair-gang
```

`prefill-lease` 根据 phase hint 在最多八组 per-team/per-CPU VDSQ 之间优先消费
PREFILL/MIXED team。当前推荐模式关闭 local-wakeup fast path，使普通 vector
wakeup 重新经过 VDSQ 和 phase 选择；preferred worker 唤醒时仍可定向抢占同核
其他组的 vector worker。它不抢占普通任务、kworker 或内核线程。`always` 保留为
旧双模型 gang 策略，只使用前两个 team。

`pair-gang` 仅用于 x86 4C/8T 构建。它要求偶数个 team，把相邻两个 `t4` team
分别放到 CPU 0-3 和 SMT sibling 4-7，并按共享时间窗整体轮转 pair。RISC-V 构建会
明确拒绝该模式。

示例：

```bash
./scx_llm_pin --tgid "$MODEL_A_PID" --tgid2 "$MODEL_B_PID" \
  --strategy rvv_dirty --vector-slice-ms 100 \
  --vector-local-window-ms 500 --dirty-threshold 2 \
  --vector-local-wakeup off --phase-wakeup-preempt on \
  --user-control active --team-gang prefill-lease \
  --prefill-starvation-ms 1000
```

`always` 是 loose gang 消融策略；`observe` 只采样 phase。正式 benchmark 应明确指定
模式，不能把这些实验结果和默认 dirty lease 自动合并。

## 验证

```bash
make
./scx_llm_pin --help

cd ../benchmark
./benchmark.sh --list-strategies
./benchmark.sh --strategy scx_global --concurrency-levels 2 --loops 1 \
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
