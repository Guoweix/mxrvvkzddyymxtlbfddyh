# INFERSCHED benchmark

`tests/benchmark` 是 INFERSCHED 的统一 Python benchmark：默认通过 SSH 驱动板端
llama-server，也可用 `--local` 在本机 x86 直跑。覆盖三种请求场景，主策略为
`scx-llm-pin`，并提供 CFS、dynamic、cfs-affinity 与两个 x86 对照策略。

## 场景

| `--scenario` | 说明 | 请求模型 |
|---|---|---|
| `dual-async` | 双 llama 异步注入 | 先释放 incumbent，等每个 incumbent 请求达到 `--injection-token` 后注入第二个 server |
| `dual-sync` | 双 llama 同步释放（宽松版 simultaneous） | A/B 所有请求共享同一绝对释放时刻，只记录释放偏差，不作有效性门禁 |
| `multi` | 多 llama 同时到达 | 1–8 个 server（JSON 配置），每个 server 的请求共享同一释放时刻 |

三个场景都使用 `prompts.json`：每个模型按请求数取前 N 条不同 prompt，并补齐到等长
的 128 input token，因此结果可横向比较。`multi` 每个模型的 `requests` 不能超过
manifest 中的 prompt 数量（默认 4）。

该板 128-token prefill 单次要 ~28s（decode ~1 tok/s），默认超时已放宽：
`--request-timeout 3600`、`--milestone-timeout 1800`。跑更长输出时按需再调大，避免
把慢板误判成注入/请求失败。

`dual-sync` 对应旧 `dual_model` 的 `--scenario simultaneous`；旧 `symmetric` 严格对齐
门禁未保留。

## 策略

| `--strategy` | 说明 |
|---|---|
| `cfs`（默认） | 纯 Linux CFS，不加载任何调度器 |
| `scx-llm-pin` | 主策略：`src/scx_llm_pin`，`rvv_dirty_local` + 自动 phase-aware |
| `cfs-affinity` | CFS + worker 每核绑 1 个 + `base_slice_ns=100ms` + `GOMP_SPINCOUNT=1000` |
| `dynamic` | `src/dynamic_threads`：graph 边界动态授权 OpenMP team 宽度（见顶层 README 附录） |
| `x86-dynamic` | `src/x86_dynamic_threads` 的本地 x86 版本（需 `--local` + root） |
| `x86-scx-llm-pin` | `src/x86_scx_llm_pin` 的 x86/SMT 版本，固定 spread 放置（需 `--local` + root） |

板卡连接与路径默认从 `tests/config.sh` 读取（`REMOTE_*`、`LLM_PATH`、
`TOOLCHAIN_ROOT`、`DT_LLAMA_BIN` 等），环境变量优先。新增策略时扩展 `--strategy`
取值并在 `runner.py` 接入，不必改动各场景调用点。

### scx-llm-pin（主策略）

部署本地 `src/scx_llm_pin/scx_llm_pin`，按每个 server PID 生成
`--team PID:decode:prefill[:mask]`。是否带用户态提示由 `--user-hint on|off` 控制：

- `on`：server 用 `--scx-hint-runtime-bin`（默认 `/mnt/llm/llama/bin-nteam-v4`）
  并设置 `GGML_SCX_LEASE_HINT_PATH=<control-dir>/workers`，调度器读到有效 hint 后
  进入 phase-aware。
- `off`：用普通 runtime，调度器走 `rvv_dirty_local` 回退。

相关参数：`--scx-vector-slice-ms`(100)、`--scx-vector-local-window-ms`(500)、
`--scx-dirty-threshold`(2)、`--scx-phase-hint-timeout-ms`(300000)、
`--scx-prefill-starvation-ms`(1000)、`--scx-phase-wakeup-preempt on|off`、
`--scx-control-dir`。`window-ms > 0` 时始终启用 local 窗口（每个 vector DSQ 消费后
的 500ms 内，该 team 的 vector 任务改走 local DSQ，兼顾普通任务调度）。

### dynamic

对应 `src/dynamic_threads`（板端）/ `src/x86_dynamic_threads`（x86）。控制器在
graph 边界动态授权，服务器通过 Unix socket 协商。运行参数：
`--dynamic-runtime-bin`、`--dynamic-controller`、`--dynamic-full-release`、
`--dynamic-decode-wait-ms`、`--dynamic-slice-ms`、`--dynamic-allocation-policy`、
`--dynamic-grant-lifetime`。要求 sched_ext 空闲且 runtime/controller 已构建。

