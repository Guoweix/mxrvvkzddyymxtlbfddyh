# benchmark v2

`experiments/benchmark` 的 Python 重写。只保留三种测试场景，且当前只支持 Linux
CFS（不加载任何 sched_ext 策略）。

## 场景

| `--scenario` | 说明 | 请求模型 |
|---|---|---|
| `dual-async` | 双 llama 异步注入 | 先释放 incumbent，等每个 incumbent 请求达到 `--injection-token` 后注入第二个 server |
| `dual-sync` | 双 llama 同步释放（宽松版 simultaneous） | A/B 所有请求共享同一绝对释放时刻，只记录释放偏差，不作有效性门禁 |
| `multi` | 多 llama 同时到达 | 1–8 个 server，每个 server 的请求共享同一释放时刻 |

三个场景都使用 `prompts.json`：每个模型按请求数取前 N 条不同 prompt，并补齐到等长
的 128 input token，因此结果可横向比较。`multi` 每个模型的 `requests` 不能超过
manifest 中的 prompt 数量（默认 4）。

该板 128-token prefill 单次要 ~28s（decode ~1 tok/s），默认超时已放宽：
`--request-timeout 3600`、`--milestone-timeout 1800`。跑更长输出时按需再调大，避免
把慢板误判成注入/请求失败。

`dual-sync` 对应旧 `dual_model` 的 `--scenario simultaneous`。旧 `symmetric` 严格对齐
门禁未保留。

## 策略

| `--strategy` | 说明 |
|---|---|
| `cfs`（默认） | 纯 Linux CFS，不加载任何调度器 |
| `dynamic` | `experiments/dynamic_threads`：patched llama-server + `dt_controller` + sched_ext BPF，按 graph 边界动态授权 OpenMP team 宽度 |
| `scx-llm-pin` | `experiments/scx_llm_pin`：`rvv_dirty_local` + 自动 phase-aware。`--user-hint on` 用 ABI v4 runtime 提供 phase hint，`off` 用普通 runtime 走回退路径 |

`dynamic` 运行方式（对应 `experiments/dynamic_threads/run_multi_benchmark.py`）：

- 部署本地 runtime（默认 `experiments/dynamic_threads/llama.cpp/build-dynamic-riscv/bin`）
  和 `dt_controller`（默认 `experiments/dynamic_threads/build/dt_controller`）到板端。
- 启动控制器：
  `dt_controller --socket <dir>/dynamic.sock --mode dynamic --scx --grant-lifetime persistent --allocation-policy prefill-first --decode-wait-ms 0 --decode-floor-on-prefill`。
- 每个 llama-server 使用 `GGML_DYNAMIC_THREADS_SOCKET=<socket>`、`OMP_DYNAMIC=FALSE`
  以及 `GGML_DYNAMIC_THREADS_RETAIN_SHRINK` 启动。
- 采集 `controller_events.csv` 与 `controller.log`，校验 BPF 放置、`wrong_cpu=0`、
  CPU 所有权无重叠、所有进程都计算过，并把 grants/shrinks/grant-wait/team-publish
  等写入报告。

```bash
python3 benchmark.py --scenario multi --config example.json --strategy dynamic \
  --output-tokens 16
```

默认配置等价于 `dynamic_threads` 的 `scx_dynamic_persistent_phase_reserve1`（缩核保留）；
`--dynamic-full-release` 关闭保留，`--dynamic-decode-wait-ms` / `--dynamic-slice-ms` /
`--dynamic-allocation-policy` / `--dynamic-grant-lifetime` / `--dynamic-runtime-bin` /
`--dynamic-controller` 可覆盖细节。要求板端 sched_ext 空闲，且 runtime/controller 已构建。

所有场景都接受 `--strategy cfs|dynamic|scx-llm-pin`。新增策略时只需扩展
`--strategy` 取值并在 `runner.py` 接入，不必改动各场景调用点。

### scx-llm-pin

`scx-llm-pin` 部署本地 `experiments/scx_llm_pin/scx_llm_pin`，并按每个 server PID
生成 `--team PID:decode:prefill[:mask]`。是否带用户态提示由 `--user-hint on|off` 控制：

- `on`：server 用 `--scx-hint-runtime-bin`（默认 `/mnt/llm/llama/bin-nteam-v4`）
  并设置 `GGML_SCX_LEASE_HINT_PATH=<control-dir>/workers`，调度器读到有效 hint 后
  进入 phase-aware。
- `off`：用普通 runtime，调度器走 `rvv_dirty_local` 回退。

