# 动态线程门禁执行记录

## 当前结论

2026-09-17，事件驱动控制器、板端 OpenMP 探针和真实双 llama-server 功能门禁均
通过。通信路径是 Unix `SOCK_SEQPACKET`；控制器使用 epoll、`SO_PEERCRED` 和
pidfd，没有 loader 轮询或备用 request-map 控制线程。

正式性能矩阵尚未执行。已完成的一轮 c=2/16-token smoke 只证明 harness 可运行并
给出初步信号，不构成策略收益结论。

## 通过的门禁

### 主机协议压力

`python3 test_control.py` 的五项测试全部通过：

- 单进程连续 10,000 次 acquire/release；
- 双进程各连续 10,000 次；
- 单 8 核、加入后 4+4、退出后恢复 8 核；
- 断链 grant 隔离到原进程退出；
- 错误 epoch 拒绝。

### 板端实际 team 与放置

最终 run ID：`control-20260917T132400Z-c33a1f56`。

- manual 1,000 graph 的实际 OpenMP team 循环为 `8→4→2→8`；
- 两个 probe 各 1,000 graph，稳定 mask 为 `0x0f` 和 `0xf0`；
- BPF `placed=13949`、`running=1445927`、`wrong_cpu=0`；
- dmesg 无新增异常，sched_ext 退出后 `enabled: 0`。

证据位于忽略版本管理的
`results/control-20260917T132400Z-c33a1f56/`。

### 真实 llama-server

最终 run ID：`llama-20260917T132604Z-767ba362`。两个 server 均以
`-t 8 -tb 8` 启动，runtime 和共享库来自同一次 Docker/GCC 14 构建。

- 单实例 16-token 请求完整返回，16 个 graph 获得 8 worker/`0xff`；
- 双实例同时请求完整返回，32 个 graph 获得 4 worker，mask 为 `0x0f/0xf0`；
- 两侧确定性输出 SHA-256 相同；
- BPF `placed=1256`、`running=5337`、`wrong_cpu=0`；
- 控制器 `status=OK`，sched_ext 正常卸载，dmesg delta 为空。

证据位于 `results/llama-20260917T132604Z-767ba362/`。

## 实施中发现并修正的问题

1. 第一版 BPF 从 enqueue 直接 dispatch 到 `SCX_DSQ_LOCAL_ON`，运行内核明确拒绝：
   `SCX_DSQ_LOCAL_ON can't be used for direct-dispatch`。改为每 CPU custom DSQ，
   由对应 CPU 的 dispatch callback consume。失败 run 保存在
   `results/control-20260917T125602Z-c2c583e3/`。
2. 第一轮真实 llama 请求能正确返回，grant 也为 8/4+4，但卸载时
   `wrong_cpu=16`。原因是成员 map 先于 affinity 发布，计算后又先恢复 affinity、
   后撤销 map，BPF 在两个边界窗口仍把 worker 视为受控成员。协议增加
   `UNMEMBERS`，生命周期改为“先 pin 后发布、先撤销后恢复”。修复后真实门禁为
   `wrong_cpu=0`。

这些属于当前实现的可定位错误；修复过程中没有改用其他通信机制。

## benchmark smoke

run ID：`benchmark-20260917T133248Z-2a826c30`。配置是 simultaneous、总并发 2、
每请求 128 input/16 output、单次重复，仅比较 CFS baseline 与 scx dynamic。

| 组别 | aggregate TPS | mean TTFT | mean ITL | Jain fairness |
|---|---:|---:|---:|---:|
| CFS baseline | 0.680 | 27.16 s | 1325.6 ms | 1.0000 |
| scx dynamic | 0.935 | 25.08 s | 598.1 ms | 1.0000 |

动态组的 32 个正式 graph 全部为 4+4，grant wait mean/p95 为
86.5/103.5 µs，team publish mean/p95 为 1.13/1.30 ms。由于只有一次短输出运行，
这些差异只能用于确认指标方向和采集有效，不能报告为最终提升。

## 旧 mmap + sched_yield 原型

旧 run `20260917T122442Z-2585e830` 在首个 request 超时：2 秒内执行
1,747,011 次 `sched_yield`，成功 grant 为 0。该次失败后按当时约定停止，没有增加
loader 轮询。旧源码和证据仍保留，当前 SOCK_SEQPACKET 方案不依赖它。

## 范围核对

新增和修改内容都在 `experiments/dynamic_threads/` 及其忽略的独立 llama worktree。
`experiments/scx_llm_pin/`、`experiments/llama_cpp_patches/` 和
`mxrvvkzddyymxtlbfddyh/` 未修改；`kbuild/llama.cpp` 保留其原有本地修改。
