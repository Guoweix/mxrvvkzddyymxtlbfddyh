# benchmark 重构分析

本目录是 `experiments/benchmark` 的 Python 重构目标。重构范围：

- 测试场景只保留 3 种：双 llama 异步注入、双 llama 同步注入、多 llama。
- 策略先只保留 `cfs`，后续再适配新策略。
- 语言：Python。

本文件是第一步的阅读与分类结果，尚未开始迁移代码。

## 目标场景与现有实现的对应关系

| 目标场景 | 现实现 | 状态 |
|---|---|---|
| 双 llama 异步注入 | `dual_model/run_benchmark.py --scenario asynchronous` + `dual_model/workload.py` | 存在 |
| 双 llama 同步注入 | 有两个候选：`--scenario simultaneous`（全部请求共享同一释放时刻，spread 只记录）和 `--scenario symmetric`（A/B 成对请求共享释放时刻并有对齐门禁）。需确认“同步注入”指哪一个，或二者是否都要 | 存在（语义待确认） |
| 多 llama | `multi_model/run.py`（1–8 个 server，JSON 配置，同时到达） | **存在**，不是缺失 |

结论：用户提到的“多 llama 测试场景（可能没有）”实际是存在的，位于
`multi_model/`。但它是 ABI v4 / N-team runtime 专用入口，依赖
`rvv_team_exec`、`rvv_lease_ctl`、`scx_llm_pin`，在纯 CFS 下也需要按目标环境裁剪。

## 现有目录/文件分类

### A. 需要保留并迁移到 Python 的核心（与 CFS 场景相关）

| 文件 | 作用 | 迁移处理 |
|---|---|---|
| `dual_model/run_benchmark.py` | SSH/SCP、双 server 生命周期、warmup、采集编排、有效性门禁 | 拆分为 `board.py` / `runner.py` / `validity.py` |
| `dual_model/workload.py` | 流式请求引擎，异步注入 + 同步释放 | 保留核心，删除 hook / 外部策略相关分支 |
| `dual_model/metrics.py` | TTFT / ITL / TPS / fairness / sampler 指标 | 基本照搬 |
| `dual_model/report.py` | 单轮 + suite 报告 | 保留 CFS 相关，删除 rvv/external 分支 |
| `dual_model/remote_sampler.py` | 板端 /proc + 平台采样 | 照搬 |
| `dual_model/prompts.json` | 等长 prompt 配置 | 照搬 |
| `dual_model/run.sh` | 双模型入口包装 | 由新 Python CLI 取代 |
| `multi_model/run.py` | N-model 同时到达入口 | 作为多 llama 场景的基础重写 |

### B. 策略 / sched_ext 专用（当前 CFS-only 不需要，后续新策略再参考）

| 文件/代码 | 原因 |
|---|---|
| `dual_model/scheduler_stats.py` | 全部是 `scx_llm_pin` 日志校验（lease / gang / phase），CFS 下不生效 |
| `run_benchmark.py` 中 `--strategy external`、`--lease-hint-path`、`--external-*-cmd`、`--pin-workers-shared`、`--metrics rvv*` 等路径 | 只服务 sched_ext / RVV trace |
| `benchmark.sh` 的 `run_dual_model_route` 与 `apply_strategy_preset` | 旧策略预设（`rvv_dirty_*`、`linux_affinity`、`scx_global`） |
| `config.sh` 中 `SCX_*` / `RVV_TRACE_*` 变量 | sched_ext / trace 专用 |
| `dual_model/run_ttft_p95_campaign.sh`、`run_four_strategy_campaign.sh`、`dual_model/aggregate_four_strategy_campaign.py`、`dual_model/simultaneous/run_campaign.sh` + `aggregate_campaign.py`、`dual_model/run_phase_aware_c2.sh` | 三/四策略 campaign，CFS-only 阶段无意义 |
| `dual_model/run_ttft_p95_campaign_abi_v4.sh`、`run_abi_v4_regression_sample.sh` | ABI v4 / N-team 专用一次性入口 |

### C. 无关 / 历史遗留 / 可直接丢弃

| 文件/目录 | 原因 |
|---|---|
| `benchmark.sh`（2419 行） | 单 server 主 benchmark，非本次三个场景 |
| `config.sh`、`verify.sh`、`compare.sh`、`run_test.sh`、`quick_test.sh`、`smoke_test.sh`、`test_single.sh`、`linux_affinity_benchmark.sh` | 单 server 时代的包装/诊断脚本 |
| `generate_report.py` | 旧单 server 报告生成器 |
| `phase_gate.py` | 单 server `SYNC_CONCURRENT_START` 相位门禁 |
| `barrier_c4/` | 历史 futex/barrier 实验，无 harness |
| `phase_c1/` | 历史 c=1 / `-t`/`-tb` 扫描结果，无 harness |
| `vector_state_switch/` | x86 AVX / RVV vector 状态切换微基准，与 LLM 调度无关 |
| `dual_model/live_concurrency_demo.py` + `test_live_concurrency_demo.py` | 终端 2+2 可视化 CLI 与回放 |
| `dual_model/report.py` 中 rvv / external 相关函数 | 随策略裁剪 |
| `dual_model/test_*.py` | 现有单测，多数绑定将被删除的模块；迁移后重写 |
| `results/`、`logs/`、`dual_model/results/`、`dual_model/demo_results/`、`multi_model/results/`、各 `__pycache__/` | 历史产物 |

### D. 需要单独判断

| 项 | 说明 |
|---|---|
| `dual_model/symmetric/` | 若“同步注入”指 symmetric，则保留；否则归入 C 类 |
| `dual_model/simultaneous/` | 若“同步注入”指 simultaneous，则保留 `run.sh`，删除三策略 campaign |
| `multi_model/BOARD_VALIDATION.md` | 记录性文档，可保留作参考 |

## 建议的新目录结构（待确认后实施）

```text
benchmark_v2/
├── cli.py                 # 统一入口：--scenario async-dual | sync-dual | multi
├── board.py               # SSH/SCP、进程与健康检查
├── server.py              # llama-server 生命周期与 warmup
├── workload/
│   ├── dual.py            # 双 llama 异步注入 / 同步释放
│   └── multi.py           # N llama 同时到达
├── metrics.py             # 指标计算
├── sampler.py             # 板端采样
├── report.py              # 报告输出
├── config.py              # 配置模型
└── prompts.json
```

## 待确认

1. “双 llama 同步注入”对应 `simultaneous`（全请求同时释放）还是 `symmetric`
   （A/B 成对对齐）？
2. 新目录名是否使用 `benchmark_v2`？
3. 多 llama 场景在 CFS-only 阶段是否仍要求支持不同 `threads` / `threads_batch` /
   `cpu_mask`，还是先退化为最简单的 N 服务同时到达？
