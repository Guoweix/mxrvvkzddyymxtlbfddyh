# 动态线程与 CPU grant：独立消融实验

> 定位：INFERSCHED 的主调度算法是 `src/scx_llm_pin`（`rvv_dirty_local` + 自动
> phase-aware）。dynamic_threads 是**附录中的对照/独立消融**方案，不作为主线；
> benchmark 通过 `--strategy dynamic`（x86 用 `--strategy x86-dynamic --local`）接入。
> 下文保留其独立实验记录。

本目录实现了 [PLAN.md](PLAN.md) 中的事件驱动方案。它不基于 `scx_llm_pin`，
没有长时间片、gang 或 loader 请求 map 轮询。llama-server 在命令行上
始终使用 `-t 8 -tb 8`；实际 OpenMP team 宽度由 graph 边界上的 grant 决定。

原 graph 模式功能门禁已通过，用户已完成小规模 c=2 性能实验。本轮持续授权实施计划
见 [PERSISTENT_PLAN.md](PERSISTENT_PLAN.md)，记录见 [PERSISTENT_REPORT.md](PERSISTENT_REPORT.md)。
原始执行记录仍保留在 [GATE_REPORT.md](GATE_REPORT.md)。不以完整七组矩阵作为本轮前提。

当前增量见 [PHASE_PLAN.md](PHASE_PLAN.md) 与 [PHASE_REPORT.md](PHASE_REPORT.md)。
N-ready 重构、phase-aware 已通过功能测试及三策略异步 campaign（27 个有效 run），
当前只使用并测试 wire v4。新增 decode 等待保底见 [DECODE_GUARD_PLAN.md](DECODE_GUARD_PLAN.md)，
验证及小对照见 [DECODE_GUARD_REPORT.md](DECODE_GUARD_REPORT.md)，不混入原 campaign。
保底功能已通过验证，但真实长 prefill graph 的最大 ITL 未在本次短测中改善。
缩核时保留存活 worker 及其原 CPU 的实现与验证见 [WORKER_RETENTION.md](WORKER_RETENTION.md)。
当前 persistent/dynamic 已扩展为最多 8 个独立进程，见
[MULTI_PROCESS_PLAN.md](MULTI_PROCESS_PLAN.md) 和 [MULTI_PROCESS_REPORT.md](MULTI_PROCESS_REPORT.md)。
CPU 总数仍为 8，旧 graph 实验模式限两个连接。

## 实现结构

- `control.h`、`client.[ch]`：固定宽度 SOCK_SEQPACKET 协议。消息包含 session、
  单调 request ID、授权 epoch、worker 数、CPU mask 和真实 TID。当前源码 wire 版本为 4，
  与已有 phase hint 的 ABI v4 无关。本实验不提供或测试旧 wire 兼容接口。
- `controller.c`：单线程 epoll 控制器，以 `SO_PEERCRED` 认证客户端，用 pidfd
  处理退出；没有请求 map 轮询。支持 `fixed8`、`fixed4`、`split4`、`dynamic` 和
  `manual`。
- `allocation.[ch]`：集合式分配器，独立计算最多 8 个实例的目标容量及 CPU mask。
  `--allocation-policy prefill-first` 仅可与 persistent 使用，默认 equal。
- `control.bpf.c`：独立的默认时间片 sched_ext。授权成员进入每 CPU custom DSQ，
  其他任务进入 GLOBAL DSQ。
- `probe.c`、`test_control.py`：协议压力、退出隔离、实际 OpenMP team 和错核检查。
- `llama-dynamic.patch`：相对于 `kbuild/llama.cpp` 当前本地基线的可审查补丁；
  `export_llama_patch.sh` 可从独立 worktree 重新生成。
- `run_control_gate.py`：板端探针门禁。
- `run_llama_gate.py`：部署隔离 runtime，验证真实单/双 llama 请求、确定性输出、
  8→4+4 grant、BPF 放置、dmesg 和清理状态。
- `run_benchmark.py`：七组消融 campaign；复用现有 dual-model 确定性 workload，
  保存 TPS、TTFT、ITL、公平性、grant wait 和 team publish 延迟。
- `run_multi_benchmark.py`：真实 2/4/8 进程三策略对照，每进程只有 1 个请求；
  `multi_workload.py` 复用流式采集，统计真正的全进程共同重叠窗口。

llama.cpp 的独立 worktree 是 `llama.cpp/`，由 `.gitignore` 排除；原
`kbuild/llama.cpp` 和最终仓库没有被本实验写入。接口仅在设置
`GGML_DYNAMIC_THREADS_SOCKET` 时启用，未设置时保持原行为。启用后控制器错误会使
graph 失败，不静默退回固定八线程。

## grant 生命周期

