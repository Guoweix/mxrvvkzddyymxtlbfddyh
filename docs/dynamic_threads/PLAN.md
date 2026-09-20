# 动态线程与 CPU grant：事件驱动控制方案

## 执行状态（2026-09-17）

- [x] SOCK_SEQPACKET 协议、epoll 控制器和主机单/双进程 10,000 次压力测试。
- [x] 独立默认 slice sched_ext、真实 OpenMP team 和板端 CPU 放置门禁。
- [x] llama.cpp graph plan/compute 动态 worker、活动通知、成员生命周期和 affinity 恢复。
- [x] Docker 内 GCC/G++ 14 RISC-V 构建，以及单 8 核/双 4+4 真实请求门禁。
- [x] 七组消融 benchmark harness 和两组短 smoke。
- [ ] 正式 128-input/256-output、c=2/4/8、两场景、每组至少三次 campaign。

后续范围调整：用户已完成足够的小规模性能实验，本轮不要求先跑完整 campaign。
当前持续授权修改以 [PERSISTENT_PLAN.md](PERSISTENT_PLAN.md) 为准；仍不进入
N-team、elastic gang 或 phase-aware 组合。

## 已确认的目标与边界

用户态控制器通过 Unix SOCK_SEQPACKET + epoll 接收请求，决定实际线程数和
CPU grant；llama.cpp 在 graph 边界应用；独立 sched_ext BPF 执行 worker 放置。
所有 benchmark server 默认 `-t 8 -tb 8`。第一版为两个实例、CPU0–7。
不继承 scx_llm_pin 策略，不添加 phase 优先、长时间片或 gang。

工作保留在本目录，llama.cpp 在保留当前本地修改的独立副本开发。原实验及最终
提交仓库不修改。这是 AI 辅助的本地实验，不自动提交至上游。

旧 mmap + sched_yield 的失败证据保留于 GATE_REPORT.md；失败原因未确定。
原通路修复不再是新方案前提。新通路任何门禁失败，停止依赖步骤并保存证据，
不自动替换通信方式。

## 接口和授权

消息包括注册、活动开始/结束、graph 申请、worker 注册/撤销、释放及确认。
包含版本、session、请求编号、graph epoch；grant 返回线程数、CPU mask 和放置模式。
使用 SO_PEERCRED 验证进程，通过 pidfd 处理进程退出。控制器事件驱动，不轮询请求 map。
每个实例最多一个在途 graph；注册连接不代表活跃推理，server 通过活动任务数
0↔非0的变化报告需求。运行时在单个 graph 内保持线程数不变。

动态规则：单活跃实例获八核；双活跃实例各四核，稳定分为 CPU0–3 和 CPU4–7。
已有 graph 不撤销授权；完成计算和 OpenMP join 后释放，控制器确认后方可复用。
按申请先后处理，等待者不被刚释放的实例越过。worker 先设置授权 affinity，成员
列表注册并获确认后才能开始计算；计算后先撤销成员列表，再恢复原 affinity 并释放
grant。BPF 只在该区间使用同一授权。

请求等待上限默认 30 秒，不用 TTL 回收正在计算的授权。连接异常但进程未退出时
隔离其授权，直至确认释放或进程退出。协议错误、控制器或调度器退出使实验失败，
不静默回退八线程。接口未启用时维持原行为。

## 实施门禁

1. 实现独立协议、控制器、模拟客户端。验证单/双进程各连续 10,000 次协议循环、
   公平性、异常消息、超时及进程退出。
2. 接入独立默认 slice 的 BPF 调度器；板端计算探针核对实际 worker CPU 放置。
3. 新门禁通过后，llama.cpp 在 ggml_graph_plan 前取得授权，包含预生成 plan 路径；
   实际 OpenMP team 与授权一致；实现同一进程 8→4→2→8。
4. 接入活动通知、成员注册及释放；固定分配通过后运行自动分配。
5. 功能验证通过才进行性能测试，负收益不触发自动添加其他策略。

llama.cpp 使用 Docker 内 RISC-V GCC/G++ 14、OpenMP 和独立构建目录；先处理
现有容器启动问题。板端使用独立 runtime，部署同次构建的 server 及配套共享库。
设置 OMP_DYNAMIC=FALSE；实际 team 大小不符时停止该轮。

## 消融与验收

同一 patched runtime，关闭旧策略接口：

| 组别 | 实际 worker | CPU 使用 |
|---|---|---|
| CFS 基线 | 8+8 | 共享八核，接口关闭 |
| 最小 sched_ext | 8+8 | 共享八核，接口关闭 |
| sched_ext + 固定8接口 | 8+8 | 共享八核 |
| sched_ext + 固定4接口 | 4+4 | 共享八核 |
| sched_ext + 固定4分核 | 4+4 | 不重叠四核 |
| sched_ext + 动态授权 | 单8 / 双4+4 | 动态分核 |
| CFS + 相同动态授权 | 单8 / 双4+4 | 相同 affinity |

功能检查计划/实际线程数、成员 epoch、授权互斥、加入/退出/异常清理、确定性输出，
无死锁、NaN、watchdog 或内核异常。性能沿用128输入/256输出、c=2/4/8，覆盖同时到达
和 incumbent/injected，每组至少三次并轮换顺序。记录 TPS、TTFT、ITL、公平性、授权
等待和通信耗时；barrier/futex 诊断单独运行。系统任务和控制器不排除在八核之外。

通信开销通过接口固定8组实测。N-team、elastic gang、phase组合留待本轮结果确认后规划。
