# 多进程扩容实施与短测

日期：2026-09-19。基线：实验仓库 `40e51ae`，独立 llama.cpp `252fa6b`。
实施范围见 [MULTI_PROCESS_PLAN.md](MULTI_PROCESS_PLAN.md)。

## 实现

- persistent/dynamic 从 2 个连接扩为最多 8 个独立进程；slot 为 0～7。
  wire 仍为 ABI v4 / 112 字节，单进程最多 8 worker，物理 CPU 仍为 0～7。
  BPF grants/members map 容量随 `DT_TEAMS` 扩为 8/64；不新增通信协议。
- 旧 graph 实验模式仍限制 2 个连接，旧 `slot * 4` 分核逻辑不会进入 persistent 路径。
  第 9 个连接拒绝接入。epoll 事件增加连接代次，防止 slot 复用后误处理旧事件。
- 复用集合式分配器及现有 phase-aware、SHRINK、worker/CPU 留存机制。
  本次 benchmark 使用 `--decode-wait-ms 0 --decode-floor-on-prefill`：每个 decode
  立即保留 1 核，余核由 prefill/MIXED 均分。全 decode 时均分，余数按 slot 顺序分配。
  断链持有的 CPU 仍隔离到 pidfd 确认退出。

例：3D 为 3/3/2，1P+3D 为 5/1/1/1，2P+2D 为 3/3/1/1；8 个活跃进程各 1 核。
所以“支持 8 进程”不代表增加物理算力，也不保证满载时 prefill 仍有额外核心。

修改仅在 `experiments/dynamic_threads/` 及其独立 llama.cpp worktree；原
`kbuild/llama.cpp`、`scx_llm_pin` 和最终提交仓库未修改。

## 验证

主机验证：

- 原 `test_control/test_persistent/test_phase/test_decode_guard/test_shrink`：30 项通过。
- `test_multi`：4 项通过，覆盖 1～8 连接逐步加入、第 9 个拒绝、断链隔离、退出、
  slot 复用、多进程 phase 分配及 8 个真实 OpenMP runtime。
- `test_multi_metrics`：2 项通过，验证共同重叠窗口、跨窗口 ITL 排除及无重叠情况。
- `test_transport`、`test_allocation`、`test_worker_team` 通过。
  存活 worker 的缩核测试仍保持 TID/CPU 不变且不重新调用 affinity。
- RISC-V controller/BPF/probe 构建通过；llama-server 使用 Docker 内 GCC/G++ 14 构建通过。

板端 8 个真实 OpenMP runtime 探针通过：同时发布的计算 team 峰值为 8，
22 次 GRANT、9 次 SHRINK，CPU 授权无重叠，`wrong_cpu=0`，退出与内核日志检查正常。
连接代次耗尽路径的 `pidfd=-1` 初始化修正后，以当前重新构建的 controller 再次通过；
证据：[最新探针 summary](results/multi-probe-20260920T112716Z-79467dc5/summary.json)。

## 真实 llama-server 短测

新增 `run_multi_benchmark.py` 和 `multi_workload.py`，不改变旧双 server benchmark。
`process_count=2/4/8` 是实际 PID 数，不是旧的进程内请求并发。

三策略为 CFS、phase-aware + 完整 RELEASE、phase-aware + 缩核保留。
每进程使用同一 `Llama-3.2-3B-Instruct-Q4_K_M.gguf`、CPU-only、
`-t 8 -tb 8 -np 1 -c 1024 --cache-ram 0 --fit off`。
前半进程全部输出第 4 个 token 后注入后半进程，每对使用相同的 128-token prompt，
每请求输出 32 token，各 1 轮，共 9 组；不自动启动正式三轮/256-token 测试。

共同重叠窗口为所有请求的最晚发送至最早末 token；只有两端均在窗口内的相邻 token
才计入重叠 ITL。总 TPS 为全部输出 token / 整组 makespan，包含 prefill 时间。
保留逐 token 时间、每进程指标、N 路公平性、PID/端口/slot 映射、二进制哈希、
controller 事件及内存快照。配对输出 token 哈希另行记录，不以哈希相等代替协议验证。

