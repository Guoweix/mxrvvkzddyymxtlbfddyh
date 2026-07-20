# scx_llm_pin

`scx_llm_pin` 是面向 8 核 RISC-V llama.cpp 负载的 sched_ext 实验调度器。
当前只保留 global、RVV dirty 和 RVV dirty local 三种代码路径。调度器接管全部
任务，不再提供旧 CPU 隔离、固定 CPU mask、LLM-only、long slice 或 6+2 fallback
策略。

## 当前策略

| Loader strategy | 行为 |
|---|---|
| `global` | 所有任务进入 sched_ext global DSQ，作为最小 sched_ext 对照 |
| `rvv_dirty` | 根据 `sstatus.VS=Dirty` 识别 vector worker，使用全 8 核唯一 CPU 分配和 per-team/per-CPU vector DSQ |
| `rvv_dirty_local` | vector worker 与 `rvv_dirty` 相同；目标 LLM 的非 vector 辅助线程进入 local DSQ |

`vector-local-window-ms=0` 对应旧 RVV dirty 行为；非零窗口启用 dirty lease。当前
benchmark 的 dirty lease 默认参数是 100 ms vector slice、500 ms local window 和
dirty threshold 2。

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

# RVV dirty lease
./scx_llm_pin --tgid "$LLM_PID" --strategy rvv_dirty \
  --vector-slice-ms 100 --vector-local-window-ms 500 --dirty-threshold 2

# RVV dirty lease，辅助线程走 local DSQ
./scx_llm_pin --tgid "$LLM_PID" --strategy rvv_dirty_local \
  --vector-slice-ms 100 --vector-local-window-ms 500 --dirty-threshold 2
```

主要参数：

```text
--tgid PID
--tgid2 PID
--strategy global|rvv_dirty|rvv_dirty_local
--vector-slice-ms MS
--vector-local-window-ms MS
--dirty-threshold N
--duration SEC
```

调度器始终调用 `scx_bpf_switch_all()`。停止 loader 会 detach struct_ops，并让任务回到
内核默认调度器。

## 用户态 Dirty Lease 控制

用户态控制要求带 sched hint 补丁的 llama.cpp。单模型可选择 observe 或 active：

```bash
./scx_llm_pin --tgid "$LLM_PID" --strategy rvv_dirty \
  --vector-slice-ms 100 --vector-local-window-ms 500 \
  --user-control observe

./scx_llm_pin --tgid "$LLM_PID" --strategy rvv_dirty \
  --vector-slice-ms 100 --vector-local-window-ms 500 \
  --user-control active --user-cpu-map auto
```

- `off`：不创建控制 map，只使用 RVV dirty 识别。
- `observe`：读取和校验 OpenMP worker 注册，不改变 CPU placement。
- `active` + `auto`：允许在线参数控制，仍使用自然唯一 CPU 分配。
- `active` + 显式 map：按 8 个唯一 CPU 的顺序覆盖 `ith -> CPU`。

控制 map 默认位于 `/sys/fs/bpf/scx_rvv_dirty_lease`。在线查看和修改：

```bash
./rvv_lease_ctl status
./rvv_lease_ctl set --slice-ms 120 --window-ms 600 --dirty-threshold 3
./rvv_lease_ctl set --cpu-map 7,6,5,4,3,2,1,0
./rvv_lease_ctl reset
```

## 双模型 Phase Hint

传入 `--tgid2` 后，两组模型分别注册 8 个 worker，并发布 `IDLE`、`PREFILL`、
`DECODE` 或 `MIXED` phase。可用模式：

```text
--team-gang off
--team-gang observe
--team-gang always
--team-gang prefill-lease
```

`prefill-lease` 保留 dirty lease 的 vector slice 和 local window，只在 vector DSQ
选择时偏向 PREFILL/MIXED team，并允许 wakeup 的 prefill worker 抢占同核另一组
vector worker。它不抢占普通任务、kworker 或内核线程。

示例：

```bash
./scx_llm_pin --tgid "$MODEL_A_PID" --tgid2 "$MODEL_B_PID" \
  --strategy rvv_dirty --vector-slice-ms 100 \
  --vector-local-window-ms 500 --dirty-threshold 2 \
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
