# dynamic_threads → x86 迁移计划

状态：**计划（未执行）**
范围：把当前 RISC-V 主线调度器 `dynamic_threads`（用户态控制器 + Unix socket
授权 + persistent grant + 相位感知 + 多进程）迁移到 x86（Intel i5-10200H，
4 物理核 / 8 SMT 逻辑 CPU，Linux 6.18 + sched_ext）。

---

## 1. 目标与非目标

### 1.1 目标
1. 在 x86 上复现 dynamic 的完整功能链：BPF 放置 + 用户态授权 + 持续授权 +
   相位感知（prefill 优先 / decode 保底）+ 多实例（2–4，最多 8）。
2. 形成与 RISC-V 同口径的对照数据（TPS / TTFT / ITL / Jain），支撑比赛
   “与 x86 横向对比”的扩展要求。
3. 保持单一配置入口（`experiments/benchmark/config.sh`）与“源码可复现”原则：x86 profile 下
   零环境变量即可构建 + 运行。

### 1.2 非目标
- **不修改 `experiments/dynamic_threads/`（RISC-V 主线）**：x86 迁移在**新目录**
  `experiments/x86_dynamic_threads/` 中进行，RISC-V 侧代码与数据保持不变。
- 不重做 `experiments/x86_scx_llm`（旧 phase-aware/scx_llm_pin 的 x86 移植）已有
  结论；本计划是**另一条主线（dynamic）**的迁移。
- 不在本机伪造 AVX-512 数据（本机无 AVX-512）。

---

## 2. 现状与差距

### 2.1 dynamic 现有组件（开发树 `experiments/dynamic_threads/`；提交仓库为 `src/dynamic_threads/`）
| 组件 | 作用 | x86 迁移关注点 |
|---|---|---|
| `control.bpf.c` | sched_ext 放置：`grants[]`/`members{}`、per-CPU DSQ、`start_boottime` 身份校验 | 6.18 kfunc API 兼容、`stopping` 重分类 |
| `controller.c` | epoll 事件循环、授权仲裁、`serve_waiters()`、策略模式 | 本机 socket、拓扑/掩码语义 |
| `allocation.c/h` | 实例集合 → 目标 CPU 向量、容量上限、保底 | 4 物理核 + SMT sibling 语义 |
| `client.c/h` + `control.h` | 线协议 v4（112B）：HELLO/ACTIVITY/ACQUIRE/MEMBERS/UNMEMBERS/RELEASE/GRANT/REVOKE/PHASE/SHRINK | 基本可移植 |
| `worker_team.c/h` | worker/affinity 生命周期（缩核保留） | worker 宽度改 4 |
| `llama/export_llama_patch.sh` | 导出 llama-dynamic 补丁 | 补丁可移植，x86 用 AVX2 构建 |

### 2.2 x86 现有资产（开发树 `experiments/`）
- `x86_scx_llm/`：**旧 phase-aware（scx_llm_pin）x86 移植**；`Makefile` 用
  `-D__TARGET_ARCH_x86`、BTF 取 `/sys/kernel/btf/vmlinux`；`--x86-smt adaptive`、
  `--team-gang pair-gang`、`--control-dir /sys/fs/bpf/scx_x86_llm`。
- `x86_sched_cost/`：AVX/XSTATE 观测器 + runner（`run_dual_model.py` 的
  `baseline/phase/smt/ebpf/cfs4/observe`、`run_single_model.py`、`run_pair_gang.py`）。
- 文档：`RESULTS_20260912.md`（x86 sched_ext 第一版 + SMT 自适应）、
  `PAIR_GANG_20260912.md`、`THREAD_SCAN_20260912.md`、`BARRIER_OVERLOAD_20260912.md`、
  `SINGLE_VS_DUAL_20260912.md`、`RESULTS_20260911.md`（AVX/XSTATE 成本）。

### 2.3 差距
| 维度 | RISC-V 现状 | x86 现状 | 差距 |
|---|---|---|---|
| 调度器 | dynamic（本计划对象） | 仅旧 scx_llm_pin 移植 | **无 dynamic x86 版** |
| 拓扑 | 8 同构核（无 SMT） | 4 物理核 + 8 SMT | 需 SMT 感知分配 |
| worker 宽度 | 8（`-t 8 -tb 8`） | 4（`-t 4 -tb 4` 最优） | 宽度与掩码语义 |
| sched_ext | K1 Linux 6.6（移植版） | Linux 6.18（主线） | kfunc API 差异 |
| 执行方式 | 远端板卡（sshpass 部署） | 本机 | 测试 harness 需本地化 |

