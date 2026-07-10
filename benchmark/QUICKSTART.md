# 快速开始指南

## 1️⃣ 验证环境（1分钟）

```bash
cd /home/gwx/Workdir/riscv-rvv/benchmark

# 检查依赖
./benchmark.sh check

# 应该看到：
# ✓ sshpass
# ✓ curl  
# ✓ jq
# ✓ bc
# ✓ SSH连接正常
```

## 2️⃣ 快速测试（30秒）

```bash
# 运行30秒快速测试
./quick_test.sh

# 等待30秒后，查看结果
cat results/latest/report.txt
```

## 3️⃣ 完整测试（5分钟）

```bash
# 运行完整的5分钟基准测试
./benchmark.sh

# 测试完成后查看详细报告
cat results/latest/report.txt
less results/latest/task_1.csv
```

## 4️⃣ 查看结果

```bash
# 最新结果（软链接）
cat results/latest/report.txt

# 特定时间的结果
ls results/
cat results/20240708_153045/report.txt

# 所有任务概览
head results/latest/task_*.summary
```

## 5️⃣ 对比测试

### 基线测试
```bash
# 1. 运行基线（CFS）
./benchmark.sh
mv results/latest results/baseline_cfs

# 2. 启用你的调度器
# ssh root@192.168.3.67
# ... 加载 sched_ext 调度器 ...

# 3. 再次测试
./benchmark.sh
mv results/latest results/with_scheduler

# 4. 对比
echo "=== 基线 ==="
grep "Overall Throughput" results/baseline_cfs/report.txt

echo "=== 优化后 ==="
grep "Overall Throughput" results/with_scheduler/report.txt
```

## 6️⃣ 常见问题

### Q: 测试失败怎么办？
```bash
# 检查远程服务器
ssh root@192.168.3.67 "ps aux | grep llama"

# 查看服务器日志
ssh root@192.168.3.67 "tail -50 /tmp/llama-server.log"

# 手动重启服务器
ssh root@192.168.3.67 "pkill llama-server; cd /mnt/llm/llama && ./run-server.sh"
```

### Q: 如何修改测试时长？
```bash
# 编辑 config.sh
nano config.sh
# 修改: export TEST_DURATION=600  # 10分钟
```

### Q: 如何只测试单个任务？
```bash
# 编辑 benchmark.sh
# 注释掉其他 run_single_task 行
```

## 7️⃣ 典型工作流

```bash
# 每日测试流程
cd /home/gwx/Workdir/riscv-rvv/benchmark

# 1. 晨间基线测试
./benchmark.sh
mv results/latest results/morning_baseline

# 2. 测试你的优化
# ... 应用优化 ...
./benchmark.sh
mv results/latest results/optimization_v1

# 3. 对比
grep -H "Overall Throughput" results/*/report.txt

# 4. 如果好，保存
cp -r results/optimization_v1 ../saved_results/best_$(date +%Y%m%d)
```

## 8️⃣ 下一步

- 阅读 `README.md` 了解详细功能
- 查看 `config.sh` 了解所有配置选项
- 编辑 prompts 测试不同负载
- 集成到你的实验流程

## 🎯 期望结果

### 基线性能（CFS，8核）
```
Overall Throughput: ~10-13 tok/s  (4并发)
Per-task tok/s:     ~2.5-3.3 tok/s
First Token Time:   ~200-300ms
```

### 优化目标
- 提升整体吞吐量 5-10%
- 减少首字延迟 10-20%
- 降低tok/s方差（更稳定）