默认 `--grant-lifetime graph` 保留原路径；使用 `--mode dynamic --grant-lifetime persistent`
启用持续授权，HELLO 回复的 pin 字段协商模式（其他消息中 pin 仍表示 CPU 绑定）。
两种生命周期使用同一份 patched runtime；persistent 当前只支持 dynamic 分配策略。

持续模式：ACQUIRE 先确认受理，再异步发送 GRANT；稳定 graph 不再发消息、不更新
BPF 成员或重设 affinity，仅在 graph 开始/结束非阻塞检查 socket。实际 team 可以
小于 grant 容量，成员身份变化才刷新成员映射。OpenMP TLS 记录及其退出锁防止恢复
affinity 时误操作复用的 TID；BPF 同时校验 sched_ext enable 时记录的 task
`start_boottime`，复用的数值 TID 不会命中旧成员。额外的
OMP_PROC_BIND/GOMP_CPU_AFFINITY/OMP_PLACES
及显式 worker CPU mask 不受支持，启用时失败而不是覆盖外部放置策略。

资源变化时控制器推送 REVOKE（携带目标宽度和 mask，不是旧授权）；客户端完成当前
graph 后先执行 UNMEMBERS/ACK。纯缩核且保留非空旧核子集时，只恢复退出 worker 的
affinity，以 SHRINK/ACK 局部归还其余 CPU；存活 worker 保留原绑定，保留的 CPU
始终归原实例所有，epoch 不变。下个实际 team 按 TID 身份和 CPU 匹配，仅调整变化
的绑定，然后 MEMBERS/ACK 才开始计算。扩核、换核、降到 0 核仍恢复全部 affinity、
RELEASE/ACK，之后才允许新授权复用全部旧 CPU。实例 idle 取消待授权需求
并释放已持有授权。主计算线程在 graph 间也保持绑定，因此性能变化不应全部归因于 IPC。
由于没有后台客户端线程，撤销在边界处理，错过检查可能再运行一个 graph；不保证即时抢占。

持续模式的资源等待和撤销响应没有 30 秒超时；30 秒只约束即时协议回复。
整体停滞由实验 watchdog 处理，绝不通过 TTL 强行回收正在使用的 CPU。

phase-aware 源码通过 HELLO 协商策略，ACQUIRE 携带首次阶段，PHASE 只上报阶段变化。
两 decode / 两 prefill 均为 4+4；一 prefill 一未受保护 decode 为 8/0，MIXED 按 prefill。
0 表示阻塞等候授权，不创建零线程 team。新阶段先报告再等 grant；PHASE 引起撤销时，
REVOKE 排在 ACK 之前，graph end 只处理归还而不等待下一授权。抢占仍发生在 graph 边界。
`--decode-wait-ms 2000`（prefill-first 的默认值）在 decode 无可用授权等待超过阈值后
给它 1 核保底，双实例变为 7/1；该保护直到实例 IDLE 才清除。`--decode-wait-ms 0`
关闭超时触发的保底；若同时指定 `--decode-floor-on-prefill`，仍在 prefill 到来时立即
为每个 decode 保留 1 核。复用 controller 的 100 ms timer，不增加客户端 IPC。
这不是授权 TTL 或出 token 的硬期限：仍需等持有者在 graph 边界释放 CPU。

以下是原 graph 模式的路径：

动态模式下，单个活跃 server 获得 8 worker/CPU0–7；两个活跃 server 分别获得
4 worker/CPU0–3 和 4 worker/CPU4–7。每个 graph 的顺序是：

```text
ACQUIRE → 先设置 worker affinity → MEMBERS → graph compute
        → UNMEMBERS → 恢复 affinity → RELEASE
```

`UNMEMBERS` 是必要的独立阶段：它保证 BPF 成员映射只在 worker 已固定到授权 CPU
期间可见，避免发布和撤销边界上的错核窗口。正在运行的 grant 没有 TTL；断链时
保持隔离，直到进程通过 pidfd 确认退出。

## 构建与门禁

控制器和探针：

```sh
cd src/dynamic_threads
make host all
python3 test_control.py
python3 test_persistent.py
python3 -m unittest -v test_multi test_multi_metrics
build/test_transport
build/test_allocation
REMOTE_PASS='<board-password>' python3 run_control_gate.py --iterations 1000
```

RISC-V llama.cpp 使用 Docker 内 GCC/G++ 14。当前构建目录配置为
`rv64gcv_zfh_zvfh_zicbop_zihintpause/lp64d`、OpenMP 开启、native/测试/examples/curl 关闭：

```sh
docker run --rm --network none \
  -v "$PWD/src/dynamic_threads:/work" \
  -w /work/llama.cpp riscv-nteam-toolchain:local \
  sh -lc 'cmake --build build-dynamic-riscv --target llama-server -j4'

python3 src/dynamic_threads/run_llama_gate.py --output-tokens 16
```