---

## 3. x86 特殊性（设计依据）

1. **SMT 是首要变量**：sibling 对 `0/4、1/5、2/6、3/7`；`-t 4` 已吃满 AVX2/缓存/
   访存，`-t 8` 反而下降（pp −6.1%、tg −7.3%）。
2. **放置必须 SMT 感知**：
   - 单 team → CPU 0–3（每物理核一个 worker）；
   - prefill + decode 重叠 → 双方都 0–3，由相位 lease 优先 prefill，**避免
     sibling 上的 AVX2 竞争**；
   - decode + decode → team0 0–3、team1 4–7（此时才用满 SMT lane）。
3. **超售场景才体现长 slice 收益**：8 worker 挤 4 逻辑核时，libgomp
   `gomp_team_barrier_wait_end` bounded-spin 占比 86% → 分组 DSQ + 长 slice 后
   ~5%，TPS +560%（256-token/400ms）。若两个 4-worker team 各占满 4 物理核，
   则无“调度视角超售”，瓶颈转为 SMT 执行资源竞争。
4. **AVX/XSTATE 不是瓶颈**：FPU save/restore 仅约 workload 的 0.028%。
5. **6.18 sched_ext 差异**：`scx_bpf_dsq_move_to_local()` 仅可用于自定义 DSQ，
   传 `SCX_DSQ_GLOBAL` 会触发 runtime error 并卸载；global DSQ 由内核自动消费；
   新 worker 首次 `enqueue` 后才发布身份，需在 `stopping` 重分类。
6. **pair-gang**（≥4 个超售 team）：偶 team→0–3、奇 team→4–7，按整对轮转
   （100 ms 默认）；四模型 64-token 相对 CFS：TPS +143.9%、ITL p95 −71.5%。
7. **本机无 `perf`**（`perf_event_paranoid=2`）：观测用自计时 eBPF。
8. **比赛扩展**：x86 vs RVV 的向量状态切换成本已有合成微基准（AVX2 +27.2 ns vs
   RVV ~500 ns，约 18.4×），迁移后需与端到端数据衔接说明。

---

## 4. 设计决策

### 4.0 代码组织：新建 `experiments/x86_dynamic_threads/`
x86 迁移**不修改** `experiments/dynamic_threads/`（RISC-V 主线），而是新建并列模块
`experiments/x86_dynamic_threads/`：从 dynamic_threads 拷贝源文件后按 x86 改造
（与 `x86_scx_llm`、`x86_sched_cost` 的组织方式一致）。x86 的测试脚本与配置同样
落在该模块内，RISC-V 侧代码、测试与数据保持原样。

拷贝与改造清单（草案）：

| 文件 | 处理 |
|---|---|
| `control.bpf.c` / `control.h` | 拷贝，按 6.18 kfunc API 与 x86 拓扑改造 |
| `controller.c` | 拷贝，本机 socket + SMT 感知仲裁 |
| `allocation.c/h` | 拷贝，改为“物理核 + SMT lane”分配 |
| `client.c/h` | 拷贝，基本不变（协议一致） |
| `worker_team.c/h` | 拷贝，worker 宽度 8 → 4 |
| `Makefile` | 新建，`__TARGET_ARCH_x86`、BTF 取 `/sys/kernel/btf/vmlinux` |
| 测试脚本/探针 | 从 dynamic_threads 拷贝到 `x86_dynamic_threads/tests/` 并本机化 |
| `llama-dynamic.patch` | 复用现有补丁，x86 用 AVX2 构建 |

### 4.1 拓扑抽象
引入“物理核 + SMT lane”抽象：
- 物理核集合 `C = {0,1,2,3}`，sibling 映射 `sib(c) = c+4`；
- 分配单位从“CPU 列表”升级为“物理核列表 + lane 选择”；
- attach 前校验 `/sys/devices/system/cpu/cpu{0..3}/topology/thread_siblings_list`
  确实为 `0/4、1/5、2/6、3/7`，否则拒绝启动。

### 4.2 分配策略（x86 profile）
| 活跃相位组合 | team0 掩码 | team1 掩码 | 说明 |
|---|---|---|---|
| 单 team | `0x0f` | — | 4 物理核 |
| prefill + decode | `0x0f` | `0x0f` | 相位 lease 优先 prefill |
| decode + decode | `0x0f` | `0xf0` | 启用 SMT lane |
| ≥4 team（pair-gang） | 偶 `0x0f` / 奇 `0xf0` | — | 整对轮转，quantum 100 ms |

