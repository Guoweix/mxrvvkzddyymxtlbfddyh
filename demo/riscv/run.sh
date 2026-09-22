#!/usr/bin/env bash
# RISC-V 功能验证：无需源码/编译，直接用 demo/riscv/bin 的二进制部署到板端。
# 前置：板端已挂载 bpffs（mount -t bpf bpf /sys/fs/bpf）。
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=config.sh
source "$HERE/config.sh"
BIN="$HERE/bin"
REMOTE="${REMOTE_USER}@${REMOTE_IP}"
D="/tmp/dt-demo-riscv"
OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8)
export SSHPASS="$REMOTE_PASS"

echo "== 部署 $BIN 到 $REMOTE:$D =="
sshpass -e ssh "${OPTS[@]}" "$REMOTE" "rm -rf $D && mkdir -p $D"
sshpass -e scp -O "${OPTS[@]}" \
    "$BIN/dt_controller" "$BIN/dt_persistent_probe" "$BIN/dt_phase_probe" \
    "$REMOTE:$D/"

echo "== 板端运行 controller + persistent 探针 =="
sshpass -e ssh "${OPTS[@]}" "$REMOTE" "
set -e
mountpoint -q /sys/fs/bpf || mount -t bpf bpf /sys/fs/bpf
cd $D
./dt_controller --socket $D/control.sock --mode dynamic --scx \
    --grant-lifetime persistent --allocation-policy equal \
    --events $D/events.csv >$D/controller.log 2>&1 &
CTL=\$!
sleep 2
echo \"state=\$(sed -n '2p' /sys/kernel/debug/sched/ext)\"
./dt_persistent_probe $D/control.sock 200 0 stable | tail -1
kill -INT \$CTL; sleep 1
echo \"after=\$(sed -n '2p' /sys/kernel/debug/sched/ext)\"
grep -E 'BPF enqueue|STOP' $D/controller.log
"
