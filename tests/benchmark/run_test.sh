#!/bin/bash
# 一键运行默认benchmark测试套件
# 默认按 2/4/8 并发各跑一轮，每轮300秒

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=========================================="
echo "RVV LLM 标准化性能测试"
echo "=========================================="
echo ""
echo "测试配置："
echo "  • 单轮窗口: 300秒"
echo "  • 并发档位: 2,4,8"
echo "  • 每档轮数: 1"
echo "  • 计算线程: 8"
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
echo "  cat results/latest/summary.txt"
echo ""
echo "对比测试："
echo "  ./compare.sh results/baseline results/latest"
echo ""
