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
THROUGHPUT1=$(awk -F': ' '/^TPS/ {print $2; exit} /总吞吐量/ {print $2; exit} /Overall Throughput/ {print $2; exit}' "$DIR1/report.txt" | awk '{print $1}')
THROUGHPUT2=$(awk -F': ' '/^TPS/ {print $2; exit} /总吞吐量/ {print $2; exit} /Overall Throughput/ {print $2; exit}' "$DIR2/report.txt" | awk '{print $1}')

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
    summary1="$DIR1/task${task}.summary"
    summary2="$DIR2/task${task}.summary"
    if [ ! -f "$summary1" ]; then
        summary1="$DIR1/task_${task}.summary"
    fi
    if [ ! -f "$summary2" ]; then
        summary2="$DIR2/task_${task}.summary"
    fi

    TTFT1=$(awk -F': ' '/First Token Time/ {print $2; exit} /首字/ {print $2; exit}' "$summary1" 2>/dev/null)
    TTFT2=$(awk -F': ' '/First Token Time/ {print $2; exit} /首字/ {print $2; exit}' "$summary2" 2>/dev/null)

    echo "  首字延迟:"
    echo "    测试1: $TTFT1"
    echo "    测试2: $TTFT2"

    # 提取平均tok/s
    TOKS1=$(awk -F': ' '/Average tok\/s/ {print $2; exit} /平均.*tok\/s/ {print $2; exit}' "$summary1" 2>/dev/null)
    TOKS2=$(awk -F': ' '/Average tok\/s/ {print $2; exit} /平均.*tok\/s/ {print $2; exit}' "$summary2" 2>/dev/null)

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
