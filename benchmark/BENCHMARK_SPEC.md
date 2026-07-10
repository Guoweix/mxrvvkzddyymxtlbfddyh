# 标准化LLM性能测试规范

## 测试目的

为RISC-V Vector Extension (RVV) + sched_ext调度器优化项目提供标准化的性能基准测试。

## 测试配置

### 硬件环境
- **开发板**: Milk-V Jupiter M1 (SpaceMit K1)
- **CPU架构**: RISC-V64, 8核心, 2-cluster设计
  - Cluster 0: CPU 0-3 (共享L2缓存)
  - Cluster 1: CPU 4-7 (共享L2缓存)
- **内存**: 16GB
- **网络**: 板卡 `192.168.3.63`, 本机 `192.168.3.25`

### 软件环境
- **内核**: Linux 6.6.63 + sched_ext + BTF
- **LLM框架**: llama.cpp
- **模型**: Llama-3.2-3B-Instruct-Q4_K_M.gguf (2GB)
- **模型路径**: `/mnt/llm/llama/models/`

### 测试参数

| 参数 | 值 | 说明 |
|------|-----|------|
| **测试时长** | 300秒 (5分钟) | 长时间测试，确保稳定性 |
| **计算线程** | 8 | 匹配CPU核心数 |
| **并行槽位** | 4 | 同时处理4个对话 |
| **上下文长度** | 4096 | 每个对话的上下文窗口 |
| **每请求token数** | 100 | 每次生成的目标token数 |
| **任务间隔** | 0.5秒 | 避免过载，模拟真实场景 |

## 测试任务

测试包含4个不同类型的任务，覆盖不同的计算模式：

### Task 1: 技术解释（计算密集型）
```
Prompt: "Explain the RISC-V Vector Extension (RVV) in detail, 
         including its purpose, key features, and how it differs 
         from other SIMD instruction sets."
```
- **特点**: 需要技术知识检索，中等长度输出
- **测试目标**: 知识密集型推理性能

### Task 2: 代码生成（逻辑密集型）
```
Prompt: "Write a complete Python implementation of a binary search tree 
         with insert, delete, search, and traverse methods. Include 
         comprehensive docstrings."
```
- **特点**: 结构化输出，强逻辑推理
- **测试目标**: 代码生成与结构化输出能力

### Task 3: 数学计算（推理密集型）
```
Prompt: "Solve this step by step: If a car travels at 60 km/h for 2.5 hours, 
         then at 80 km/h for 1.5 hours, what is the average speed for 
         the entire journey?"
```
- **特点**: 多步骤数学推理
- **测试目标**: 逐步推理与计算能力

### Task 4: 创意写作（生成密集型）
```
Prompt: "Write a short story about an AI researcher who discovers that 
         their neural network has become self-aware. Make it suspenseful 
         and thought-provoking."
```
- **特点**: 长文本输出，创意生成
- **测试目标**: 连续生成与创造性输出能力

## 关键性能指标 (KPI)

### 1. 首字延迟 (Time To First Token, TTFT)
- **定义**: 从发送请求到收到第一个token的时间
- **单位**: 毫秒 (ms)
- **测量方法**: 从llama.cpp的 `timings.prompt_ms` 获取
- **重要性**: 反映用户感知的响应速度

### 2. 每任务Token生成速率 (tok/s)
- **定义**: 每秒生成的token数量
- **单位**: tokens/second
- **测量方法**: 每个请求独立计算 `tokens / (generation_ms / 1000)`
- **报告指标**:
  - 平均 tok/s
  - 最小 tok/s
  - 最大 tok/s
- **重要性**: 反映实际推理吞吐量

### 3. 整体吞吐量
- **定义**: 5分钟内所有任务的总token数 / 总时间
- **单位**: tokens/second
- **测量方法**: `总tokens / 300秒`
- **重要性**: 反映系统整体性能

### 4. 请求完成数
- **定义**: 5分钟内每个任务完成的请求数
- **单位**: 次数
- **重要性**: 反映并发处理能力

### 5. 系统资源 (可选)
- CPU使用率 (每核心)
- 内存使用情况
- 上下文切换次数
- L2缓存命中率

## 数据记录格式

### CSV格式 (task_N.csv)
```csv
timestamp,request_num,tokens,generation_ms,total_ms,toks_per_sec
1720447890.123,1,100,3250.5,3500.2,30.77
1720447893.456,2,98,3180.2,3420.1,30.82
1720447896.789,3,102,3350.8,3600.5,30.43
```

