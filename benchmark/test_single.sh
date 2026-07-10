#!/bin/bash
# 单并发性能测试 - 用于诊断性能问题

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/config.sh"

echo "=========================================="
echo "单并发性能诊断测试"
echo "=========================================="
echo ""
echo "配置："
echo "  IP: $REMOTE_IP"
echo "  线程: $LLM_THREADS"
echo "  并行槽位: $LLM_PARALLEL"
echo "  测试次数: 10次"
echo ""

# 测试10次请求
for i in {1..10}; do
    echo "[$i/10] 测试中..."

    response=$(curl -s "http://$REMOTE_IP:$REMOTE_PORT/completion" \
        -H "Content-Type: application/json" \
        -d "{
            \"prompt\": \"Count from 1 to 20:\",
            \"n_predict\": 30,
            \"temperature\": 0.7,
            \"stream\": false
        }")

    tokens=$(echo "$response" | jq -r '.tokens_predicted // 0')
    gen_ms=$(echo "$response" | jq -r '.timings.predicted_ms // 0')
    prompt_ms=$(echo "$response" | jq -r '.timings.prompt_ms // 0')

    if [ "$gen_ms" != "0" ] && [ "$gen_ms" != "null" ]; then
        toks=$(echo "scale=2; $tokens / $gen_ms * 1000" | bc)
        echo "  TTFT: ${prompt_ms}ms, 生成: ${gen_ms}ms, tokens: $tokens, tok/s: $toks"
    else
        echo "  错误: $response"
    fi

    sleep 1
done

echo ""
echo "测试完成"
