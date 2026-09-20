# 持续 grant 执行记录（2026-09-18）

## 实现

独立实验新增 `--grant-lifetime persistent`；默认仍为 graph。
分配政策维持单 8 核、双 4+4，没有加入 phase-aware、gang 或长时间片。
SOCK_SEQPACKET v3 支持已受理回复、异步 GRANT/REVOKE、授权代次校验以及有界
EPOLLOUT 发送队列；不是已有 phase hint ABI v4 的修改。

稳定 graph 保留 grant、BPF 成员与 affinity，不发送协议消息。OpenMP team 通过
带引用计数的 TLS 身份记录检测成员变化；退出锁保证远程 affinity 恢复期间 TID
不能因线程退出而被复用。小于 grant 容量的实际 team 不改变 CPU 所有权。
资源交接仍严格先撤销映射、恢复 affinity、释放，再分配；错误不回退。

## 已完成验证

- 主机 `make host`：通过（`-Wall -Wextra -Werror`）。
- `test_control` + `test_persistent`：14 项通过，第二轮 47.393 秒。
  证据：`results/persistent-host-gate-20260918-instrumented/`。
  原 graph 单/双进程各 10,000 次协议循环仍通过。
- 稳定真实 OpenMP 10,000 graph：仅一次 REQUEST/GRANT/MEMBERS/UNMEMBERS/RELEASE。
- 实际宽度变化 1,000 次、线程销毁重建 100 次、8→4+4→8、安全交接、待授权取消、
  需求合并、断链隔离、旧代次拒绝、等待 31.2 秒不误超时：通过。
- `build/test_transport`：强制 socket 背压后有序发送、MEMBERS 回复前插入 REVOKE、
  过期通知拒绝均通过。
- Docker `riscv-nteam-toolchain:local --network none` 内 RISC-V llama-server 构建通过。
  离线 UI 资源下载警告不影响 server 构建；没有打开网络或修改构建策略。
- 板端探针：`results/control-20260918T025422Z-23a1686c/summary.json`，通过。
  稳定 1,000 graph：1 grant / 1 MEMBERS；宽度变化 1,000 graph：1 grant / 801 MEMBERS；
  重建 1,000 graph：1 grant / 1,000 MEMBERS。双实例合计 2,000 graph，mask 15/240；
  BPF wrong_cpu=0，scheduler 清理正常，dmesg 无新增错误。
- 真实 llama：`results/llama-20260918T025616Z-d3120c41/summary.json`，通过。
  单实例与双实例均完成确定性请求；授权覆盖单 8 核和双 4+4，只有 3 次成员更新；
  BPF wrong_cpu=0，scheduler 与 dmesg 清理正常。
- benchmark smoke：`results/benchmark-20260918T052728Z-2229f325/campaign.json`，通过。
  16 输出 token 下 graph 为 264 条消息/32 grant，persistent 为 44 条消息/4 grant。

## 性能消融结果

最终结果：`results/benchmark-20260918T080750Z-6beee320/campaign.json`，`complete=true`。
工作负载为 c=2 asynchronous、128 输入/256 输出，graph/persistent 各 3 次，按
`persistent, graph, graph, persistent, persistent, graph` 交错。六轮都完整、确定性
且有效，最终 sched_ext 为 disabled，dmesg 无新增错误。

| 指标 | graph | persistent | 变化 |
|---|---:|---:|---:|
| 聚合 TPS | 2.7795 | 2.7946 | +0.54% |
| 平均 TTFT | 19.783 s | 19.791 s | +0.04% |
| injected TTFT | 26.752 s | 26.749 s | -0.01% |
| 平均 ITL | 585.400 ms | 584.700 ms | -0.12% |
| ITL p95 | 717.003 ms | 706.183 ms | -1.51% |
| incumbent overlap ITL p95 | 717.147 ms | 711.722 ms | -0.76% |
| 协议消息/轮 | 4104 | 44 | -98.93% |
| grant/成员更新/轮 | 512 / 512 | 4 / 4 | grant -99.22% |

这组样本证明了机制目标：稳态通信和 map 更新不再随 token 线性增长，且没有破坏
吞吐、TTFT 或尾延迟。性能差异很小，三次样本不足以把这些小幅变化解释为
确定收益；本轮结论应是“消除逐 token 控制开销且性能不回退”，不是宣称明显提速。

persistent 的 4 次 revoke 平均从通知到 RELEASE 为 136.5 ms，p95 样本均值为
358.0 ms；这是 graph 边界响应时间，不是逐 token IPC 延迟。它符合没有后台控制线程、
只在边界协调的设计约束。

早期结果 `benchmark-20260918T053226Z-90cdd155` 保留为历史证据，不与最终版本混算。
最终版本补齐了 BPF start_boottime 身份验证；对应板端门禁为
`control-20260918T080549Z-b6354447`、真实 llama 门禁为
`llama-20260918T080627Z-9e53c20b`，均通过。最终 campaign 的六轮均有效，
dmesg delta 为空，wrong_cpu/stale_identity 均为 0。

最终性能构建 SHA256（后续 phase-aware 构建另行记录）：

- dt_controller: `56c6feeb4089e647e8e9d9acad5ca72a501bc469971a13c5819a0886f2d3802b`
- llama-server: `ce89049ad5d545c198f402f141617a4f0c0f87af3b619af3bbf88199c57c3351`
- libggml-cpu: `539fac9c43c9915184729337ab6651ca8b5263552e255591bccaa970b14e232a`

正式 campaign 前发现一个更早被中断的 `benchmark-20260918T034632Z-c87d5586`
留下两个 server 和一个 controller。确认本地主控已消失且 PID/路径只属于该实验后，
精确终止这三个进程并确认 sched_ext disabled；该不完整数据未纳入结果。

## 复现性能消融

```sh
python3 experiments/dynamic_threads/run_benchmark.py \
  --cases scx_dynamic,scx_dynamic_persistent --interleave \
  --scenarios asynchronous --concurrencies 2 --loops 3 --output-tokens 256
```

使用同一套 runtime/controller，每次重复交替 case 顺序；每个 block 重启实例并做相同
warmup，避免旧 harness 将某个 case 的所有重复连续运行。输出增加协议消息数、
成员更新次数、授权次数、撤销次数、排队和交接耗时。所有 server 保持 `-t 8 -tb 8`。
