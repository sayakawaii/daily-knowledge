#!/bin/bash
# 第 8 章测试（真 CAN 控制器 mttcan，不需要硬件）：用控制器的内部回环模式
# 内部回环：控制器把发出的帧在芯片内部直接送回自己的接收端，不经过收发器和总线，所以不用接 CAN 收发器。
# 需要先补完 can_echo.c。运行：./test_mttcan.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }

restore() { sudo ip link set can0 down; sudo ip link set can0 type can loopback off; }
trap restore EXIT
sudo modprobe can_raw
sudo ip link set can0 down

# TODO 1：配置 can0：波特率 500 kbit/s，打开内部回环（loopback on），然后 up。
#   CAN 网卡必须先设波特率才能 up。两条命令：
#   sudo ip link set can0 type can bitrate 波特率 loopback on
#   sudo ip link set can0 up

check "can0 已打开" "ip link show can0 | grep -q ',UP'"
check "波特率 500000" "ip -details link show can0 | grep -q 'bitrate 500000'"
check "控制器处于回环模式" "ip -details link show can0 | grep -q 'LOOPBACK'"
check "控制器状态 ERROR-ACTIVE（正常）" "ip -details link show can0 | grep -q 'ERROR-ACTIVE'"

tx0=$(cat /sys/class/net/can0/statistics/tx_packets)
out=$(./can_echo can0 can0 123 2>&1); rc=$?
echo "      $out"
check "mttcan 内部回环收到 0x123" "[ $rc -eq 0 ]"
check "can0 的 tx_packets 加了 1（帧真的经过了控制器）" "[ \$(cat /sys/class/net/can0/statistics/tx_packets) -eq $((tx0 + 1)) ]"

echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
