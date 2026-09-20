# Phase-aware 实施记录（2026-09-18）

## 已完成

1. 最终 persistent 报告已更新到 `benchmark-20260918T080750Z-6beee320`，六轮均有效；
   修改前的三个 RISC-V 二进制哈希与 campaign 完全一致。同步了 BPF member 的
   start_boottime 头文件，保留早期实验，不重复性能复跑。
2. 分配器已提取为实例集合输入/目标向量输出，支持容量上限、稳定余数、CPU 保留和
   隔离核心排除。运行时、连接数与 BPF map 仍限两个实例。
3. 实现 wire v4（112 bytes）、HELLO 策略协商、首次 ACQUIRE 阶段与 PHASE 更新，
   `equal` 默认 / `prefill-first` 显式启用。复用现有阶段提示，普通及缓存 graph
   计算路径都传入 runtime；稳定阶段不增加 RPC。CPU 交接仍由原事件驱动控制器执行。
4. 增加阶段及 target 日志、真实 OpenMP runtime 探针、负向测试、板端门禁参数及
   `scx_dynamic_persistent_phase` benchmark case。后两类脚本尚未执行。

没有修改原始 llama.cpp、scx_llm 或最终提交仓库，也没有新增备用通信。

## 已通过的验证

- N-ready 独立回归：14 项通过，45.966 秒；证据 `results/nready-host-gate-20260918/`。
- 分配器：1/2/3/8 模拟实例、容量限制、优先级、CPU 保留、无可用核心通过。
- 传输层：有界背压、MEMBERS 中收到 REVOKE、旧代次拒绝，以及 PHASE 回复前交错
  GRANT/REVOKE、连续 10,000 次同阶段调用零 PHASE RPC，通过。
- wire v4 全套主机门禁：failfast 停止时已运行 21 项，20 通过、1 失败，79.681 秒。
  原有 14 项全部通过；新增通过的 6 项是：
  - 真实 runtime 10,000 graph：仅 1 GRANT / 1 MEMBERS / 1 UNMEMBERS / 1 RELEASE，
    2 次阶段记录（首次 prefill + 转 decode），共 18 条协议消息，affinity 恢复。
  - 真实双 OpenMP runtime：decode 暂停、prefill 全核、decode 恢复和 affinity 恢复。
  - 双 prefill 4+4 及阶段变化后的安全交接。
  - 非法 MIXED 信息拒绝。
  - prefill 持有超过 31.2 秒，decode 等待不超时，随后恢复。
  - MIXED 优先及 idle 释放。

证据目录：`results/phase-host-gate-20260918/`。测试结束无残留主机控制器/探针进程。
探针的 mask 输出和板端/benchmark 脚本是在主机门禁运行期间补充的，尚未重新构建验证。

## 已废弃的旧版本测试

早期曾加入 `test_phase.PhaseGate.test_old_wire_rejected`：在 v4 HELLO 后故意发送
v3 ACTIVITY。它只是在测试拒绝旧 wire，与旧 phase dirty lease 完全无关。

测试先用当前 v4 完成 HELLO，随后故意发送 version=3 的 ACTIVITY。
控制器确实执行拒绝，日志为：

```text
REJECT slot=0 op=2 request=2 epoch=0
STOP grants=0 status=OK
```

控制器以原请求为基础返回 EPROTO，所以拒绝回复仍带 version=3。
测试 actor 的 receive() 要求所有回复 version=4，在返回 status 前就抛出
`RuntimeError('bad response identity')`；上层因此没能执行预期的非零 status 断言。
这是负向测试读取方式与拒绝回复语义不匹配，现有证据不表明旧版本获得了授权，
也不表明 phase 策略或通信通道失效。

该测试及测试 actor 的可选版本参数现已删除。当前实验不保留 v3 接口或兼容路径，
只测试 v4 正常功能。上面的历史失败保留用于解释旧证据，不作为当前门禁。

当时的 llama-dynamic.patch 是未验收源码快照，不能与上一轮 RISC-V 二进制混用；
下述 v4 重跑已重新导出、构建并验证当前版本。

此次实际测试的主机构建 SHA256：

- dt_controller_host: `bb5724a3cbb565d27521de33e5de715aa5fbeea7cbbf4bdc44b01823e41ac5a7`
- dt_phase_probe_host: `ac98d917b7aeabd67cbb56d5bc1a1943e677682cb1d770c8bbc8ddb1edf307f5`
- test_transport: `a2c39d3b23c3e0f30fd81e2899aedc98bf51274d4341d19141864d5442d0449b`

## v4 重跑与板端结果

删除旧 wire 测试后，当前 v4 主机套件 23 项全部通过（80.696 秒）；allocation 与
transport 专项同时通过。Docker `riscv-nteam-toolchain:local --network none` 完成
llama-server 构建，UI 资源下载失败仍只是离线警告。

- 板端控制门禁：`control-20260918T114755Z-0157255b`，通过。稳定 1000 graph 为
  1 GRANT / 1 MEMBERS / 2 PHASE；双实例共 2000 graph 覆盖 8→4+4→8，
  wrong_cpu=0，dmesg 与 sched_ext 清理正常。
- 真实 llama 门禁：`llama-20260918T114811Z-6518d704`，通过。单/双实例均输出完整，
  双实例输出一致；8 次 grant 覆盖全 8 核及 mask 15/240，6 次 PHASE，
  wrong_cpu=0，dmesg 与 sched_ext 清理正常。

## 完整性能实验

首次完整实验启动后在旧 dirty-lease case 的收尾验证处停止：

- 结果目录：`results/benchmark-20260918T115025Z-c2646ff8/`
- 该进程及执行会话均已退出
- 策略仅 `cfs_baseline`、`scx_phase_dirty_lease`、
  `scx_dynamic_persistent_phase`
- 场景仅 asynchronous，并发 2/4/8，各 3 轮，256 output tokens，token 4 注入
- 三组共用本轮 patched runtime；dirty lease 使用 ABI v4 N-team scheduler，当前
  dynamic 使用 persistent grant + prefill-first。

其中旧 dirty-lease 的 c=2/4/8 三个 loop-0 workload 均完整有效，调度器运行时统计
也记录 rvv_team0=8、rvv_team1=8；但脚本先停止 server 再停止 scheduler，使最终一行
统计变成 0/0，验证器因而误判 `workers_8_plus_8=false`。清理顺序已修为该 case 先停止
scheduler、保留最终 8+8 统计，再停止 server。失败 campaign 不与正式结果混算。

修复后使用 server 尚存活时的最终调度统计重新执行验证器，phase-lease 全部检查通过。
板端确认 sched_ext disabled、端口无残留监听后，完整矩阵已重新后台启动：

- 结果目录：`results/benchmark-20260918T121901Z-dcaae244/`
- 本地进程：420486；执行会话：39110

早先一次 nohup 尝试被执行环境回收，只创建了空目录
`benchmark-20260918T114948Z-57351b05` 和空日志，不是有效实验，不能纳入结果。