### 4.3 BPF 兼容层
- `control.bpf.c` 增加 x86 编译分支：`__TARGET_ARCH_x86`、BTF 取本机；
- 移除/改写 6.18 不兼容调用（尤其 `move_to_local` 只对自定义 DSQ）；
- 保留 `start_boottime` 身份校验；
- 新增 `stopping` 重分类路径（首次 enqueue 后发布身份）。

### 4.4 运行时与补丁
- `llama-dynamic.patch` 保持可移植；x86 用 AVX2 构建（`-t 4 -tb 4`）；
- socket 改为本机路径（`GGML_DYNAMIC_THREADS_SOCKET=/tmp/...`）。

### 4.5 测试与配置
- `experiments/benchmark/config.sh` 增加 **x86 profile**（本机：`REMOTE_*` 置空、`TOOLCHAIN_ROOT`
  指向本机、`LLAMA_CPU_SRC`/`DT_LLAMA_BIN` 指向 x86 构建）；
- runner 去掉“远端部署”路径，改为本机直接启动（可复用 `x86_sched_cost` 的
  runner 结构）。

---

## 5. 分阶段计划与验收标准

> 每阶段必须通过“功能门禁”才算完成；性能验收在 M5 统一执行。

### M0 环境与基线
- 交付：新建 `experiments/x86_dynamic_threads/` 模块骨架（从 dynamic_threads
  拷贝并标注待改造点）；x86 构建 profile；llama.cpp AVX2（含 dynamic 补丁）构建
  产物；`x86_sched_cost` 观测器可运行。
- 验收：
  - `uname -r` 为 6.18.x，`/sys/kernel/sched_ext` 存在，`/sys/kernel/btf/vmlinux` 可读；
  - `clang --version`、`bpftool` 可用；
  - 单 `llama-server -t 4` 可跑通 128-in/256-out；
  - 观测器能输出 `sched_switch`/futex/FPU save-restore 计数。

### M1 BPF 端口（先 fixed 策略）
- 交付：`control.bpf.c` x86 版本 + skeleton，可在 6.18 加载并 attach。
- 验收：
  - attach 后 `/sys/kernel/sched_ext/state=enabled`、`switch_all=1`；
  - 退出后回到 `disabled`，退出原因正常（`unregistered from user space`）；
  - 内核日志无 runtime error / watchdog / BUG / Oops；
  - fixed 放置下探针任务的 `wrong_cpu=0`。

### M2 控制器与协议（单 team、graph lifetime）
- 交付：本机 socket 的 `dt_controller` + `client`；`ACQUIRE/MEMBERS/UNMEMBERS/
  RELEASE` 全链路。
- 验收（对齐 RISC-V 控制门禁口径）：
  - 1000 个 graph 仅 1 次授权 + 1 次成员发布；
  - 授权不重叠；成员 TID 身份校验通过；
  - `wrong_cpu=0`；内核干净；
  - 断链隔离、过期代次拒绝、取消等待合并等既有单测在 x86 上通过。

### M3 SMT 感知与相位感知
- 交付：`allocation.c` SMT 感知分配 + 相位感知（prefill 优先/decode 保底）。
- 验收：
  - 拓扑校验生效（sibling 映射不符即拒绝）；
  - 单 team 用 `0x0f`；prefill+decode 双方 `0x0f`；decode+decode 为 `0x0f`/`0xf0`；
  - 调度计数中 primary / sibling / latency-primary / decode-shared 四类路径
    均非零（对齐 `x86_scx_llm` 的验证口径）；
  - 相位切换在 graph 边界完成，无跨相位错放。

### M4 多进程与 pair-gang
- 交付：2/4 team 的 persistent + 相位感知；pair-gang 整对轮转。
- 验收：
  - 4 team 各 4 worker 全部识别，DSQ 入队/消费计数平衡；
  - pair 内运行占比 ≥ 98%（pair 外运行集中在换对边界）；
  - pair switch 次数与 quantum 一致；
  - 推理结束前 sched_ext 始终 `enabled`。

### M5 性能对照（正式）
- 交付：CFS vs dynamic（equal / 相位感知）在 c=2/4/8、128-in/256-out、异步注入
  下各 ≥3 轮；输出 TPS/TTFT/p95/ITL p95/Jain。
- 验收阈值（暂定，可据基线校准）：
  - 相位感知相对 CFS：injected TTFT p95 **≤ −30%**；Aggregate TPS **不低于 −5%**；
    ITL p95 **≤ −30%**；Jain **≥ 0.99**；
  - equal 对照：TPS 提升但 TTFT 不恶化超过 +25%（用于说明相位感知必要性）；
  - pair-gang（4 team）：TPS 相对 CFS **≥ +50%**（对齐已有 +143% 量级的方向性）；
  - 所有轮次通过内核干净检查，无效 attempt 不入汇总。

