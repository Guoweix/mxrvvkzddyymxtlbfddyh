# Decode 等待保底执行记录（2026-09-19）

实现范围和规则见 [DECODE_GUARD_PLAN.md](DECODE_GUARD_PLAN.md)。代码仅修改本实验目录；
llama.cpp runtime、wire v4、BPF 调度器及原仓库均未修改。

## 实现

- controller 增加 `--decode-wait-ms`，prefill-first 下默认 2000，0 关闭保底。
- decode 真正开始无授权等待后计时，触发后保留 1 核，直到实例 IDLE 才清除保护。
  MIXED/prefill 阶段变化保留该标记；全部进入 decode 时仍正常均分。
- 分配器先保底再按优先级均分，优先保留 CPU。真正交接仍等待 RELEASE；断链保持隔离。
- 新增等待、超预算、保护后恢复事件和指标；benchmark 增加 ITL p99/max。
- `scx_dynamic_persistent_phase` 保留严格优先；新增 `scx_dynamic_persistent_phase_guard`。
  两者显式记录各自阈值，不改变原默认 case 集合。

计时修正包括一个实际消息交错：REVOKE 可能在 MEMBERS 注册期间到达。此时本 graph
仍有权完成计算，所以 MEMBERS 必须结束该段等待，不能把随后的计算误判为饥饿。
仅发送 GRANT、没有完成 MEMBERS 则不算恢复。

## 功能验证

新增两个聚焦场景测试：黏性保护/阶段变化/IDLE 重置，以及运行中 graph 不计等待/
安全交接/MEMBERS 恢复计时。另复用原有回归，不引入新的多层验收流程。
分配器验证 P1+D1、P1+D3、P2+D2、P1+D7，及无 prefill 时的均分、上限和不可用 CPU。
这里的 8 实例仅为分配器测试，真实运行仍限两个进程。
最终完整主机回归 25/25 通过（原 23 项 + 新增 2 项），分配器和 transport 测试通过。
逐场景证据保存在 `results/decode-guard-host-e4FJff/`。

最终 controller 板端证据：
[control-20260918T163847Z-c53fb878](results/control-20260918T163847Z-c53fb878/summary.json)。
两个真实 OpenMP 探针各运行 200 个 graph，观察到 decode=1、prefill=7，随后各为 4；
实际 mask 包含 1/254、225/30，验证按所有权审计而非假定连续核。
7 次 grant 无重叠，BPF 无错核、内核无新增告警，调度器正常卸载。
本次触发保护到 MEMBERS 就绪为 **7.76 ms**；稳定单实例仅 1 次 GRANT、1 次 MEMBERS、2 次 PHASE。

真实 llama 功能证据：
[llama-20260918T163659Z-84a6e5ef](results/llama-20260918T163659Z-84a6e5ef/summary.json)。
单/双实例均完整输出 16 token，双实例确定性输出一致；7 次 grant、5 次成员注册，
BPF `wrong_cpu=0`，所有权审计、内核和清理检查通过。
该次使用计时边界修正前的 controller；后续异步对照使用修正后的最终构建。

最终 controller SHA256：`056a8f31533051d0c6853e164c4033e67ff39b8dbbf002b2258bbc8225ccbbda`。
llama-server 与本轮之前保持一致，不需要重新编译 llama.cpp。

## 小规模异步对照

证据：[benchmark-20260918T163903Z-cb95ef4b](results/benchmark-20260918T163903Z-cb95ef4b/campaign.json)。
严格优先与 2000 ms 保底，c=2、128-token prompt、每请求输出 16 token、各 1 次。
两个 run 均有效、输出确定性检查通过、BPF 无错核、授权无重叠、内核与卸载检查通过。
这里只验证端到端行为，不作为正式性能统计；未启动完整多轮 campaign。

| 指标 | 严格 prefill 优先 | 2000 ms 黏性保底 |
| --- | ---: | ---: |
| aggregate TPS | 0.925 | 0.917 |
| injected TTFT | 13.010 s | 13.082 s |
| incumbent 最大 ITL | 13.307 s | 14.573 s |
| decode 最大无授权等待（至 MEMBERS） | 12.707 s | 12.782 s |
| 保护触发次数 | 0 | 1 |
| 触发保护 → MEMBERS 就绪 | — | 10.704 s |
| GRANT / MEMBERS / 协议消息数 | 5 / 5 / 56 | 7 / 6 / 68 |

本次样本**没有缩短最长 decode 停顿**。事件表显示保护在等待约 2.077 s 时触发，
但 128-token prefill graph 尚未结束，又等约 10.704 s 才完成恢复。实际发出了 1/7，
随后 prefill 转 decode，目标又变为 4/4。这次交错还让 incumbent 完成了一个 1-worker
graph 才切回 4 核；这是最长 ITL 比严格优先更长的直接候选原因，不能把增加的消息数
简单解释成逐 token 通信回归。

结论：**保底分配与安全交接功能有效，但仅靠当前 graph 边界的超时保底，不能解决
这个长 prefill graph 的延迟问题。** 需要更短可交接边界才可能接近 2 秒等待目标；本轮
不擅自改变 prefill 切分或引入新的抢占/通信方案，也不据此单轮短测宣称整体性能回退。

## 后续：8/0 与预先 7/1

新增 `--decode-floor-on-prefill`。它不等待超时：当 prefill 与 decode 共存时，目标直接
计算为 7/1，prefill 不会先获得 8 核再等待长 graph 归还。无 prefill 时 decode 仍正常均分。
聚焦测试确认 prefill 的首次 grant 为 7、decode 为 1，没有 `DECODE_PROTECT` 事件。

2026-09-19 已后台启动正式小对照：两边都传 `--decode-wait-ms 0`，即超时为无限长；
比较严格 8/0 与预先 7/1，异步 c=2、256 输出 token、交错顺序 3 轮。campaign 为
`results/benchmark-20260919T004828Z-f536d7ec/`，启动日志为
`results/prefill-exclusive-vs-reserve1-20260919T004800Z.log`。本报告不持续轮询该任务，
完成后以 campaign.json 的 `complete` 和每个 run 的 `valid` 为准。

## 限制

2000 ms 是触发保护的软预算，**不是最大 ITL**。当前 graph 不可中断，触发后仍可能
等很久才能释放 CPU；1 核就绪也不等于立即生成 token。是否需要更细的 prefill graph
边界，应根据真实对照的恢复延迟决定，本轮没有改 batch/ubatch 或 graph 切分。