字段说明：
- `timestamp`: Unix时间戳（秒，带小数）
- `request_num`: 该任务的第几个请求
- `tokens`: 本次生成的token数
- `generation_ms`: 生成时间（毫秒）
- `total_ms`: 总请求时间（毫秒，包括网络）
- `toks_per_sec`: 本次请求的tok/s

### Summary格式 (task_N.summary)
```
Task ID: 1
Prompt: Explain the RISC-V Vector Extension...
Duration: 300.5s
Total Requests: 92
Total Tokens: 9200
Average tok/s: 30.62
First Token Time: 245ms
```

## 使用方法

### 1. 基线测试（CFS默认调度器）
```bash
cd /home/gwx/Workdir/riscv-rvv/benchmark
./benchmark.sh
```

### 2. 对比测试（使用sched_ext调度器）
```bash
# 先运行基线
./benchmark.sh
mv results/latest results/baseline_cfs

# SSH到板卡，加载调度器
ssh root@192.168.3.63
cd /path/to/scheduler
sudo ./scx_scheduler &

# 返回本机，再次测试
exit
./benchmark.sh
mv results/latest results/with_scx_scheduler

# 对比结果
./compare.sh results/baseline_cfs results/with_scx_scheduler
```

### 3. 查看结果
```bash
# 文本报告
cat results/latest/report.txt

# JSON报告（用于自动化分析）
cat results/latest/report.json

# 单个任务详情
cat results/latest/task_1.summary
cat results/latest/task_1.csv
```

## 测试脚本说明

### config.sh
配置文件，包含所有测试参数。修改此文件以调整测试配置。

### benchmark.sh
主测试脚本，执行以下步骤：
1. 检查依赖（sshpass, curl, jq, bc）
2. SSH连接到板卡
3. 启动llama-server（端口9090）
4. 并发启动4个测试任务
5. 运行5分钟
6. 停止服务器
7. 调用Python生成报告

### generate_report.py
报告生成器，从CSV和Summary文件生成：
- `report.txt`: 人类可读的文本报告
- `report.json`: 机器可读的JSON报告

### compare.sh
对比工具，比较两次测试结果的差异。

## 预期结果范围

基于之前的测试数据，正常的性能范围：

| 指标 | CFS基线 | 优化目标 |
|------|---------|----------|
| **TTFT** | 200-300ms | <200ms |
| **平均tok/s** | 28-32 tok/s | >32 tok/s |
| **5分钟总tokens** | 35,000-40,000 | >40,000 |
| **每任务请求数** | 85-95次 | >95次 |

## 故障排除

### 问题1: SSH连接失败
```bash
# 检查网络
ping 192.168.3.63

# 检查SSH
sshpass -p bianbu ssh root@192.168.3.63 "echo test"
```

### 问题2: llama-server启动失败
```bash
# 手动检查
ssh root@192.168.3.63
cd /mnt/llm/llama
./run-server.sh

# 查看日志
tail -f /tmp/llama-server.log
```

### 问题3: 测试中途失败
- 检查网络稳定性
- 检查板卡内存是否充足
- 查看benchmark日志：`tail -f logs/benchmark_*.log`

### 问题4: 性能异常低
- 检查是否有其他进程在运行
- 检查CPU频率是否正常
- 检查是否有热节流

## 注意事项

⚠️ **测试环境一致性**
- 每次测试前确保板卡只运行测试相关进程
- 避免同时运行其他负载
- 确保网络连接稳定

⚠️ **数据有效性**
- 测试完成后检查日志是否有错误
- 确认4个任务都有数据输出
- 对比多次测试结果的一致性

⚠️ **资源消耗**
- 5分钟测试会消耗约40,000个tokens
- 确保模型已预加载到内存
- 监控内存使用，避免OOM

## 扩展性

### 添加新的测试任务
1. 编辑 `config.sh`，添加 `PROMPT_5`
2. 编辑 `benchmark.sh`，在 `run_benchmark()` 中添加第5个任务
3. 更新 `generate_report.py`，将任务数改为5

### 修改测试时长
```bash
# 编辑 config.sh
export TEST_DURATION=600  # 10分钟
```

### 集成perf监控
```bash
# 在benchmark.sh中添加
ssh_exec "perf stat -e context-switches,cache-misses -p \$(pgrep llama-server) &"
```

## 版本历史

- **v1.0** (2026-07-08): 初始版本
  - 5分钟测试
  - 4个并发任务
  - TTFT和tok/s记录
  - CSV + Summary + Report输出

## 许可证

GPL-2.0
