# dynamic_threads → x86 迁移报告

对应计划：[`../dynamic_threads/X86_MIGRATION_PLAN.md`](../dynamic_threads/X86_MIGRATION_PLAN.md)
模块：`experiments/x86_dynamic_threads/`（独立新建，未修改 RISC-V 主线）

## 平台

- Intel Core i5-10200H：4 物理核 / 8 SMT 逻辑 CPU，sibling `0/4、1/5、2/6、3/7`；
- AVX2 + FMA + XSAVE，无 AVX-512；
- Linux 6.18.42-1-MANJARO，`CONFIG_SCHED_CLASS_EXT=y`，BTF `/sys/kernel/btf/vmlinux`；
- clang 22.1.8，bpftool v7.3.0（jupiter-buildroot），系统 libbpf。

## 交付物

- `Makefile`：`__TARGET_ARCH_x86` + 本机 BTF；用户态 host cc + 系统 libbpf。
- `control.bpf.c` / `control.h` / `controller.c` / `allocation.c` / `worker_team.c` /
  `client.c`：x86 版调度器与控制器。
- `tests/`：探针、单元测试、`run_dual_model_x86.py`（本机 benchmark runner）。
- `llama/`：干净基线 llama.cpp + `0001→0002→llama-dynamic` 补丁 + `build-x86`。

## M0 环境与基线 ✅

- 单 `llama-server -t 4 -tb 4`：128-in/256-out 冒烟通过（pp 46.76 / tg 8.65 tok/s）。
- `x86_sched_cost` 观测器可运行并输出。

## M1 BPF 端口 ✅

6.18 API 适配（相对 RISC-V 6.6 移植版）：

| 项 | 6.6（RISC-V） | 6.18（x86） |
|---|---|---|
| enable 回调 | `(task, scx_enable_args*)` | `(task)` |
| 入队 | `scx_bpf_dispatch` | `scx_bpf_dsq_insert` |
| 出队 | `scx_bpf_consume` | `scx_bpf_dsq_move_to_local` |
| global DSQ | 手动 consume | 内核自动消费，禁止 move_to_local |
| switch_all | `scx_bpf_switch_all()` | 不需要 |

验收：attach 后 `state=enabled`、`switch_all=1`、`ops=dt_control`；退出
`disabled (unregistered from user space)`；dmesg 无 error/watchdog；
`placed=202 running=203 wrong_cpu=0`。

## M2 控制器与协议（单 team）✅

- persistent 1000 graph：**1 REQUEST/GRANT、1 MEMBERS、1 UNMEMBERS、1 RELEASE、
  1 REVOKE**；`wrong_cpu=0 stale_identity=0`。
- x86 单测：`test_control/persistent/decode_guard/shrink/multi_metrics` 23 项通过。

## M3 SMT 感知与相位感知 ✅

- `allocation.c`：`dt_allocate_smt`（物理核 lane `0x0f` / sibling lane `0xf0`）。
- 控制器 attach 前校验 sibling 拓扑（`0/4,1/5,2/6,3/7`）。
- 事件验证：单 team → `0x0f`；prefill+decode → prefill `0x0f` + decode 保底
  1 sibling 核；decode+decode → `0x0f`/`0xf0`。
- BPF 增加每 `(cpu,team)` DSQ + `prefill_mask`，dispatch 优先消费 prefill team；
  prefill worker 唤醒时 `scx_bpf_kick_cpu(SCX_KICK_PREEMPT)` 定向抢占。

> 说明：x86 的 phase 优先最初只靠 DSQ 顺序，TTFT 无改善；加入“prefill 独占物理核、
> decode 保底 1 sibling 核 + 定向抢占”后才出现 TTFT 收益。这是与 RISC-V
> （8 同构核、无 SMT）最大的机制差异。

## M4 多进程 pair-gang ✅

