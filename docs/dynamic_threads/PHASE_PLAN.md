# Persistent 收尾、N-ready 与 prefill 优先（2026-09-18）

仅修改本实验及独立 llama.cpp worktree；原仓库、scx_llm 和最终提交仓库不动。
运行时仍限制两个实例，benchmark 为 -t 8 -tb 8、OMP_DYNAMIC=FALSE、每 server -np 1。

## 顺序

1. 最终 persistent campaign 已完成，更新报告并同步头文件、导出补丁，不重复性能复跑。
2. 提取集合式分配器：容量限制、稳定均分及 CPU 保留；交接器独立保留释放后再授权。
   模拟 3/8 实例验证算法，但不扩展连接数和 BPF map。先通过原有回归门禁。
3. 添加显式 allocation-policy=equal|prefill-first（默认 equal，仅 persistent 可启用优先）。
   两 decode=4+4，一 prefill 一 decode=8/0，两 prefill=4+4，单实例=8。
   MIXED 含 prompt 视为 prefill；0 表示等待，不执行 0 线程 graph。

## 协议与运行时

- 升级 socket 协议（与 phase hint ABI v4 无关），HELLO 协商策略，ACQUIRE 携带首次阶段，
  PHASE 仅在阶段改变时发送；等分策略不新增阶段 IPC。
- 复用 server/context/backend 的真实阶段提示，普通和缓存计划计算路径均接入。
- 新阶段先上报再等待 grant；PHASE 导致本实例交出授权时，REVOKE 必须先于 ACK。
- graph end 仅撤销/释放，不等下一 grant；避免下一阶段未上报导致死锁。
- 未变的分配保持授权与成员映射；graph 边界合作式交接，不中断正在运行的 graph。
- 没有 grant TTL 或 decode 保底；资源等待可超过 30 秒，即时 RPC 回复仍限时。
- 保留 socket/epoll 用户态策略控制面，BPF 负责 placement；无新增轮询或后台线程。

## 验证与停止条件

原回归 → 阶段专项/长等待/消息交错 → 离线 RISC-V 构建 → 板端/真实 llama →
两策略小规模 simultaneous/asynchronous benchmark。记录 TTFT、TPS、ITL、阶段、
授权/成员/消息数及交接耗时。功能门禁失败保存证据，停止依赖测试并讨论，不回退通信方案。
完整多轮 phase 性能消融和真实 8 实例支持不在本轮。

## 状态

- [x] 最终 persistent 报告与补丁收尾
- [x] N-ready 重构及原回归（14 项、transport、1/2/3/8 实例分配器测试通过）
- [x] 阶段通信与优先策略及主机门禁（只验证 wire v4）
- [x] RISC-V / 板端 / 真实 llama 门禁
- [ ] 三策略完整异步对照（已后台启动）和最终执行报告

阶段代码已实现。只验证当前 wire v4；不保留、不测试旧 wire 兼容接口。
