# rvv_sched_cost

阶段一的 CO-RE eBPF 观测工具，用于在不修改内核的情况下统计目标
`llama-server` 线程的调度事件，并基于 RISC-V `pt_regs.status` 的 `SR_VS`
位推断 V 状态保存/恢复候选事件。

## 统计口径

- `sched_switch`：使用 `tp_btf/sched_switch` 获取 `prev` / `next`
  `task_struct`。
- `sched_wakeup`：使用 `tp_btf/sched_wakeup` 记录 wakeup 时间。
- 目标过滤：`--tgid` 过滤进程线程组，`--tid` 可进一步过滤单个线程。
  `--comm` 会同时匹配 `/proc/PID/comm` 和 `/proc/PID/cmdline`，因此通过自带
  `ld-linux-riscv64-lp64d.so.1` 启动的 `llama-server` 也可以自动解析 TGID。
- `v_save_candidates`：目标线程作为 `prev` 切出时，`task_pt_regs(prev)->status`
  中 `SR_VS == SR_VS_DIRTY`。
- `v_restore_candidates`：目标线程作为 `next` 切入时，`task_pt_regs(next)->status`
  中 `SR_VS != SR_VS_OFF`。
- histogram 使用 log2 微秒桶：bucket 0 表示 `<1us`，bucket N 表示
  `[2^(N-1), 2^N-1] us`。

注意：本工具不能直接统计真实 V save/restore 指令路径耗时。它用于验证采集框架、
目标线程过滤、调度频率、off-CPU / wake-to-run 延迟和 V 状态候选事件频率。若需要
真实 V save/restore 耗时，需要第二阶段内核 tracepoint。

## 构建

默认板卡地址是当前实验地址 `192.168.3.67`，账号密码使用 `root/bianbu`：

```sh
cd experiments/rvv_sched_cost
make fetch-btf BOARD_IP=192.168.3.67
make
```

如果目标端 libbpf/elfutils 尚未在 Buildroot staging 中生成，`make` 会调用：

```sh
sudo make -C ../../jupiter-buildroot/buildroot \
  O=$(realpath ../../jupiter-buildroot/output/k1_v2) \
  BR2_EXTERNAL=$(realpath ../../jupiter-buildroot/buildroot-ext) \
  libbpf
```

## 部署与运行

```sh
make deploy BOARD_IP=192.168.3.63
sshpass -p bianbu ssh -o StrictHostKeyChecking=no root@192.168.3.63 \
  '/tmp/rvv_sched_cost --comm llama-server --duration 30 --json /tmp/rvv_sched_cost.json'
```

也可以显式指定 TGID：

```sh
/tmp/rvv_sched_cost --tgid <llama-server-pid> --duration 60 --json /tmp/result.json
```

若只想观察调度开销，不读取 `pt_regs.status`，可加：

```sh
/tmp/rvv_sched_cost --tgid <pid> --duration 30 --no-vs
```

## 输出

默认输出文本 summary，并可通过 `--json` 写 JSON。主要字段：

- `switch_events` / `switches_in` / `switches_out`
- `wakeups`
- `offcpu_events` / `offcpu_avg_ns` / `offcpu_max_ns`
- `wake_to_run_events` / `wake_to_run_avg_ns` / `wake_to_run_max_ns`
- `v_save_candidates`
- `v_restore_candidates`
- `prev_vs_samples` / `next_vs_samples`
- per-CPU summary
- per-thread summary
