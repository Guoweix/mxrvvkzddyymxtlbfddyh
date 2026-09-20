# 持续 grant 实施计划（2026-09-18）

本轮只改变授权生命周期，保留单实例 8 核、双实例 4+4 分核。phase-aware、
prefill 优先、gang 和时间片调整不在本轮范围。开发只在本实验及独立 llama.cpp
worktree，benchmark 保持 `-t 8 -tb 8`、`OMP_DYNAMIC=FALSE`。

## 协议和运行时

- 保留显式 graph/persistent 模式，同一新构建用于消融；默认 graph。
- SOCK_SEQPACKET 协议 v3 在 HELLO 返回模式，ACQUIRE 立即确认已受理，之后推送
  GRANT。授权代次不再是 graph 序号。新增异步 REVOKE，客户端统一分派通知与回复。
- 活跃期间保留 grant、真实成员映射和 affinity；稳定 graph 只做非阻塞接收检查，
  不发送协议消息或更新 BPF map。实际 OpenMP 成员变化才刷新成员。
- graph 边界处理 REVOKE：完成计算 → UNMEMBERS/ACK → 恢复 affinity →
  RELEASE/ACK → 新 GRANT → 绑定和 MEMBERS/ACK → 计算。旧 grant 未释放不得复用。
- 活跃变化重新计算目标分配，合并交接期间的需求变化；idle 取消等待并释放授权。
- 30 秒只用于即时协议回复，不作为资源等待、graph 完成或授权 TTL。
  断链时隔离旧授权至 pidfd 确认退出；错误停止，不采用备用通信或静默回退。
- 持久保存合法 CPU 范围，处理新线程继承窄 affinity、线程退出/重建及 TID 复用。

## 顺序与门禁

1. 协议/控制器：稳定 10,000 graph 零重复握手、8→4+4→8、取消/合并、过期代次、
   断链隔离、超过 30 秒的 graph、异步回复交错与有界发送背压。
2. OpenMP：稳定成员、线程重建、小于 grant 上限的实际 team、反复变化与 idle
   恢复；授权不重叠，BPF wrong_cpu=0。Docker 内现有 RISC-V 工具链构建。
3. 真实 llama 确定性输出与内核清洁门禁通过后，再比较 scx_dynamic 与
   scx_dynamic_persistent，c=2 异步注入，128 输入/256 输出，交错各 3 次。
   记录 TPS、TTFT、ITL、消息与成员更新次数、排队及交接耗时。

功能门禁失败则停止依赖测试，保存证据并讨论，不自动替换方案。最近的小规模
性能实验已经足够作为本轮起点，不要求先跑完整七组 c=2/4/8 campaign。

## 实施状态

- [x] 协议与控制器
- [x] 持续成员和 affinity
- [x] 主机功能门禁（14 项及传输层专项）
- [x] RISC-V 与板端功能门禁
- [x] 小规模性能消融及报告
