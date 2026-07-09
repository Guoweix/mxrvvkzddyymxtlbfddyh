# scx_llm_pin - CPU Affinity Pinning Scheduler for LLM Workloads

## 概述

`scx_llm_pin` 是一个基于 Linux sched_ext 框架的 eBPF 调度器，专门用于优化 LLM（大语言模型）推理场景的上下文切换开销。

### 核心功能

- **CPU 亲和性绑定**：将 `llama-server` 工作线程固定到指定的 CPU 核心组
- **隔离系统任务**：防止其他进程干扰 LLM 推理
- **避免跨核迁移**：减少 cache miss 和 RISC-V Vector 状态保存/恢复开销
- **统计反馈**：实时显示调度决策和 CPU 迁移次数

### 目标

通过减少上下文切换次数和跨核迁移，降低：
- RISC-V Vector (RVV) 寄存器状态保存/恢复次数
- L1/L2 cache miss 率
- 调度器开销

预期提升 LLM 推理吞吐量 **10-20%**。

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
ssh root@192.168.3.63

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
3. **队列分发**：所有任务分发到全局队列 (`SCX_DSQ_GLOBAL`)
4. **时间片**：使用默认时间片 (`SCX_SLICE_DFL`)，约 6ms

### BPF 程序结构

- **`select_cpu()`**：CPU 选择回调，实现亲和性绑定逻辑
- **`enqueue()`**：任务入队回调，分发到全局队列
- **`dispatch()`**：任务调度回调，消费全局队列
- **`init()`**：初始化回调，调用 `scx_bpf_switch_all()` 接管所有任务

### BPF Maps

- **`config_map`**：用户态写入配置（TGID、CPU mask）
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
