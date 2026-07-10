#!/bin/bash
# Smoke Test - 快速性能测试
# 目的：验证llama-server基础性能（单并发，5次请求）

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/config.sh"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo "=========================================="
echo "🔥 Smoke Test - LLM性能快速检测"
echo "=========================================="
echo ""
echo "配置："
echo "  服务器: http://$REMOTE_IP:$REMOTE_PORT"
echo "  测试次数: 5"
echo "  每次tokens: 30"
echo ""

# 检查服务器
echo -n "检查服务器状态... "
if curl -s "http://$REMOTE_IP:$REMOTE_PORT/health" | grep -q "ok"; then
    echo -e "${GREEN}✓ 在线${NC}"
else
    echo -e "${RED}✗ 离线${NC}"
    exit 1
fi

echo ""
echo "开始测试..."
echo "----------------------------------------"

total_toks=0
total_time=0
ttft_sum=0
count=0

for i in {1..5}; do
    echo -n "[$i/5] "

    start=$(date +%s.%N)
    response=$(curl -s "http://$REMOTE_IP:$REMOTE_PORT/completion" \
        -H "Content-Type: application/json" \
        -d '{
            "prompt": "Count from 1 to 20:",
            "n_predict": 30,
            "temperature": 0.7,
            "stream": false
        }')
    end=$(date +%s.%N)

    # 解析响应
    tokens=$(echo "$response" | jq -r '.tokens_predicted // 0')
    gen_ms=$(echo "$response" | jq -r '.timings.predicted_ms // 0')
    prompt_ms=$(echo "$response" | jq -r '.timings.prompt_ms // 0')

    if [ "$gen_ms" != "0" ] && [ "$gen_ms" != "null" ]; then
        # 计算tok/s（使用Python避免bc精度问题）
        toks=$(python3 -c "print(f'{$tokens / $gen_ms * 1000:.2f}')")
        wall_time=$(python3 -c "print(f'{$end - $start:.2f}')")

        echo -e "${GREEN}✓${NC} TTFT: ${prompt_ms}ms, 生成: ${gen_ms}ms, tok/s: $toks, 总耗时: ${wall_time}s"

        total_toks=$(python3 -c "print($total_toks + $tokens)")
        total_time=$(python3 -c "print($total_time + $gen_ms)")
        ttft_sum=$(python3 -c "print($ttft_sum + $prompt_ms)")
        count=$((count + 1))
    else
        echo -e "${RED}✗ 失败${NC}"
        echo "Response: $response"
    fi

    # 短暂休息避免过载
    sleep 0.5
done

echo "----------------------------------------"
echo ""

if [ "$count" -gt 0 ]; then
    avg_toks=$(python3 -c "print(f'{$total_toks / $total_time * 1000:.2f}')")
    avg_ttft=$(python3 -c "print(f'{$ttft_sum / $count:.2f}')")

    echo "=========================================="
    echo "📊 测试结果汇总"
    echo "=========================================="
    echo "  成功请求: $count/5"
    echo "  总tokens: ${total_toks}"
    echo "  平均tok/s: ${avg_toks}"
    echo "  平均TTFT: ${avg_ttft}ms"
    echo ""

    # 性能评估
    toks_val=$(python3 -c "print(float('$avg_toks'))")

    if (( $(python3 -c "print(1 if $toks_val >= 20 else 0)") )); then
        echo -e "${GREEN}✓ 性能优秀${NC} (>= 20 tok/s)"
    elif (( $(python3 -c "print(1 if $toks_val >= 10 else 0)") )); then
        echo -e "${YELLOW}⚠ 性能一般${NC} (10-20 tok/s)"
    elif (( $(python3 -c "print(1 if $toks_val >= 5 else 0)") )); then
        echo -e "${YELLOW}⚠ 性能较差${NC} (5-10 tok/s)"
    else
        echo -e "${RED}✗ 性能异常低${NC} (< 5 tok/s)"
        echo "  建议检查："
        echo "  - llama-server配置（-np, -t参数）"
        echo "  - RVV编译选项（当前VLEN可能不正确）"
        echo "  - 系统负载"
    fi
    echo ""
else
    echo -e "${RED}✗ 所有请求失败${NC}"
    exit 1
fi
