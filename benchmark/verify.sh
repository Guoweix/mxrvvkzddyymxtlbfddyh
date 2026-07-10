#!/bin/bash
# 快速验证测试脚本
# 用于在板卡离线时验证脚本逻辑

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=========================================="
echo "测试脚本验证"
echo "=========================================="
echo ""

# 1. 检查所有文件存在
echo "[1/6] 检查文件完整性..."
files=(
    "$SCRIPT_DIR/config.sh"
    "$SCRIPT_DIR/benchmark.sh"
    "$SCRIPT_DIR/generate_report.py"
    "$SCRIPT_DIR/compare.sh"
    "$SCRIPT_DIR/README.md"
)

for file in "${files[@]}"; do
    if [ -f "$file" ]; then
        echo "  ✓ $(basename $file)"
    else
        echo "  ✗ $(basename $file) - 缺失"
        exit 1
    fi
done

# 2. 检查依赖
echo ""
echo "[2/6] 检查系统依赖..."
deps=(sshpass curl jq bc python3)
missing=0
for dep in "${deps[@]}"; do
    if command -v $dep &> /dev/null; then
        echo "  ✓ $dep"
    else
        echo "  ✗ $dep - 未安装"
        missing=1
    fi
done

if [ $missing -eq 1 ]; then
    echo ""
    echo "请安装缺失的依赖："
    echo "  sudo pacman -S sshpass curl jq bc python"
    exit 1
fi

# 3. 检查配置
echo ""
echo "[3/6] 检查配置..."
source "$SCRIPT_DIR/config.sh"
echo "  IP地址: $REMOTE_IP"
echo "  计算线程: $LLM_THREADS"
echo "  并行槽位: $LLM_PARALLEL"
echo "  测试时长: ${TEST_DURATION}秒"
echo "  并发任务: $CONCURRENT_TASKS"

# 4. 检查脚本可执行
echo ""
echo "[4/6] 检查脚本权限..."
if [ -x "$SCRIPT_DIR/benchmark.sh" ]; then
    echo "  ✓ benchmark.sh 可执行"
else
    echo "  ✗ benchmark.sh 不可执行"
    chmod +x "$SCRIPT_DIR/benchmark.sh"
    echo "  已修复"
fi

if [ -x "$SCRIPT_DIR/compare.sh" ]; then
    echo "  ✓ compare.sh 可执行"
else
    echo "  ✗ compare.sh 不可执行"
    chmod +x "$SCRIPT_DIR/compare.sh"
    echo "  已修复"
fi

# 5. 验证Python脚本语法
echo ""
echo "[5/6] 验证Python脚本..."
python3 -m py_compile "$SCRIPT_DIR/generate_report.py"
if [ $? -eq 0 ]; then
    echo "  ✓ generate_report.py 语法正确"
else
    echo "  ✗ generate_report.py 有语法错误"
    exit 1
fi

# 6. 测试网络连接
echo ""
echo "[6/6] 测试网络连接..."
if ping -c 1 -W 2 $REMOTE_IP &> /dev/null; then
    echo "  ✓ 板卡在线 ($REMOTE_IP)"

    # 测试SSH
    if sshpass -p "$REMOTE_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 \
       "$REMOTE_USER@$REMOTE_IP" "echo '测试成功'" 2>/dev/null | grep -q "测试成功"; then
        echo "  ✓ SSH连接正常"

        echo ""
        echo "=========================================="
        echo "✅ 所有检查通过！"
        echo "=========================================="
        echo ""
        echo "可以运行测试："
        echo "  cd $SCRIPT_DIR"
        echo "  ./benchmark.sh"
        echo ""
    else
        echo "  ✗ SSH连接失败"
        echo ""
        echo "请检查："
        echo "  1. SSH密码是否正确 (config.sh中的REMOTE_PASS)"
        echo "  2. 板卡SSH服务是否运行"
        echo "  3. 防火墙设置"
    fi
else
    echo "  ⚠ 板卡离线 ($REMOTE_IP)"
    echo ""
    echo "=========================================="
    echo "⚠️  板卡当前不可达"
    echo "=========================================="
    echo ""
    echo "脚本验证通过，但需要等待板卡上线才能运行测试。"
    echo ""
    echo "请检查："
    echo "  1. 板卡是否通电"
    echo "  2. 网络连接是否正常"
    echo "  3. IP地址是否正确 ($REMOTE_IP)"
    echo ""
fi
