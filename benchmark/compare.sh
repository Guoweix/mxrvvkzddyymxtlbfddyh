#!/bin/bash
# 对比两次测试结果

if [ $# -ne 2 ]; then
    echo "用法: $0 <结果目录1> <结果目录2>"
    echo "示例: $0 results/baseline_cfs results/optimized"
    exit 1
fi

DIR1="$1"
DIR2="$2"

if [ ! -d "$DIR1" ] || [ ! -d "$DIR2" ]; then
    echo "错误: 目录不存在"
    exit 1
fi

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "             测试结果对比"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "测试1: $DIR1"
echo "测试2: $DIR2"
echo ""

# 提取整体吞吐量
THROUGHPUT1=$(grep "Overall Throughput" "$DIR1/report.txt" | awk '{print $3}')
THROUGHPUT2=$(grep "Overall Throughput" "$DIR2/report.txt" | awk '{print $3}')

echo "整体吞吐量:"
echo "  测试1: $THROUGHPUT1 tok/s"
echo "  测试2: $THROUGHPUT2 tok/s"

if [ -n "$THROUGHPUT1" ] && [ -n "$THROUGHPUT2" ]; then
    DIFF=$(echo "scale=2; $THROUGHPUT2 - $THROUGHPUT1" | bc)
    PERCENT=$(echo "scale=2; ($THROUGHPUT2 - $THROUGHPUT1) / $THROUGHPUT1 * 100" | bc)
    echo "  差异:  $DIFF tok/s ($PERCENT%)"
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "             每任务对比"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

for task in 1 2 3 4; do
    echo ""
    echo "Task $task:"

    # 提取首字延迟
    TTFT1=$(grep "First Token Time" "$DIR1/task_$task.summary" | awk '{print $4}')
    TTFT2=$(grep "First Token Time" "$DIR2/task_$task.summary" | awk '{print $4}')

    echo "  首字延迟:"
    echo "    测试1: $TTFT1"
    echo "    测试2: $TTFT2"

    # 提取平均tok/s
    TOKS1=$(grep "Average tok/s" "$DIR1/task_$task.summary" | awk '{print $4}')
    TOKS2=$(grep "Average tok/s" "$DIR2/task_$task.summary" | awk '{print $4}')

    echo "  平均tok/s:"
    echo "    测试1: $TOKS1"
    echo "    测试2: $TOKS2"

    if [ -n "$TOKS1" ] && [ -n "$TOKS2" ]; then
        DIFF=$(echo "scale=2; $TOKS2 - $TOKS1" | bc)
        PERCENT=$(echo "scale=2; ($TOKS2 - $TOKS1) / $TOKS1 * 100" | bc)
        echo "    差异:  $DIFF tok/s ($PERCENT%)"
    fi
done

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "详细报告:"
echo "  测试1: cat $DIR1/report.txt"
echo "  测试2: cat $DIR2/report.txt"
