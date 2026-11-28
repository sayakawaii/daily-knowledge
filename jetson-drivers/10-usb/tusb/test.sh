#!/bin/bash
# 第 10 章测试（USB 主机侧驱动，用 dummy_hcd 虚拟控制器，不需要硬件）
# 同一块板子上：gadget（设备侧）插在虚拟设备控制器上，出现在虚拟主机总线上，再由 tusb（主机侧）驱动。
# 运行：cd 10-usb/tusb && make && ./test.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }
SIM=../../tools/usb-sim
G=/sys/kernel/config/usb_gadget/tloop

sudo rmmod tusb 2>/dev/null
$SIM/gadget.sh down > /dev/null 2>&1
$SIM/gadget.sh up || exit 1
sleep 1

# ---- 在 sysfs 里找到设备 ----
# 每个 USB 设备在 /sys/bus/usb/devices/ 下有一个目录，名字是「总线号-端口路径」，比如 3-1；
# 它的接口是「3-1:配置号.接口号」，比如 3-1:1.0。目录里有 idVendor、idProduct 等描述符字段。
#
# TODO 1：找出 idVendor 是 1209、idProduct 是 0001 的那个设备目录名，存到 DEV（形如 3-1）。
#   提示：for d in /sys/bus/usb/devices/*; do ...; done，比较 $d/idVendor 和 $d/idProduct 的内容。
DEV=
check "找到设备 1209:0001（$DEV）" "[ -n '$DEV' ] && [ \"\$(cat /sys/bus/usb/devices/$DEV/idProduct 2>/dev/null)\" = 0001 ]"
check "接口类是厂商自定义（ff），所以没有现成的驱动" "[ -n '$DEV' ] && [ \"\$(cat /sys/bus/usb/devices/$DEV:1.0/bInterfaceClass)\" = ff ] && [ ! -e /sys/bus/usb/devices/$DEV:1.0/driver ]"

sudo insmod tusb.ko
sleep 0.5
# 驱动绑定的设备会出现在 /sys/bus/usb/drivers/<驱动名>/ 下
I=$(ls -d /sys/bus/usb/drivers/tutorial-usb/*:1.0 2>/dev/null | head -1)
check "tusb 绑定到了接口 $(basename "$I")" "[ -n '$I' ]"
check "/dev/tusb0 出现" "[ -c /dev/tusb0 ]"
check "端点：bulk IN 0x81、bulk OUT 0x02" "[ \"\$(cat $I/ep_in 2>/dev/null)\" = 0x81 ] && [ \"\$(cat $I/ep_out 2>/dev/null)\" = 0x02 ]"
check "速度是 high-speed（USB 2.0，480 Mbit/s）" "[ \"\$(cat $I/speed 2>/dev/null)\" = high-speed ]"
# TODO 2：从端点描述符里读出 bulk IN 端点的最大包长，存到 MPS。
#   提示：接口目录下的 ep_81/ 就是端点 0x81，里面的 wMaxPacketSize 是十六进制（没有 0x 前缀）。
MPS=
check "驱动读到的包长和描述符一致（512）" "[ -n '$MPS' ] && [ \$(($MPS)) = \"\$(cat $I/max_packet 2>/dev/null)\" ]"

# ---- 回环 ----
OUT=$(./usb_loop /dev/tusb0 2000 2>&1)
echo "$OUT" | sed 's/^/      /'
check "7 种长度回环全部正确" "[ \$(echo \"\$OUT\" | grep -c '^OK') = 7 ]"
check "长度正好是包长整数倍（512、2048）也不卡住：靠零长度包" "echo \"\$OUT\" | grep -q '^OK 512\$' && echo \"\$OUT\" | grep -q '^OK 2048\$'"
check "tx_bytes = rx_bytes" "[ \"\$(cat $I/tx_bytes 2>/dev/null)\" = \"\$(cat $I/rx_bytes 2>/dev/null)\" ] && [ \"\$(cat $I/tx_bytes 2>/dev/null)\" -gt 0 ] 2>/dev/null"

# ---- 热拔插 ----
# 先把 /dev/tusb0 打开着（文件描述符 3），再「拔掉」设备，驱动不能崩，还开着的文件要返回错误
exec 3<> /dev/tusb0 2>/dev/null
# TODO 3：模拟拔线：让 gadget 从设备控制器上断开。
#   提示：插上是往 $G/UDC 写控制器名（看 tools/usb-sim/gadget.sh），拔掉就是写一个空字符串。

sleep 1
check "拔掉后 /dev/tusb0 消失" "[ ! -e /dev/tusb0 ]"
check "还开着的文件，写入返回错误" "! { echo x >&3; } 2>/dev/null"
exec 3>&-
check "驱动正常处理了断开（日志里有 disconnected）" "sudo dmesg | tail -20 | grep -q 'tusb0 disconnected'"
echo dummy_udc.0 | sudo tee $G/UDC > /dev/null
sleep 1
check "重新插上，/dev/tusb0 又出现" "[ -c /dev/tusb0 ]"

sudo rmmod tusb
$SIM/gadget.sh down > /dev/null
echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
