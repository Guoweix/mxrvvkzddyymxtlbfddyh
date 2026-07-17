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

控制接口只在启用 `--user-control observe|active` 时创建。loader 将配置和 worker map pin 到 bpffs；llama.cpp 通过 `GGML_SCX_LEASE_HINT_PATH` 在 OpenMP graph 开始时连接一次，每个 worker 仅在 session/TID 变化时注册。BPF 只在首次 dirty 分类时读取一次 worker map，后续 enqueue/dispatch 继续走原 dirty lease 路径。

```bash
# 默认 active 模式不改变已验证的自然唯一 CPU 分配
./scx_llm_pin --tgid "$LLM_PID" --strategy rvv_dirty \
  --llm-cpus 0-7 --system-cpus 0-7 \
  --vector-cpus 0-7 --ordinary-cpus 0-7 \
  --vector-slice-ms 100 --vector-local-window-ms 500 \
  --dirty-threshold 2 --user-control active \
  --user-cpu-map auto

# 查看或原子更新在线策略
./rvv_lease_ctl status
./rvv_lease_ctl set --slice-ms 120 --window-ms 600 --dirty-threshold 3

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
