# 缩核时保留存活 worker 的原 CPU

## 范围与修改前基线

按用户要求，先检查 Git 并提交现有工作，再实现本增量：

- `experiments`：`726e146`，decode 等待保底与 prefill 预留核心对照。
- 独立 `llama.cpp` worktree：`403a849`，动态线程与 phase-aware 实验基线；
  位于 `exp/dynamic-threads-worker-retention` 分支。

本次不修改分核比例、时间片、数据分块、cache 拓扑策略或实际实例上限。
不修改 `kbuild/llama.cpp` 原 worktree、`scx_llm_pin` 或最终提交仓库。
`-t/-tb` 仍均为 8。

## 实现

旧实现已经在 allocator 层尽量保留 CPU 集合，但 grant 变化时仍先解绑全部
worker、释放全部 CPU，再重新绑定。仅优化下一次 CPU mask，无法消除这个窗口。

现在 persistent 模式的 REVOKE 携带目标宽度和目标 mask；当前 graph 仍使用旧 grant。
目标是旧 mask 的非空严格子集时，在 graph 边界执行：

```text
graph 结束 → UNMEMBERS/ACK → 只恢复被移除 worker 的原 affinity
           → SHRINK/ACK（只归还移除的核，保留其余核的所有权）
           → 观察下个实际 OpenMP team → 增量绑核 → MEMBERS/ACK → graph
```

- `SHRINK` 是 wire v4 中新增的操作；112 字节布局不变，无旧 wire 兼容层。
  REVOKE 字段含义改变，controller 与 runtime 必须成套部署。
- 保留 grant 的 session/epoch，不重新排队；旧成员映射已撤销，下一次 MEMBERS
  才恢复 BPF placement。不存在先把保留核心分给别人、再试图拿回来的窗口。
- 当前分配器缩核保留按 CPU 编号排序的前缀，恰好对应存活的低编号 OpenMP worker。
  team 记录实际线程的 TLS 身份和原 CPU，只有两者匹配才跳过 affinity 系统调用。
  不能仅凭 `ith` 或可复用的数值 TID 判断线程未变。
- 实际 team 宽度在同一 grant 内变化时，也只调整改变的成员。OpenMP 若退出、重建
  或重排线程，按实际身份重新处理，不假定 runtime 一定保留旧线程。
- 新 libgomp worker 可能继承仍被固定的主线程 affinity；即使已 UNMEMBERS，
  仍用 team 初始化时的合法范围作为恢复基线。远程恢复继续受 TLS 退出锁保护。
- 扩核、换核、降到 0 核、IDLE 继续使用完整 detach/RELEASE 路径。
  本轮没有实现扩核跨 grant 的绑定保留。

若目标在旧 REVOKE 发出后变化，controller 仍可安全接收旧 grant 的严格子集归还，
随后用新的 REVOKE 对齐最新目标。未 UNMEMBERS、归还集合含未持有核心或 epoch 错误
均拒绝。失败不回退；断链后保留部分的所有权仍隔离到 pidfd 确认退出。

统计同步识别 SHRINK：所有权审计只释放被移除的核；handoff 按事件顺序匹配，
避免同一 epoch 多次缩核被旧的字典统计覆盖。稳定 graph 仍无逐 token RPC。

## 验证

- 主机协议、persistent、phase、decode 保底及局部归还回归通过。
- `test_worker_team.c` 使用真实 OpenMP/affinity，协议端点为 stub：
  `8→4`、`4→1`、`7→4` 保留相同 TID/CPU；对存活 worker 的
  `sched_setaffinity` 调用增量为 **0**，移除 worker 恢复原合法范围。
  同时覆盖同一 grant 内的实际宽度变化、线程重建和最终 affinity 恢复。
  主机与 RISC-V 板端均通过。该测试不测 cache 命中率。
- Docker 内 GCC/G++ 14 的 RISC-V `llama-server` 构建成功；离线 UI 素材缺失警告
  不影响本实验使用的推理接口。
- 板端实际 runtime + sched_ext：
  `results/control-20260919T033605Z-06ee538d/summary.json`。
  immediate 7/1、等待超时关闭；单实例稳定阶段仅 1 GRANT / 1 MEMBERS / 2 PHASE。
  双实例 400 个 graph，实际出现 `8→1` 和 `7→4` 两次 SHRINK；
  4 次 GRANT、CPU 所有权无重叠、BPF wrong_cpu=0、无新增内核警告且 scheduler 正常卸载。
- 真实 llama.cpp 单/双实例短测通过：
  `results/llama-20260919T033650Z-97c4c649/summary.json`。
  persistent + prefill-first、decode 等待超时关闭、每请求 16 输出 token、`-t 8 -tb 8`。
  双实例确定性输出一致；实际出现 2 次 SHRINK，6 次 GRANT、5 次 MEMBERS；
  所有权无重叠、wrong_cpu=0、stale_identity=0，内核及 scheduler 清理正常。
  此短测走 strict prefill 优先；immediate 7/1 的精确交接由上述板端 runtime 测试覆盖。
- `git diff --check` 通过；独立 llama.cpp 源码已同步导出至 `llama-dynamic.patch`。
  本轮未启动完整 TPS/TTFT/ITL campaign，新增修改保留在工作区供审查。

复现主要检查（板端命令先设置现有 REMOTE_* 环境）：

```sh
make -j4 host
build/test_transport
build/test_allocation
build/test_worker_team
python3 -m unittest -v test_control test_persistent test_phase test_decode_guard test_shrink
python3 run_control_gate.py --grant-lifetime persistent --allocation-policy prefill-first \
  --decode-wait-ms 0 --decode-floor-on-prefill --iterations 200
python3 run_llama_gate.py --grant-lifetime persistent --allocation-policy prefill-first \
  --decode-wait-ms 0 --output-tokens 16
```

## 边界

此改动消除了存活 worker 不必要的解绑/迁移，并不保证其 cache 不被驱逐。
缩核后 ggml 仍按新的 worker 数划分任务；被移除核心的私有 cache 不会自动迁移，
cache 命中率和 TPS/TTFT/ITL 是否改善仍需独立性能对照。

为便于该对照，llama runtime 提供仅供消融的
`GGML_DYNAMIC_THREADS_RETAIN_SHRINK=0`：它保持新版协议和分配策略不变，只把
REVOKE 处理恢复为完整 detach/RELEASE。默认未设置时仍启用缩核保留。
