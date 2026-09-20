# 真正的 1～8 进程支持

基线：实验仓库 `40e51ae`，独立 llama.cpp `252fa6b`。

1. persistent/dynamic 扩为 8 个连接，CPU 仍是 0～7；wire v4/112 字节不变。
   BPF grants/members 容量分别为 8/64，epoll 带连接代次，旧 graph 模式限 2 连接。
2. 每个 decode 保底 1 核；余核由 prefill/MIXED 均分，全 decode 时均分。
   继续在 graph 边界 SHRINK/RELEASE；断链隔离到 pidfd 确认退出。
3. 独立多进程 benchmark：每 server `-t 8 -tb 8 -np 1 -c 1024`。
   前半组全到第 4 token 后注入后半组；每对相同的 128-token prompt。
   CFS、完整 RELEASE、缩核保留三策略，记录进程映射、内存、逐 token 时间与授权。
   共同重叠窗口为最晚发送到最早结束，ITL 两端均须在窗口内。
4. 主机回归及多连接检查 → 板端 8 进程 OpenMP 探针 → 三策略 2/4/8 个真实
   llama-server，各 1 轮、32 输出 token。错误保存证据，停止依赖步骤。

正式三轮/256 token campaign 留待短测完成后另行启动；历史双进程 benchmark
仍表示进程内请求并发，不与新的 process_count 混用。
