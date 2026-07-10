# RVV LLM Benchmark Framework

## 概述

这是一个完整的LLM推理基准测试框架，专门用于测试RISC-V Vector Extension (RVV)环境下的性能。

## 功能特性

- ✅ 自动启动和管理远程llama-server
- ✅ 并发4个不同的对话任务
- ✅ 5分钟长时间测试
- ✅ 记录首字延迟（Time To First Token, TTFT）
- ✅ 记录每个任务的实时tok/s
- ✅ 监控系统资源（CPU、内存、线程）
- ✅ 生成详细的测试报告

## 文件结构

```
benchmark/
├── config.sh              # 配置文件
├── benchmark.sh           # 主测试脚本
├── generate_report.py     # 报告生成器
├── results/               # 测试结果目录
│   └── YYYYMMDD_HHMMSS/  # 每次测试的结果
│       ├── config.txt     # 测试配置
│       ├── task_*.csv     # 各任务详细数据
│       ├── task_*.summary # 各任务总结
│       ├── report.txt     # 文本报告
│       └── report.json    # JSON报告
└── logs/                  # 日志目录
```

## 快速开始

### 1. 检查配置

编辑 `config.sh` 确认配置：

```bash
export REMOTE_IP="192.168.3.67"      # 远程机器IP
export REMOTE_USER="root"             # SSH用户
export REMOTE_PASS="bianbu"           # SSH密码
export LLM_THREADS=8                  # 计算线程数
export LLM_PARALLEL=4                 # 并行槽位数
export TEST_DURATION=300              # 测试时长（秒）
export CONCURRENT_TASKS=4             # 并发任务数
```

### 2. 运行基准测试

```bash
cd /home/gwx/Workdir/riscv-rvv/benchmark
./benchmark.sh
```

测试过程：
1. 检查依赖（sshpass, curl, jq, bc）
2. 测试SSH连接
3. 启动llama-server
4. 并发运行4个任务
5. 持续5分钟
6. 生成测试报告

### 3. 查看结果

测试完成后，结果保存在 `results/YYYYMMDD_HHMMSS/` 目录：

```bash
# 查看文本报告
cat results/latest/report.txt

# 查看JSON报告（可用于自动化分析）
cat results/latest/report.json

# 查看单个任务的详细数据
cat results/latest/task_1.summary
```

## 测试任务说明

框架包含4个预定义的测试任务，涵盖不同的负载类型：

### Task 1: 技术解释（计算密集）
- Prompt: "Explain the RISC-V Vector Extension..."
- 特点: 需要技术知识，中等长度输出

### Task 2: 代码生成（逻辑密集）
- Prompt: "Write a Python binary search tree..."
- 特点: 结构化输出，逻辑推理

### Task 3: 数学计算（推理密集）
- Prompt: "Solve this step by step: average speed..."
- 特点: 数学推理，逐步求解

### Task 4: 创意写作（生成密集）
- Prompt: "Write a short story about AI..."
- 特点: 长输出，创意生成

## 输出指标

### 整体指标
- **Total Requests**: 总请求数
- **Total Tokens**: 总生成token数
- **Overall Throughput**: 整体吞吐量（tok/s）
- **Test Duration**: 实际测试时长

### 每个任务的指标
- **First Token Time (TTFT)**: 首字延迟（ms）
- **Average tok/s**: 平均token生成速率
- **Min/Max tok/s**: 最小/最大token生成速率
- **Total Requests**: 该任务的请求数
- **Total Tokens**: 该任务的token数
- **Average Generation Time**: 平均生成时间

### 系统指标
- **CPU Usage**: CPU使用率（每个核心）
- **Memory Usage**: 内存使用情况
- **Thread Count**: 线程数量
- **Context Switches**: 上下文切换（如果有perf工具）

## 使用场景

### 1. 基线测试
```bash
# 测试CFS默认调度器
./benchmark.sh

# 结果会保存在 results/baseline_cfs/
```

### 2. 对比测试
```bash
# 先运行基线
./benchmark.sh
mv results/latest results/baseline

# 启用你的sched_ext调度器
# ... 加载调度器 ...

# 再次运行测试
./benchmark.sh
mv results/latest results/with_scheduler

# 对比结果
diff results/baseline/report.txt results/with_scheduler/report.txt
```

### 3. 参数调优
```bash
# 测试不同线程数
for threads in 4 8 12 16; do
    sed -i "s/LLM_THREADS=.*/LLM_THREADS=$threads/" config.sh
    ./benchmark.sh
    mv results/latest "results/threads_$threads"
done
```

### 4. CPU绑定测试
```bash
# 在benchmark.sh开始前，在远程机器执行绑定
ssh_exec "taskset -cp 0-6 \$(pgrep llama-server)"

# 然后运行测试
./benchmark.sh
```

## 自定义测试

### 修改测试时长
```bash
# 编辑 config.sh
export TEST_DURATION=600  # 10分钟
```

### 修改并发数
```bash
# 编辑 config.sh
export CONCURRENT_TASKS=8  # 8个并发任务
```

### 添加自定义Prompt
```bash
# 编辑 config.sh，添加新的prompt
export PROMPT_5="Your custom prompt here..."

# 然后编辑 benchmark.sh，在 run_benchmark() 中添加：
run_single_task 5 "$PROMPT_5" "$result_dir/task_5" &
```

## 数据格式

### CSV格式 (task_*.csv)
```csv
timestamp,request_num,tokens,generation_ms,total_ms
1234567890.123,1,100,3250.5,3500.2
1234567890.456,2,98,3180.2,3420.1
```

### Summary格式 (task_*.summary)
```
Task ID: 1
Prompt: Explain the RISC-V Vector Extension...
Duration: 300.5s
Total Requests: 92
Total Tokens: 9200
Average tok/s: 3.06
First Token Time: 245ms
```

## 故障排除

### SSH连接失败
```bash
# 检查网络连通性
ping 192.168.3.67

# 测试SSH
sshpass -p bianbu ssh root@192.168.3.67 "echo test"
```

### LLM服务器启动失败
```bash
# 手动检查
ssh root@192.168.3.67
cd /mnt/llm/llama
./run-server.sh

# 查看日志
tail -f /tmp/llama-server.log
```

### 缺少依赖
```bash
# Ubuntu/Debian
sudo apt-get install sshpass curl jq bc python3

# Arch/Manjaro
sudo pacman -S sshpass curl jq bc python
```

## 高级用法

### 集成perf监控
```bash
# 在benchmark.sh的run_benchmark()中添加：
ssh_exec "perf stat -e context-switches,cache-misses -p \$(pgrep llama-server) -o /tmp/perf.log &"
```

### 导出数据到Excel
```bash
# CSV文件可以直接在Excel中打开
# 或使用Python pandas：
python3 << EOF
import pandas as pd
df = pd.read_csv('results/latest/task_1.csv')
df.to_excel('task_1.xlsx')
EOF
```

### 自动化CI集成
```bash
# 在CI pipeline中使用
./benchmark.sh
if [ $? -eq 0 ]; then
    python3 compare_results.py baseline.json results/latest/report.json
fi
```

## 注意事项

- ⚠️ 确保远程机器有足够内存（至少8GB）
- ⚠️ 测试期间不要运行其他重负载任务
- ⚠️ 首次运行可能需要下载模型
- ⚠️ 5分钟测试会消耗大量token
- ⚠️ 确保网络连接稳定

## 贡献

欢迎提交改进建议和bug报告！

## 许可

GPL-2.0
