#!/bin/bash
# 快速测试版本（30秒）

# 临时修改配置
export TEST_DURATION=30
export TOKENS_PER_REQUEST=16
export CONCURRENCY_LEVELS=2
export BENCHMARK_LOOPS=1

# 加载其他配置
source ./config.sh

# 覆盖测试时长
export TEST_DURATION=30
export TOKENS_PER_REQUEST=16
export CONCURRENCY_LEVELS=2
export BENCHMARK_LOOPS=1

echo "=========================================="
echo "快速测试模式（30秒）"
echo "=========================================="

# 运行benchmark
./benchmark.sh

echo ""
echo "=========================================="
echo "快速测试完成！"
echo "=========================================="
echo "查看结果: cat results/latest/summary.txt"