### x86（--local）

`x86-dynamic` / `x86-scx-llm-pin` 在本机运行（`--local`，不经 SSH；加载
sched_ext 需要 root）。每个模型默认 `threads=threads_batch=4`；`x86-scx-llm-pin`
固定 spread：team0→CPU0-3、team1→CPU4-7。相关参数：`--x86-runtime-bin`、
`--x86-model`、`--x86-dynamic-controller`、`--x86-scx-llm-bin`、`--x86-threads`、
`--x86-control-dir`。

## 用法

```bash
cd tests/benchmark

# 板端（默认从 tests/config.sh 读板卡信息）
python3 benchmark.py --scenario dual-sync --strategy cfs --concurrency 2 --loops 1 --output-tokens 16
python3 benchmark.py --scenario dual-sync --strategy scx-llm-pin --user-hint on \
    --concurrency 2 --loops 1 --output-tokens 16

# 多 llama，先校验配置再运行
python3 benchmark.py --scenario multi --config example.json --check-config
python3 benchmark.py --scenario multi --config example.json --strategy scx-llm-pin --user-hint on

# 本机 x86（需 root 加载 sched_ext）
sudo python3 benchmark.py --local --scenario multi --config example.json \
    --strategy x86-scx-llm-pin --output-tokens 16
```

## 目录结构

```text
tests/benchmark/
├── benchmark.py            # 统一 CLI
├── prompts.json            # 4 个等长（128 token）prompt
├── example.json            # multi 场景模型配置示例
├── bench/
│   ├── board.py            # SSH/SCP 或本地(Remote/Local)传输、进程控制、健康检查
│   ├── server.py           # llama-server 生命周期与 warmup
│   ├── workload.py         # 流式请求引擎（dual / multi，可独立执行）
│   ├── remote_sampler.py   # 板端/本机的 /proc 与平台采样器
│   ├── metrics.py          # 指标计算
│   ├── report.py           # run_metrics.json / summary / report.md
│   ├── scx_llm_pin.py      # 主策略适配 + SCX stats 校验
│   ├── x86_scx_llm_pin.py  # x86 主策略适配
│   ├── dynamic_threads.py  # dynamic 策略适配 + controller 事件校验
│   ├── x86_dynamic.py      # x86 dynamic 适配
│   ├── cfs_affinity.py     # cfs-affinity 策略
│   └── runner.py           # suite 编排与有效性门禁
└── tests/
```

## 输出

```text
results/<timestamp>_<scenario>_<strategy>_suite/
├── config.json
├── prompts.json
├── system_info.txt
├── summary.json
├── summary.csv
├── report.md
└── run<NN>_<direction>_attempt<M>/
    ├── run.json
    ├── workload.json
    ├── events.jsonl
    ├── sampler.jsonl
    ├── server_*.log
    ├── dmesg_*.log
    └── run_metrics.json
```

## RVV trace（可选）

默认不加载 `rvv_sched_cost`。需要时加 `--rvv-trace`，此时默认使用板端已有的
二进制，路径由 `tests/config.sh` 的 `RVV_REMOTE_DIR` 决定：

```bash
python3 benchmark.py --scenario dual-sync --concurrency 2 --output-tokens 16 --rvv-trace
python3 benchmark.py --scenario dual-sync --concurrency 2 --output-tokens 16 --rvv-trace --rvv-upload
python3 benchmark.py --scenario dual-sync --concurrency 2 --output-tokens 16 --rvv-trace --rvv-futex-time
```

`--rvv-remote-dir` 覆盖板端目录，`--rvv-local-bin` / `--rvv-local-libelf` 覆盖上传源；
每个 server 单独运行一个 `rvv_sched_cost`，结果写入 `rvv_<model>.json`。trace 缺失或
校验失败会使该轮无效。

## 测试

```bash
python3 -m unittest discover -s tests -v
```

## 与旧版差异

- 移除单 server `benchmark.sh`、`linux_affinity`、`barrier_c4`、`phase_c1`、
  `vector_state_switch` 等历史入口。
- 策略收敛为 CFS / scx-llm-pin / dynamic / cfs-affinity 及两个 x86 策略；移除旧
  `rvv_dirty_*`、三/四策略 campaign 与终端 live demo。
- RVV trace 改为显式开关 `--rvv-trace`，默认使用板端已有的 `rvv_sched_cost`。
- 支持 `--local` 在 x86 本机直跑。
