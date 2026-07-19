# scx_llm_pin - CPU Affinity Pinning Scheduler for LLM Workloads

## 概述

`scx_llm_pin` 是一个基于 Linux sched_ext 的实验调度器，包含 CPU placement、local DSQ 和 RVV dirty lease 等策略。当前 LLM 主策略通过 RVV dirty 状态识别 8 个 OpenMP 计算 worker，为它们分配互不重复的 CPU，并用 vector DSQ、100ms slice 和 500ms local lease 协调运行顺序。

### 核心功能

- **CPU 亲和性绑定**：将 `llama-server` 工作线程固定到指定的 CPU 核心组
- **隔离系统任务**：防止其他进程干扰 LLM 推理
- **避免跨核迁移**：减少 cache miss 和 RISC-V Vector 状态保存/恢复开销
- **用户态控制**：可选的 OpenMP worker 注册、observe/active 模式、pinned config 和控制 CLI
- **统计反馈**：实时显示调度决策、RVV worker、DSQ 和用户提示统计

### 目标

通过减少上下文切换次数和跨核迁移，降低：
- RISC-V Vector (RVV) 寄存器状态保存/恢复次数
- L1/L2 cache miss 率
- 调度器开销

性能收益取决于负载相位和参数，必须用同口径多轮 benchmark 验收；不能仅由 RVV 上下文切换次数推导。

---

## 编译

### 前置条件

- Linux 内核 6.6+ 并启用 `CONFIG_SCHED_CLASS_EXT=y`
- Linux 内核 BTF 支持 (`CONFIG_DEBUG_INFO_BTF=y`)
- Buildroot 交叉编译环境（已配置 RISC-V 工具链）
- `clang`、`llvm` 用于编译 BPF 程序
- `bpftool` 用于生成 skeleton

### 编译步骤

```bash
cd experiments/scx_llm_pin

# 1. 从板卡获取 BTF 信息（首次需要）
make fetch-btf

# 2. 生成 vmlinux.h（首次需要）
make host-bpftool
make vmlinux.h

# 3. 编译完整程序
make

# 4. 部署到板卡
make deploy
```

---

## 使用方法

### 基本用法

```bash
# 在板卡上运行
ssh root@192.168.3.67

# 查找 llama-server PID
ps aux | grep llama-server

# 启动调度器（LLM 使用 CPU 0-3，系统使用 CPU 4-7）
cd /tmp/scx_llm_pin
./scx_llm_pin --tgid <llama-server-pid> --llm-cpus 0-3 --system-cpus 4-7
```

### 完整参数

```
Usage: scx_llm_pin [options]

Options:
  --tgid <pid>           目标进程 TGID（llama-server 的 PID）[必需]
  --llm-cpus <range>     LLM 专用 CPU 范围（如 0-3 或 0,1,2,3）[必需]
  --system-cpus <range>  系统进程 CPU 范围（如 4-7）[可选，默认为非 llm-cpus]
  --duration <sec>       运行时长（秒），0 为无限期（默认：0）
  -h, --help             显示帮助信息

Example:
  ./scx_llm_pin --tgid 1234 --llm-cpus 0-3 --system-cpus 4-7 --duration 60
```

### 实时监控

调度器运行时会每 10 秒打印一次统计信息：

```
=== scx_llm_pin statistics ===
LLM tasks scheduled:    1523
LLM tasks pinned:       1523
System tasks scheduled: 8921
CPU migrations:         12
```

- **LLM tasks scheduled**：LLM 线程被调度的总次数
- **LLM tasks pinned**：成功固定到指定 CPU 的次数
- **System tasks scheduled**：系统线程被调度的总次数
- **CPU migrations**：跨核迁移次数（应该接近 0）

### Dirty Lease 用户态控制

控制接口只在启用 `--user-control observe|active` 时创建。loader 将配置和 worker map pin 到 bpffs；llama.cpp 通过 `GGML_SCX_LEASE_HINT_PATH` 在 OpenMP graph 开始时连接，每个 worker 仅在 session/TID 变化时重写 slot。BPF 在分类阶段有界读取 worker map；成员身份稳定后，常规 enqueue/dispatch 继续走 dirty lease 路径。

