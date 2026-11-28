#!/bin/bash
# 第 8 章测试（虚拟 CAN 网卡对，不需要硬件）：tvcan0 <-> tvcan1
# 先补完 can_echo.c 里的 2 处 TODO，再：cd 08-can/tvcan && make && ./test.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }
stat() { cat /sys/class/net/$1/statistics/$2 2>/dev/null || echo -1; }

sudo rmmod tvcan 2>/dev/null
sudo modprobe can_dev                 # tvcan 用到 can_dev 模块里的函数；insmod 不会自动加载依赖
sudo modprobe can_raw                 # CAN_RAW socket 协议
sudo insmod tvcan.ko
check "两个网卡都出现了" "ip link show tvcan0 > /dev/null 2>&1 && ip link show tvcan1 > /dev/null 2>&1"
check "网卡类型是 CAN" "ip link show tvcan0 | grep -q 'link/can'"

# 1. 打开网卡
# TODO 1：把 tvcan0 和 tvcan1 都打开（up）。网卡打开时内核调用驱动的 ndo_open。
#   命令：sudo ip link set 网卡名 up

check "tvcan0 已打开" "ip link show tvcan0 | grep -q ',UP'"

# 2. 收发
out=$(./can_echo tvcan0 tvcan1 123 2>&1); rc=$?
echo "      $out"
check "tvcan0 发 0x123，tvcan1 收到" "[ $rc -eq 0 ]"
check "反方向：tvcan1 发 0x7FF，tvcan0 收到" "./can_echo tvcan1 tvcan0 7ff > /dev/null 2>&1"
check "tvcan0 tx_packets = 1" "[ \$(stat tvcan0 tx_packets) = 1 ]"
check "tvcan1 rx_packets = 1" "[ \$(stat tvcan1 rx_packets) = 1 ]"
check "rx_bytes = 8（一帧 8 字节数据）" "[ \$(stat tvcan1 rx_bytes) = 8 ]"

# 3. 接收过滤
check "过滤器只收 0x123 时，收不到 0x456" "./can_echo tvcan0 tvcan1 456 123 2>&1 | grep -q 'no frame'"
check "过滤器只收 0x123 时，0x123 能收到" "./can_echo tvcan0 tvcan1 123 123 > /dev/null 2>&1"

# 4. 用 can-utils 看一眼
timeout 1 candump -n 1 tvcan1 > /tmp/candump.out 2>&1 &
sleep 0.2; cansend tvcan0 321#DEADBEEF; wait
check "candump 看到 cansend 发的 321#DEADBEEF" "grep -q '321.*DE AD BE EF' /tmp/candump.out"

# 5. 对端关闭
# TODO 2：关掉 tvcan1（down），然后从 tvcan0 发一帧（cansend tvcan0 111#00）。
#   驱动发现对端没打开，会丢掉这一帧并让 tx_dropped 加 1。

check "对端关闭时 tvcan0 的 tx_dropped = 1" "[ \$(stat tvcan0 tx_dropped) = 1 ]"

sudo rmmod tvcan
check "卸载后网卡消失" "! ip link show tvcan0 > /dev/null 2>&1"
echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
