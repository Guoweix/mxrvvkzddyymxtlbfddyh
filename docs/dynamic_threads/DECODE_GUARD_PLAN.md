# Decode 等待保护与分核（2026-09-19）

## 范围

在现有 persistent + prefill-first 上增加等待超时后的黏性 1 核保底。
只改 `experiments/dynamic_threads`，不改原 llama.cpp、scx_llm_pin 或最终提交仓库。
不增加客户端后台线程或逐 token IPC，不更改 wire v4、BPF 或实际 worker 接口。
本轮真实运行仍为两个 llama-server、8 核、`-t 8 -tb 8`；分配器覆盖最多 8 个需求，
不将其等同于已经支持 8 个真实进程。

## 分配规则

- 无 prefill：所有 decode 按容量上限均分；有保护的 decode 不获得额外份额。
- 有 prefill：先给每个已保护的 decode 1 核，再将剩余核均分给 prefill。
  MIXED 按 prefill 处理，未受保护的 decode 可暂时没有 grant。
- 8 核例子：P1+D1 为 7/1，P1+D3 为 5/1/1/1，P2+D2 为 3/3/1/1，
  P1+D7 为每个 1 核。这里的 D 均已触发保护；未触发时仍是严格 prefill 优先。
- 尽量保留原 CPU；余数按稳定实例顺序分配。不能假定 mask 永远是连续的 0x0f/0xf0。
- 保护保持到实例 IDLE；拿到 grant、切换为 MIXED/prefill 都不清除保护。
  单个请求完成但同一实例仍忙，不算 IDLE。

## 等待与超时

`--decode-wait-ms` 默认 2000，0 表示关闭保护、保留严格优先。
计时从 decode 明确提出需求且没有持有 grant 开始；已有 graph 收到 REVOKE 后仍在
计算，不提前计入无授权等待。重复阶段通知、目标重算不重置计时。
GRANT 发出后继续计时，直到 MEMBERS 就绪，避免低估启动耗时。
如果 REVOKE 与 MEMBERS 交错，本 graph 仍可计算，计时同样应结束；没有注册就被客户端
丢弃的过期 grant 不算恢复。
离开 decode 或进入 IDLE 取消该段等待；保护标志只在 IDLE/进程退出清除。

复用 controller 的 100 ms timer 检查阈值。超时仅设置保底、重算目标并触发 REVOKE，
仍执行 graph 边界 → UNMEMBERS → 恢复 affinity → RELEASE → GRANT → MEMBERS。
UNMEMBERS、REVOKE、断链均不允许提前复用 CPU；断链占用保持到 pidfd 确认退出。
**这是软等待预算，不是 2 秒 token 延迟保证。** 单个长 prefill graph 可使交接晚于阈值，
不通过 TTL 强抢 CPU、终止请求或切换通信方案。

## 测量与对照

CSV 新增保护状态、当前等待、超过预算的时间、触发保护后的恢复耗时。
重点记录 `DECODE_PROTECT` 到 `DECODE_RESUME` 的延迟、最大 decode 无授权等待、
ITL p99/max、injected TTFT 和 aggregate TPS。MEMBERS 表示 workers 就绪，不等于已产出 token。

benchmark 保留 `scx_dynamic_persistent_phase` 为严格优先（明确传 0），新增
`scx_dynamic_persistent_phase_guard`（默认 2000，可覆盖）。现有默认 case 集合不改变。

## 执行顺序

1. 实现分配器 floor、controller 计时/保护与事件记录，补两个聚焦测试并跑原回归。
2. RISC-V 构建；板端真实 OpenMP 验证 8/0 → 7/1 → 4/4、错核和授权不重叠。
3. 真实双 llama 功能测试，再做严格优先/保底的 c=2 异步小规模对照。
   失败则保留证据、停止依赖测试，不引入备选通信方案。
4. 写执行报告。完整多轮性能矩阵不在本轮自动启动。

## 执行状态

以上实现、主机回归、板端验证与两策略小对照已完成，见
[DECODE_GUARD_REPORT.md](DECODE_GUARD_REPORT.md)。功能正确，但单轮真实长 prefill 场景
没有改善最大 ITL；2 秒是软触发阈值的限制已通过事件实测确认。