短测 9/9 有效，campaign `complete=true`、`kernel_clean=true`，所有配对输出 token
哈希一致。SCX 六组均为 `wrong_cpu=0`、CPU ownership 无重叠；完整 RELEASE 组
没有 SHRINK，缩核保留组在 2/4/8 进程分别观察到 2/6/12 次 SHRINK。
证据：[campaign](results/multi-20260919T122036Z-28647cf2/campaign.json)。

| 进程 | 策略 | 总 TPS | 注入 TTFT (s) | 全程 ITL p95 (s) | 相对 CFS：TPS / TTFT / ITL |
|---:|---|---:|---:|---:|---:|
| 2 | CFS | 1.124 | 22.318 | 2.166 | 基线 |
| 2 | 完整 RELEASE | 1.444 | 15.652 | 1.823 | +28.5% / -29.9% / -15.8% |
| 2 | 缩核保留 | 1.443 | 15.726 | 1.820 | +28.3% / -29.5% / -16.0% |
| 4 | CFS | 0.900 | 44.549 | 3.973 | 基线 |
| 4 | 完整 RELEASE | 1.468 | 35.888 | 1.943 | +63.2% / -19.4% / -51.1% |
| 4 | 缩核保留 | 1.465 | 35.955 | 1.949 | +62.9% / -19.3% / -50.9% |
| 8 | CFS | 0.873 | 89.242 | 7.267 | 基线 |
| 8 | 完整 RELEASE | 1.292 | 89.394 | 2.044 | +47.9% / +0.2% / -71.9% |
| 8 | 缩核保留 | 1.228 | 95.228 | 2.020 | +40.6% / +6.7% / -72.2% |

以上仅为一轮 32-token 功能短测，不能作为稳定性能结论。2、4 进程下两种 SCX
实现都同时改善吞吐、注入 TTFT 和 ITL；缩核保留与完整 RELEASE 的差异不超过
约 0.5%。8 进程下完整 RELEASE 优于缩核保留：后者相对完整 RELEASE 总 TPS
低 5.0%、注入 TTFT 高 6.5%，没有观察到留核优势。

8 进程 SCX 的全进程共同重叠窗口为 0，因此对应 overlap ITL 和 N 路 Jain 公平性
有意记为 `null`，不是采集失败。等待“所有 incumbent 均到第 4 token”期间出现明显
进度偏斜：最快 incumbent 已完成，第二波客户端线程才真正发送请求。CFS 的共同窗口
仍有 182.7 s。这说明当前 phase/grant 粒度在 8 进程时倾向于集中资源，能提高总吞吐
并显著压低 decode ITL，但会牺牲尾部 TTFT/同步推进公平性；正式测试必须保留这一现象，
不能用缺失的共同窗口指标宣称 8 进程公平性改善。

内存按实际测量控制启动：逐个加载、预热并记录 MemAvailable/RSS/PSS；下一进程
按已观测的单进程加载增量加 512 MiB 余量检查。RVV repack 存在私有权重缓冲，
不能把 GGUF 的 mmap 共享等同于所有模型权重零增量共享。该检查不是内存硬上限。

8 进程三组的最低 MemAvailable 均约 3.35 GiB；8 个进程的 PSS 合计约 13.39 GiB，
未触发 OOM。campaign 结束后 sched_ext、controller 与 server 均已清理，dmesg 增量为空。

## 交付判断

8 进程扩容的功能实现已满足交付门槛：协议兼容、容量/slot 复用、实际 OpenMP runtime、
真实 2/4/8 server、CPU ownership、错核、内存和退出清理均通过。当前工作区尚未提交，
因此交付前还需要审阅 diff 并分别处理实验仓库和独立 llama.cpp worktree 的提交。

性能策略尚不宜定型：短测只有一轮，且 8 进程暴露 TTFT/公平性问题。若下一阶段要给出
性能结论，应运行三轮正式测试，并为“全进程共同窗口为空”补充 injection-relative、
per-process TTFT 分布或活跃集合公平性指标；这不阻塞本次多进程功能代码交付。
