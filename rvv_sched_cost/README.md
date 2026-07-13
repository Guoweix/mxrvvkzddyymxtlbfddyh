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
- `switches_out_state`：目标线程切出时的状态分布，顺序为
  `running, preempt, sleeping, uninterruptible, other`。`preempt` 表示调度器抢占；
  `sleeping` / `uninterruptible` 表示线程切出前主动进入等待状态。
- `v_save_out_state`：只统计 `SR_VS == SR_VS_DIRTY` 切出候选事件的同一状态分布，
  用于判断 RVV dirty 线程到底是被抢占，还是自己阻塞/睡眠后让出 CPU。
- `--no-switch-out`：关闭 `prev`/switch-out 侧统计。开启后不再统计
  `switches_out`、off-CPU、`v_save_candidates`、`prev_vs_samples` 和
  `*_out_state`，只保留 switch-in、wake-to-run 和 restore 侧统计，用于降低
  trace 对长测的性能干扰。
- `--no-switch-out-state` / `--no-switch-reason`：只关闭切出理由分布
  `switches_out_state` / `v_save_out_state`，但仍保留 `switches_out`、
  `v_save_candidates` 和 `v_restore_candidates`。适合只需要 RVV 上下文切换次数的
  benchmark 长测。
- `--rvv-only`：只挂载 `sched_switch`，仅累计目标进程的全局
  `v_save_candidates` / `v_restore_candidates`。不维护 per-thread/per-CPU、时间戳、
  histogram、waker 或 futex 数据，适合只比较 RVV 上下文切换次数的低扰动长测。
- `--futex`：额外挂载 `raw_syscalls/sys_enter`，只统计目标 TGID/TID 的 futex
  系统调用，并按 wait / wake / requeue / wake_op / PI / waitv 分类，同时聚合 futex
  地址。默认不启用，避免所有 syscall 都经过额外 tracepoint。
- `--futex-detail`：在 `--futex` 基础上额外记录 futex 地址 + TID 维度。该模式每个
  futex syscall 多一次 hash map 更新，只适合短时诊断。
- `--futex-stack`：在 `--futex` 基础上记录 futex 地址 + wait/wake 类型 + 用户态
  stack id，并在 JSON/text 中输出原始 IP。该模式依赖用户态栈回溯能力，开销和扰动
  明显高于普通 `--futex`，建议只在短窗口内用于定位 top futex 的调用来源。
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

若需要降低长测观测开销，可关闭 switch-out 侧统计：

```sh
/tmp/rvv_sched_cost --tgid <pid> --duration 300 --no-switch-out
```

若仍需要 RVV save/restore 候选次数，但不需要切出理由，可关闭 out-state：

```sh
/tmp/rvv_sched_cost --tgid <pid> --duration 300 --no-switch-out-state
```

若需要确认 wakeup storm 是否对应用户态 futex 同步放大，可开启 futex 计数：

```sh
/tmp/rvv_sched_cost --tgid <pid> --duration 300 --futex --json /tmp/result.json
```

若需要进一步定位 futex 来源，可短时开启用户态调用栈采样，并同时保存目标进程
`/proc/<pid>/maps`：

```sh
cat /proc/<pid>/maps > /tmp/llama_server_maps.txt
/tmp/rvv_sched_cost --tgid <pid> --duration 30 --futex-stack --json /tmp/result.json
```

## 输出

默认输出文本 summary，并可通过 `--json` 写 JSON。主要字段：

- `switch_events` / `switches_in` / `switches_out`
- `wakeups`
- `offcpu_events` / `offcpu_avg_ns` / `offcpu_max_ns`
- `wake_to_run_events` / `wake_to_run_avg_ns` / `wake_to_run_max_ns`
- `switches_out_state`
- `v_save_candidates`
- `v_restore_candidates`
- `v_save_out_state`
- `futex`
- `futex_addrs` / `futex_tid_addrs` / `futex_stacks`
- `prev_vs_samples` / `next_vs_samples`
- per-CPU summary
- per-thread summary