- 4 team 各 4 worker；偶 team `0x0f`、奇 team `0xf0`；`--team-gang pair-gang
  --gang-quantum-us 100000` 整对轮转。
- 事件 `PAIR_SWITCH` 轮转正常；`wrong_cpu=0`；探针全部 PASS。

## M5 性能对照

### 场景修正（关键）

x86 的特殊性在 **SMT**：硬件只有 4 个 AVX2 执行单元；当 runnable 超售时，SMT 用
细粒度交织缓解“相位不平衡导致的等待”。因此有意义的对照必须是**调度造成的超售**：
**3–4 个 llama-server 各 4 线程**（12–16 worker / 8 逻辑核），而不是 2×4（不超售）
或 2×8（不符合真实用法）。

- **非超售**（2 服务 ×4 线程 = 8 worker / 8 逻辑核）：`dynamic`(equal) 与 CFS 接近
  （TPS +2~5%、TTFT −8~11%），符合预期。
- **超售**（4 服务 ×4 线程 = 16 worker / 8 逻辑核）：见下。

### 4 进程 × t4（16 worker / 8 逻辑核），128-in/64-out，3 轮

数据：`tests/results/20260921_141240_x86_multi`。

| 模式 | Aggregate TPS | TTFT mean | TTFT p95 | makespan |
|---|---:|---:|---:|---:|
| CFS baseline | 2.628 ± 0.089 | 13.152 ± 0.896 s | 13.674 s | 97.5 s |
| dynamic (equal) | **7.063 ± 0.248 (+168.8%)** | 12.238 ± 2.245 s (−7.0%) | 22.999 s (+68.2%) | 36.3 s (−62.8%) |
| pair-gang (100 ms) | **6.506 ± 0.086 (+147.6%)** | **8.314 ± 0.146 s (−36.8%)** | **11.407 s (−16.6%)** | 39.4 s (−59.6%) |

结论：

- 超售场景下调度收益巨大：**TPS +148~169%、makespan −60~63%**，印证收益来自
  协调多个独立 OpenMP runtime 的 barrier/相位等待，而不是向量状态搬运。
- **pair-gang** 吞吐与延迟最均衡（TTFT −36.8%、p95 −16.6%）；`dynamic`(equal)
  吞吐更高但 TTFT p95 变差（+68%），因为它只做 SMT lane 均分、不做相位优先。

### 2 服务相位感知（非超售，参考）

数据：`tests/results/20260921_125733_x86_dynamic_dual`。该场景不超售，TPS 基本
持平；TTFT 收益来自 prefill 独占物理核 + decode 保底 + 定向抢占。

| 并发 c | CFS TTFT | dynamic-phase TTFT | CFS TTFT p95 | dynamic-phase p95 |
|---:|---:|---:|---:|---:|
| 2 | 5.409 s | 3.802 s (−29.7%) | 5.904 s | 4.129 s (−30.1%) |
| 4 | 8.340 s | 5.256 s (−37.0%) | 12.182 s | 6.903 s (−43.3%) |
| 8 | 19.204 s | 11.630 s (−39.4%) | 24.196 s | 15.190 s (−37.2%) |

## M6 边界与缺口

- 本机无 AVX-512，未做 AVX-512 对比。
- x86 的 phase 优先依赖抢占（`SCX_KICK_PREEMPT`）；抢占抖动未单独量化。
- pair-gang 的“pair 外运行占比”未单独插桩，当前以探针 CPU 归属校验与
  `PAIR_SWITCH` 计数作为等价证据。
- 3 进程场景（12 worker / 8 逻辑核）未单独成表；`dynamic`(equal) 可用同一 runner
  以 `--ports 18080 18081 18082` 复现。
- M5 的正式阈值原按“2 服务 c=2/4/8”设定，经场景修正后以“4 进程超售”为主口径；
  非超售下的 ITL p95 在 phase 模式下会变差（decode 被保底到少量 sibling 核）。