传入 `--tgid2` 时启用双模型 ABI v3。worker map 包含两个 4096-byte page，两个模型分别注册 8 个 worker；每页新增一个 64-byte team phase hint。CPU owner、per-CPU vector DSQ 和 local window 都按 `(team, CPU)` 隔离，因此每个模型内部都可以唯一使用 CPU0-7。

patched llama.cpp 会在每次 CPU graph 前发布 `PREFILL`、`DECODE` 或 `MIXED`，graph 返回后发布 `IDLE`。server 通过实验 C API `llama_set_sched_hint()` 标记当前 batch 中的 prompt/decode token 数；未设置 API 时 context 仍可根据 ubatch 自动推断。phase 和 worker slot 都使用 sequence counter，并校验 TGID、team、session、ABI 与 8-worker 数量。

显式 worker 页是计算组成员的首选来源。页尚未完整时调度器最多等待 1 秒，让 OpenMP worker 先运行并注册；完整页中的非成员会被明确排除。只有页缺失、ABI 无效或注册 grace 超时才回退 RVV dirty，因此未打补丁的 llama.cpp 仍可运行。

```bash
# 默认 active 模式不改变已验证的自然唯一 CPU 分配
./scx_llm_pin --tgid "$LLM_PID" --strategy rvv_dirty \
  --llm-cpus 0-7 --system-cpus 0-7 \
  --vector-cpus 0-7 --ordinary-cpus 0-7 \
  --vector-slice-ms 100 --vector-local-window-ms 500 \
  --dirty-threshold 2 --user-control active \
  --user-cpu-map auto

# 双模型 phase 只观测，不改变已验收 dirty lease
./scx_llm_pin --tgid "$MODEL_A_PID" --tgid2 "$MODEL_B_PID" \
  --strategy rvv_dirty --llm-cpus 0-7 --system-cpus 0-7 \
  --user-control active --team-gang observe

# 在自然 500ms lease 边界优先 PREFILL/MIXED，并定向抢占同核异组 vector worker
./scx_llm_pin --tgid "$MODEL_A_PID" --tgid2 "$MODEL_B_PID" \
  --strategy rvv_dirty --llm-cpus 0-7 --system-cpus 0-7 \
  --vector-slice-ms 100 --vector-local-window-ms 500 \
  --user-control active --team-gang prefill-lease \
  --prefill-starvation-ms 1000

# 查看或原子更新在线策略
./rvv_lease_ctl status
./rvv_lease_ctl set --slice-ms 120 --window-ms 600 --dirty-threshold 3

# 在 60 秒内优先消费 team 1 的 vector DSQ；off 可提前取消
./rvv_lease_ctl set --prefer-team 1 --prefer-ms 60000
./rvv_lease_ctl set --prefer-team off

# 显式 ith0..7 -> CPU7..0；已分类 worker 在下次 wakeup 使用新映射
./rvv_lease_ctl set --cpu-map 7,6,5,4,3,2,1,0

# 回到 100ms/500ms/threshold=2 和自然唯一分配
./rvv_lease_ctl reset
```

模式语义：

- `off`：不创建控制 map，不读取用户提示，保持原调度器路径。
- `observe`：验证并缓存 worker 的 `ith/TID/session`，不改变 CPU placement。
- `active` + `auto`：允许在线参数控制，但 CPU placement 仍使用原 dirty lease 自然唯一分配。
- `active` + 显式 map：按用户给出的唯一排列覆盖 `ith -> CPU`；切回 `auto` 后重新进入自然分配。

team gang 模式默认 `off`，均要求双 TGID 和有效用户控制页：