### M6 文档与比赛材料
- 交付：x86 迁移报告（方法、参数、原始数据、结论与边界）；与 AVX/XSTATE 微基准
  的衔接说明。
- 验收：
  - 报告含可复现命令与原始结果路径；
  - 明确 AVX-512 缺口与“整机/内核组合差异不可直接归因状态字节数”的边界。

---

## 6. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| 6.18 kfunc/API 差异 | BPF 加载失败、scheduler 被卸载 | 参考 `x86_scx_llm` 兼容层；先 fixed 后 phase；禁止对 global DSQ 调 `move_to_local` |
| SMT 竞争抵消 prefill 收益 | TTFT 改善不显著 | 只在 decode+decode 启用 sibling；prefill+decode 固定 0–3 |
| 本机无 `perf` / 无 AVX-512 | 观测受限、对比不完整 | 自计时 eBPF；AVX-512 留缺口并在报告说明 |
| 超售语义与 K1 不同 | 结论不可比 | 明确“runnable 超售”定义（worker 数 vs 逻辑 CPU 数），并同时报告超售/非超售两组 |
| 拓扑校验失败 | attach 拒绝 | 提前读 topology，失败即报错退出 |
| 本机执行 vs 远端部署 | harness 需改造 | x86 profile 本地化；抽象 Remote 层 |
| llama.cpp x86 补丁冲突 | 运行时 hint 不生效 | 按 0001→0002→llama-dynamic 顺序应用并验证 hint 连接日志 |

---

## 7. 验收清单（Checklist）

- [ ] M0：内核 6.18 + sched_ext + BTF 可用；llama.cpp AVX2 可跑
- [ ] M1：BPF attach/detach 正常；`wrong_cpu=0`；无 runtime error/watchdog
- [ ] M2：1000 graph → 1 grant / 1 member；授权不重叠；断链/代次/取消单测通过
- [ ] M3：SMT 拓扑校验；三类相位放置正确；四类选择路径计数非零
- [ ] M4：4 team pair-gang；DSQ 平衡；pair 外运行 < 2%；sched_ext 全程 enabled
- [ ] M5：c=2/4/8 ≥3 轮；TTFT p95 ≤ −30%；TPS ≥ −5%；ITL p95 ≤ −30%；Jain ≥ 0.99
- [ ] M6：迁移报告 + 原始数据 + 边界说明；AVX-512 缺口记录
- [ ] 回归：RISC-V 侧 45 项单测与 control gate 仍通过
- [ ] 复现：仅改 `experiments/benchmark/config.sh` 即可构建 + 运行

---

## 8. 附录

### 8.1 关键参数（x86 profile 草案）
```
# experiments/benchmark/config.sh (x86 profile)
TOOLCHAIN_ROOT=/usr            # 本机工具链（clang/bpftool 来自系统或指定目录）
LLAMA_CPU_SRC=<x86 llama.cpp>/ggml/src/ggml-cpu
DT_LLAMA_BIN=<x86 llama.cpp>/build-x86/bin
REMOTE_IP=                     # 本机执行，空
LLM_THREADS=4                  # -t 4
LLM_THREADS_BATCH=4            # -tb 4
```

### 8.2 关键命令（草案）
```bash
# 构建（x86 新模块）
make -C experiments/x86_dynamic_threads all
# 本机控制门禁（需先本机化 runner）
python3 experiments/x86_dynamic_threads/run_control_gate.py --grant-lifetime persistent \
  --allocation-policy prefill-first --decode-wait-ms 0 --decode-floor-on-prefill \
  --iterations 200
# 性能对照
python3 experiments/x86_dynamic_threads/run_benchmark.py --cases cfs_baseline,scx_dynamic_persistent,scx_dynamic_persistent_phase \
  --scenarios asynchronous --concurrencies 2,4,8 --loops 3 --output-tokens 256
```

### 8.3 参考文档
- `experiments/x86_scx_llm/README.md`、`RESULTS_20260912.md`
- `experiments/x86_sched_cost/{README,PAIR_GANG_20260912,THREAD_SCAN_20260912,BARRIER_OVERLOAD_20260912,SINGLE_VS_DUAL_20260912,RESULTS_20260911}.md`
- 开发树 `experiments/dynamic_threads/{README,PLAN,PERSISTENT_*,PHASE_*,DECODE_GUARD_*,MULTI_PROCESS_*,WORKER_RETENTION}.md`
