# demo：可验证交付物（预编译二进制）

无需源码/编译，直接用本目录内的**预编译二进制 + test 脚本**运行。
按平台分为 `x86/` 与 `riscv/`。

```
demo/
├── x86/
│   ├── bin/        dt_controller、dt_probe、dt_persistent_probe、dt_phase_probe（x86-64）
│   ├── runtime/    patched llama.cpp（llama-server + 共享库，x86-64）
│   ├── tests/      本机 runner（run_dual_model_x86 / run_multi_x86）与单测
│   ├── config.sh   指向 demo 内 bin/runtime 与模型路径
│   └── run.sh      功能验证（启动 controller + persistent 探针）
└── riscv/
    ├── bin/        dt_controller、dt_probe、dt_persistent_probe、dt_phase_probe、rvv_sched_cost（RISC-V）
    ├── runtime/    patched llama.cpp（llama-server + 共享库，RISC-V）
    ├── tests/      部署/门禁 runner（run_control_gate 等）
    ├── config.sh   板端连接与 runtime 路径
    └── run.sh      功能验证（部署到板端 + controller + 探针）
```

## 前置条件

- **x86**：Linux 6.18 + `sched_ext` + BTF（`/sys/kernel/btf/vmlinux`）、root、`libbpf`；
  运行完整推理负载还需自备模型 GGUF（放到 `x86/model.gguf` 或设 `DT_X86_MODEL`）。
- **RISC-V**：K1 板卡 + 含 `sched_ext` 的内核；板端已挂载 bpffs
  （`mount -t bpf bpf /sys/fs/bpf`，`run.sh` 会自动尝试）；板端模型
  （默认 `/mnt/llm/llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf`）；host 有 `sshpass`。

## 运行

### x86（本机）

```bash
sudo -n demo/x86/run.sh                      # 功能验证：attach + 探针 + wrong_cpu=0
# 完整推理负载（需模型）：
sudo -n env DT_X86_MODEL=/path/to/model.gguf \
  python3 demo/x86/tests/run_dual_model_x86.py \
    --mode baseline --mode dynamic-phase --concurrency 2 --loops 1 --output-tokens 32
# 4 进程超售对照：
sudo -n env DT_X86_MODEL=/path/to/model.gguf \
  python3 demo/x86/tests/run_multi_x86.py --mode baseline --mode dynamic --mode pair-gang
```

### RISC-V（板端）

```bash
sudo -n demo/riscv/run.sh                    # 部署 + 功能验证
# 控制门禁（预编译二进制，无需编译）：
cd demo/riscv/tests && sudo -n python3 run_control_gate.py \
    --grant-lifetime persistent --allocation-policy prefill-first \
    --decode-wait-ms 0 --decode-floor-on-prefill --iterations 200
```

## 统一 benchmark（仅用 demo 二进制）

`tests/benchmark` 的默认路径已指向本 demo，**无需源码或 `experiments/`**：

```bash
# x86 本机（需 root 加载 sched_ext）
sudo python3 tests/benchmark/benchmark.py --local --scenario dual-sync \
    --strategy x86-dynamic --concurrency 2 --output-tokens 16

# RISC-V 板端（默认从 tests/config.sh 读板卡信息）
python3 tests/benchmark/benchmark.py --scenario dual-sync \
    --strategy dynamic --concurrency 2 --output-tokens 16
```

`dynamic` 使用 `demo/riscv/{bin,runtime}`，`x86-dynamic` 使用
`demo/x86/{bin,runtime}`；可用 `--dynamic-controller/--dynamic-runtime-bin` 与
`--x86-dynamic-controller/--x86-runtime-bin` 覆盖。demo 内的二进制由提交仓库
`src/` 构建，二者逐字节一致。

## 说明

- 二进制为对应架构的最终构建产物；`tests/` 内的 runner 已指向本 demo 的
  `bin/`、`runtime/` 与 `config.sh`，不依赖源码目录。
- 分配语义相关的 RISC-V 单测不适用于 x86 SMT 分配，x86 的验证为架构无关单测 +
  本机 runner（见 `x86/tests`）。
- 模型 GGUF 体积较大，未随 demo 提供，请自行放置或用环境变量指定。