- `off`：完全保留已验收 active-auto dirty lease。
- `observe`：读取并统计 phase，不改变 enqueue/dispatch。
- `always`：两组 graph 同时活跃时始终启用共享 team quantum，用于消融实验。
- `prefill-lease`：保留 100ms vector slice 和 500ms local lease；在下一次自然 vector DSQ 消费时优先唯一的 `PREFILL/MIXED` team。该组 vector worker 因 `SCX_ENQ_WAKEUP` 入队且同核正在运行另一组 vector worker 时，使用 `SCX_ENQ_PREEMPT` 定向抢占；不会抢占普通任务、kworker 或内核线程，也不启用 gang quantum 或全核 kick。

`always` active gang 期间 vector worker 进入各自 `(team, CPU)` DSQ，dispatch 先服务 global/system，再消费当前 team；当前 team 为空会立即回退另一 team，避免严格 gang 导致 CPU 空转或 watchdog。team 切换会 kick 计算 CPU，vector slice 被限制到 gang quantum。该实现是 work-conserving loose gang，不保证 8 个 worker 同时 runnable。

`prefill-lease` 单独记录每个 `(team, CPU)` vector DSQ 的 runnable pending 时间。等待达到 `--prefill-starvation-ms`（默认 1000ms）时，该 worker 在 global 前获得一个 vector slice，但不续 local lease；参数限制为 100..4000ms。加入定向 wakeup 抢占后的三轮 256-token 测试相对 observe 为 TPS -1.17%、run-level mean TTFT 均值 -17.35%、TTFT max -4.24%、上下文切换 +11.24%；其中 mean TTFT 受一个批处理完成顺序改变的样本显著影响。安全门禁通过，但性能与稳定性门槛未全部通过，不能替换默认 dirty lease。完整结果见 `docs/rvv_phase_lease_wakeup_preempt_20260719.md`。

`--gang-sample-us 1000` 可请求采样每个 CPU 当前运行的 team，并输出 `SCX_GANG_SAMPLES`；默认关闭，因为高频用户态轮询不应混入正式性能数据。`SCX_GANG_STATS` 始终提供机器可解析的 phase、gang、worker 和 DSQ 摘要。`rvv_lease_ctl status` 会显示 gang 配置及两页当前 phase。

preferred team 是带 TTL 的低频实验提示。它只在 global/system DSQ 为空后改变两个 vector DSQ 的消费顺序，preferred DSQ 为空时立即回退；不会改变任务入队位置，也不会抑制另一 team 的 local lease。正式双模型测试未证明它有 TPS 收益，因此默认关闭。

在线 `--window-ms` 可用于 phase/injection 实验。256-token 双模型正式测试中，注入时从 500ms 改为 1000ms 虽将 vector DSQ consume 减少约 43%，TPS 却下降 0.34%；推荐默认仍为 500ms。完整结果见 `docs/rvv_dirty_lease_dual_model_userspace_20260718.md`。

提示页缺失、session 不匹配或 worker slot 无效时，调度器只增加 fallback 统计，仍靠 RVV dirty 识别和原 lease 路径运行，不会依赖用户态继续推进。

---

## 验证效果

### 1. 启动 LLM 推理服务

```bash
PORT=9091 THREADS=8 PARALLEL=1 CTX=1024 /mnt/llm/llama/run-server.sh
```

### 2. 记录 LLM PID

```bash
LLM_PID=$(pgrep llama-server)
echo "LLM PID: $LLM_PID"
```

### 3. 采集基线数据（无调度器）

```bash
cd /tmp/rvv_sched_cost
./rvv_sched_cost --tgid $LLM_PID --duration 60 --json baseline.json

# 运行 LLM 推理请求（60秒）
# ...
```

### 4. 启动调度器

```bash
cd /tmp/scx_llm_pin
./scx_llm_pin --tgid $LLM_PID --llm-cpus 0-3 --system-cpus 4-7 &
```

### 5. 采集优化后数据

```bash
cd /tmp/rvv_sched_cost
./rvv_sched_cost --tgid $LLM_PID --duration 60 --json pinned.json

# 运行相同的 LLM 推理请求（60秒）
# ...
```