相关参数：`--scx-vector-slice-ms`(100)、`--scx-vector-local-window-ms`(500)、
`--scx-dirty-threshold`(2)、`--scx-phase-hint-timeout-ms`(300000)、
`--scx-prefill-starvation-ms`(1000)、`--scx-phase-wakeup-preempt on|off`、
`--scx-control-dir`。`window-ms > 0` 时始终启用 local 窗口（每个 vector DSQ 消费后
的 500ms 内，该 team 的 vector 任务改走 local DSQ，兼顾普通任务调度）。

## 用法

```bash
export REMOTE_IP=<board-ip>
export REMOTE_USER=<user>
export REMOTE_PASS=<password>

# 双 llama 异步注入，c=4
python3 benchmark.py --scenario dual-async --concurrency 4 --loops 3 \
  --llm-path /mnt/llm/llama --runtime-bin-dir /mnt/llm/llama/bin \
  --model-a /mnt/llm/llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf \
  --model-b /mnt/llm/llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf

# 双 llama 同步释放，c=4
python3 benchmark.py --scenario dual-sync --concurrency 4 --loops 3 \
  --llm-path /mnt/llm/llama --runtime-bin-dir /mnt/llm/llama/bin

# 多 llama，先校验配置再运行
python3 benchmark.py --scenario multi --config example.json --check-config
python3 benchmark.py --scenario multi --config example.json --output-tokens 16
```

## 目录结构

```text
benchmark_v2/
├── benchmark.py            # 统一 CLI
├── prompts.json            # 4 个等长（128 token）prompt
├── example.json            # multi 场景模型配置示例
├── ANALYSIS.md             # 第一步的旧代码分类
├── bench/
│   ├── board.py            # SSH/SCP、进程控制、健康检查
│   ├── server.py           # llama-server 生命周期与 warmup
│   ├── workload.py         # 流式请求引擎（dual / multi，可独立执行）
│   ├── remote_sampler.py   # 上传到板端的 /proc 与平台采样器
│   ├── metrics.py          # 指标计算
│   ├── report.py           # run_metrics.json / summary / report.md
│   ├── dynamic_threads.py  # dynamic 策略适配与 controller 事件校验
│   └── runner.py           # suite 编排与有效性门禁
└── tests/
```

## 输出

```text
results/<timestamp>_<scenario>_suite/
├── config.json
├── prompts.json
├── system_info.txt
├── summary.json
├── summary.csv
├── report.md
└── run<NN>_<direction>_attempt<M>/
    ├── run.json
    ├── config.txt
    ├── workload.json
    ├── events.jsonl
    ├── sampler.jsonl
    ├── server_*.log
    ├── dmesg_*.log
    └── run_metrics.json
```

## RVV trace（可选）

默认不加载 `rvv_sched_cost`。需要时加 `--rvv-trace`，此时默认使用开发板上已有的
二进制，路径由 config.sh 的 `RVV_REMOTE_DIR` 决定（二进制
`$RVV_REMOTE_DIR/rvv_sched_cost`，库目录 `$RVV_REMOTE_DIR/lib`）：

```bash
# 板端已部署 rvv_sched_cost
python3 benchmark.py --scenario dual-sync --concurrency 2 --output-tokens 16 --rvv-trace

# 板端没有：显式上传本地二进制（experiments/rvv_sched_cost/rvv_sched_cost + libelf）
python3 benchmark.py --scenario dual-sync --concurrency 2 --output-tokens 16 \
  --rvv-trace --rvv-upload

# 额外采集 futex blocked time
python3 benchmark.py --scenario dual-sync --concurrency 2 --output-tokens 16 \
  --rvv-trace --rvv-futex-time
```

相关参数：`--rvv-remote-dir` 覆盖板端目录，`--rvv-local-bin` / `--rvv-local-libelf`
覆盖上传源；开启 trace 后，每个 server 都会单独运行一个 `rvv_sched_cost`，结果写入
`rvv_<model>.json`，报告汇总 `rvv_save_candidates` / `rvv_restore_candidates`（及
futex 统计）。trace 缺失或校验失败会使该轮无效。

## 测试

```bash
python3 -m unittest discover -s tests -v
```

## 与旧版差异

- 移除单 server `benchmark.sh`、`linux_affinity`、`barrier_c4`、`phase_c1`、
  `vector_state_switch` 等历史入口。
- 移除所有 sched_ext 策略（`observe`、`phase-aware` 等）、scheduler 日志校验与
  三/四策略 campaign。
- RVV trace 改为显式开关 `--rvv-trace`，默认使用板端已有的 `rvv_sched_cost`。
- 移除终端 live demo 与离线回放。
- `multi` 场景不再依赖 `rvv_team_exec` / `rvv_lease_ctl`，通过 Python 在 exec 前设置
  CPU 亲和性即可。
