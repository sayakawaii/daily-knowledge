#!/bin/bash
# 第 7 章测试（serdev 驱动）：ttyTHS2 + UART 内部回环，不需要接线
# 前提：第 2 章的覆盖层已装好（ttyTHS2 下有 tutorial-serdev 节点）
# 运行：cd 07-uart/tserdev && make && ./test.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }
DEV=/dev/tutorial_serdev
P=/sys/bus/serial/devices/serial0-0

check "覆盖层已生效（serdev 设备存在）" "[ -d $P ]"
check "ttyTHS2 已经不是普通串口了" "[ ! -e /dev/ttyTHS2 ]"
sudo rmmod tserdev 2>/dev/null
sudo dmesg -C
sudo insmod tserdev.ko
check "驱动加载，内部回环默认打开" "sudo dmesg | grep -q 'loopback on'"
check "$DEV 出现" "[ -c $DEV ]"

# 1. 发出去、收回来
# TODO 1：往 $DEV 写 hello，再从 $DEV 读 6 个字节存到 R（用 timeout 1 防止读不到时卡住）。
#   驱动收到的数据先放在 kfifo 里，所以先写后读没问题。
#   提示：head -c 字节数 文件；$( ) 会去掉末尾换行，所以 R 应该是 hello

check "回环收到 hello" "[ '$R' = hello ]"
check "tx_bytes = 6" "[ \"\$(cat $P/tx_bytes)\" = 6 ]"
check "rx_bytes = 6" "[ \"\$(cat $P/rx_bytes)\" = 6 ]"

# 2. 非阻塞读
# 没有数据时，O_NONBLOCK 的 read 应该立刻返回 -EAGAIN，而不是卡住
check "空的时候非阻塞读返回 EAGAIN" "timeout 1 dd if=$DEV iflag=nonblock bs=1 count=1 2>&1 | grep -q 'Resource temporarily unavailable'"

# 3. 改波特率
# TODO 2：通过 sysfs 把波特率改成 921600。

check "波特率是 921600" "[ \"\$(cat $P/baudrate)\" = 921600 ]"
echo world > $DEV
check "921600 下回环正常" "[ \"\$(timeout 1 head -c 6 $DEV)\" = world ]"

# 4. 关掉回环
# TODO 3：通过 sysfs 关掉内部回环。
#   关掉后 TX 不再接到 RX；ttyTHS2 的引脚没有引出来，发出去的数据就没了。

echo lost > $DEV
# RX 引脚悬空，关回环的瞬间可能收到一两个噪声字节，所以只检查有没有收到 lost
check "关掉回环后收不到自己发的数据" "[ \"\$(cat $P/loopback 2>/dev/null)\" = 0 ] && ! timeout 0.5 cat $DEV | grep -q lost"

sudo rmmod tserdev
check "卸载后 $DEV 消失" "[ ! -e $DEV ]"
echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