### 6. 对比结果

关键指标对比：
- `switch_events`：上下文切换总次数（目标：↓ 50-70%）
- `v_save_candidates`：V 状态保存次数（目标：↓ 50-70%）
- `offcpu_avg_ns`：平均 off-CPU 时间（目标：↓ 30-50%）
- `cpu_migrations`（从 scx_llm_pin 统计）：跨核迁移次数（目标：→ 0）

预期结果：
```json
{
  "baseline": {
    "switch_events": 4356,
    "v_save_candidates": 2169,
    "offcpu_avg_ns": 34324804
  },
  "pinned": {
    "switch_events": 1520,      // -65%
    "v_save_candidates": 756,   // -65%
    "offcpu_avg_ns": 22000000,  // -36%
    "improvement": "+12-15% tok/s"
  }
}
```

---

## 技术细节

### 调度策略

1. **任务识别**：通过 `task_struct->tgid` 匹配目标 LLM 进程
2. **CPU 选择**：
   - LLM 线程：从 `llm_cpu_mask` 中选择 CPU（优先保持原 CPU）
   - 系统线程：从 `system_cpu_mask` 中选择 CPU
3. **队列分发**：基础策略使用 global/local DSQ；RVV lease 使用 per-CPU vector DSQ，并在放行后的固定窗口内使用 local DSQ
4. **时间片**：基础策略使用 `SCX_SLICE_DFL`；RVV lease 的 vector slice 和 local window 可配置

### BPF 程序结构

- **`select_cpu()`**：CPU 选择回调，实现亲和性绑定逻辑
- **`enqueue()`**：任务入队回调，分发到全局队列
- **`dispatch()`**：任务调度回调，消费全局队列
- **`init()`**：初始化回调，调用 `scx_bpf_switch_all()` 接管所有任务

### BPF Maps

- **`config_map`**：用户态写入配置（TGID、CPU mask）
- **`user_worker_map`**：可选 mmap worker 注册页，仅在首次 RVV 分类读取
- **`stats_map`**：统计信息（调度次数、迁移次数）
- **`percpu_stats`**：Per-CPU 统计（预留）

---

## 故障排除

### 问题：无法加载 BPF 程序

```
failed to attach sched_ext struct_ops: -22
```

**解决**：
- 检查内核是否启用 sched_ext：`cat /sys/kernel/sched_ext/state`（应显示 `enabled` 或 `disabled`）
- 确认没有其他 sched_ext 调度器正在运行：`cat /sys/kernel/sched_ext/root/ops`

### 问题：统计信息全为 0

**解决**：
- 确认 `--tgid` 参数正确（使用 `ps aux | grep llama-server` 验证）
- 确认 LLM 服务正在运行且有活动
- 检查 BPF 程序日志：`dmesg | tail -50`

### 问题：性能没有提升

**原因**：
- LLM 推理本身可能不受调度限制（如纯计算密集型，无抢占）
- 其他瓶颈：内存带宽、I/O、模型大小

**建议**：
- 先用 `rvv_sched_cost` 确认基线上下文切换次数是否频繁（>50次/秒）
- 如果基线切换次数很低，优化空间有限

---

## 下一步优化

当前调度器仅实现 CPU 亲和性绑定。后续可继续优化：

1. **延长时间片**（阶段2）：减少被动抢占
2. **协作式调度**（阶段3）：优化线程间唤醒路径

详见 `../../PLAN.md`。

---

## 相关工具

- **rvv_sched_cost**：上下文切换观测工具（`../rvv_sched_cost/`）
- **scx_min**：最小化 sched_ext 调度器模板（`../scx_min/`）
- **scx_nop**：空操作调度器（`../scx_nop/`）

---

## 参考文档

- [Linux sched_ext 文档](https://docs.kernel.org/scheduler/sched-ext.html)
- [BPF CO-RE 文档](https://nakryiko.com/posts/bpf-portability-and-co-re/)
- [Milk-V Jupiter 官方文档](https://milkv.io/jupiter)
