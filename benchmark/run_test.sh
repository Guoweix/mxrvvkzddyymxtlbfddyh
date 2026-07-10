#!/bin/bash
# 一键运行完整的5分钟benchmark测试
# 自动启动llama-server，并发4个任务，记录所有指标

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=========================================="
echo "RVV LLM 标准化性能测试"
echo "=========================================="
echo ""
echo "测试配置："
echo "  • 时长: 5分钟"
echo "  • 计算线程: 8"
echo "  • 并行对话: 4"
echo "  • 测试任务: 4个不同类型"
echo ""
echo "开始测试..."
echo ""

# 运行主测试脚本
"$SCRIPT_DIR/benchmark.sh"

echo ""
echo "=========================================="
echo "测试完成！"
echo "=========================================="
echo ""
echo "查看结果："
echo "  cat results/latest/report.txt"
echo ""
echo "对比测试："
echo "  ./compare.sh results/baseline results/latest"
echo ""