持续模式板端门禁：

```sh
python3 src/dynamic_threads/run_control_gate.py --grant-lifetime persistent --iterations 1000
python3 src/dynamic_threads/run_llama_gate.py --grant-lifetime persistent --output-tokens 16
```

脚本只在板端独立 `/tmp/<run-id>/` 部署 runtime，不覆盖
`/mnt/llm/llama/bin`。运行前要求 sched_ext 未启用；退出时检查 scheduler 已卸载。

## 消融 benchmark

真实多进程短测（先设置现有 REMOTE_* 环境）：

```sh
python3 run_multi_probe.py
python3 run_multi_benchmark.py --process-counts 2,4,8 --loops 1 --output-tokens 32 --interleave
```

三组默认为 CFS、完整 RELEASE、缩核保留。每进程 `-t 8 -tb 8 -np 1 -c 1024`，
共用同一模型文件；有 prefill 时每个 decode 保底 1 核，余核由 prefill 均分。
例如 1P+3D 为 5/1/1/1，2P+2D 为 3/3/1/1；8 个活跃进程时每进程 1 核。
全 decode 时均分，余数按稳定 slot 顺序分配（3 个进程为 3/3/2）。
前半进程全到第 4 token 后注入后半进程；配对 prompt 各 128 token。
`process_count` 表示真实 PID 数，`requests_per_process` 固定为 1。
启动/预热时保存 MemAvailable 和各进程 RSS/PSS；模型含私有 repack 缓冲，不能假定
所有权重都由 mmap 共享。每组前后检查板端空闲，只清理本次创建的进程。
正式测试可将参数改为 `--loops 3 --output-tokens 256 --interleave`，后台启动即可。

以下为原双 server 的请求并发实验：其 concurrency=8 表示两个 server 各 4 个请求，
并非 8 个进程，结果目录及统计与上述实验独立。

快速编排 smoke：

```sh
python3 src/dynamic_threads/run_benchmark.py \
  --cases cfs_baseline,scx_dynamic --scenarios simultaneous --smoke
```

正式默认配置正是计划中的两场景、c=2/4/8、三次重复和每请求 256 输出 token：

```sh
python3 src/dynamic_threads/run_benchmark.py
```

七组依次为 `cfs_baseline`、`scx_minimal`、`scx_fixed8`、`scx_fixed4`、
`scx_split4`、`scx_dynamic` 和 `cfs_dynamic`。case 和每个 case 内的 run 都按固定
seed 打乱，输出写入忽略版本管理的 `results/<campaign-id>/`。正式 campaign 很长，
不要把 `--smoke` 数据并入正式统计。

新增显式 case `scx_dynamic_persistent` / `cfs_dynamic_persistent`，不改变默认七组。
phase 对照使用 `scx_phase_dirty_lease` 与 `scx_dynamic_persistent_phase`；前者启动
ABI v4 N-team dirty-lease scheduler，后者启动本实验 persistent prefill-first 控制器。
缩核保留消融使用 `scx_dynamic_persistent_phase_reserve1` 与
`scx_dynamic_persistent_phase_reserve1_full_release`；后者只通过
`GGML_DYNAMIC_THREADS_RETAIN_SHRINK=0` 恢复完整 RELEASE，其他配置不变。
`scx_dynamic_persistent_phase` 显式关闭保底以保持历史含义；新增
`scx_dynamic_persistent_phase_guard` 启用保底，benchmark 的 `--decode-wait-ms` 只调整此 case。
另有 `scx_dynamic_persistent_phase_reserve1`：同样将超时设为 0（无限等待、不做超时切换），
但只要 prefill 与 decode 共存，就在 prefill 第一次获准前直接确定 7/1；用于区分
“保留 1 核是否有效”和“超时后无法跨越长 graph 交接”两个问题。
两者的 c=2 异步小对照：

```sh
python3 src/dynamic_threads/run_benchmark.py \
  --cases scx_dynamic_persistent_phase,scx_dynamic_persistent_phase_guard \
  --scenarios asynchronous --smoke --decode-wait-ms 2000
```

本轮对照使用以下命令，`--interleave` 会交替各次重复的 case 顺序：

```sh
python3 src/dynamic_threads/run_benchmark.py \
  --cases scx_dynamic,scx_dynamic_persistent --interleave \
  --scenarios asynchronous --concurrencies 2 --loops 3 --output-tokens 256
```

## 历史原型

`handshake.*` 和 `run_gate.py` 保留最初 mmap + `sched_yield` 原型及失败证据。
它不是当前实现的依赖，也没有被偷偷替换成 loader 轮询。当前方案采用经讨论确认的
Unix SOCK_SEQPACKET 控制面；任何新的门禁失败仍应停止其后续性能步骤并保留证据。
