#!/usr/bin/env bash
# x86 功能验证：无需源码/编译，直接用 demo/x86/bin 的二进制。
# 1) 启动 dt_controller（dynamic + sched_ext + persistent）
# 2) 运行 dt_persistent_probe
# 3) 检查 wrong_cpu=0 与干净卸载
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BIN="$HERE/bin"
SOCK=/tmp/dt-demo-x86.sock
EV=/tmp/dt-demo-x86-events.csv
LOG=/tmp/dt-demo-x86-controller.log

[ "$(id -u)" = 0 ] || { echo "需要 root（sudo -n $0）"; exit 1; }
rm -f "$SOCK" "$EV"

"$BIN/dt_controller" --socket "$SOCK" --mode dynamic --scx \
    --grant-lifetime persistent --allocation-policy equal \
    --events "$EV" >"$LOG" 2>&1 &
CTL=$!
trap 'kill -INT "$CTL" 2>/dev/null || true' EXIT

for _ in $(seq 1 40); do
    [ "$(cat /sys/kernel/sched_ext/state 2>/dev/null)" = enabled ] && break
    sleep 0.25
done
[ "$(cat /sys/kernel/sched_ext/state)" = enabled ] || { echo "attach 失败"; cat "$LOG"; exit 1; }
echo "sched_ext state=enabled ops=$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)"

"$BIN/dt_persistent_probe" "$SOCK" 200 0 stable | tail -1

kill -INT "$CTL"; wait "$CTL" 2>/dev/null || true
grep -E "BPF enqueue|STOP" "$LOG" || true
[ "$(cat /sys/kernel/sched_ext/state)" = disabled ] && echo "detach 正常" || { echo "detach 失败"; exit 1; }
