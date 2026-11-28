#!/bin/bash
# 第 10 章真硬件测试：一根 USB 线，让板子自己当自己的 USB 设备。
#
# 接线：一根 USB-C 转 USB-A 的数据线（不能是只能充电的线）。
#   C 头插板子的 USB-C 口（就是刷机用的那个，设备控制器 3550000.usb）
#   A 头插板子自己的任意一个 USB-A 口（主机控制器 tegra-xusb）
# 和 test.sh 的区别：这次数据真的走了 USB PHY 和线缆，速度也由真实的控制器协商。
#
# USB-C 口平时被 NVIDIA 的 l4t gadget 占用（虚拟网卡 usb0、串口 ttyGS0 等），测试时临时借用，退出时恢复。
# 运行：cd 10-usb/tusb && make && ./test_cable.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }
SIM=../../tools/usb-sim
UDC=3550000.usb
L4T=/sys/kernel/config/usb_gadget/l4t

restore() {
	sudo rmmod tusb 2>/dev/null
	$SIM/gadget.sh down > /dev/null 2>&1
	if [ -d $L4T ] && [ -z "$(cat $L4T/UDC)" ]; then
		echo $UDC | sudo tee $L4T/UDC > /dev/null && echo "已恢复 l4t gadget"
	fi
}
trap restore EXIT

$SIM/gadget.sh down > /dev/null 2>&1
[ -d $L4T ] && echo "" | sudo tee $L4T/UDC > /dev/null
sudo insmod tusb.ko 2>/dev/null
$SIM/gadget.sh up $UDC || exit 1
sleep 3

I=$(ls -d /sys/bus/usb/drivers/tutorial-usb/*:1.0 2>/dev/null | head -1)
check "设备出现在真实的主机总线上，tusb 绑定了" "[ -n '$I' ]"
[ -n "$I" ] || { echo "没找到设备：检查线是不是数据线、两头是否插好；dmesg 看枚举日志"; exit 1; }
BUS=$(basename "$I" | cut -d- -f1)
check "总线 $BUS 的主机控制器是 tegra-xusb（不是 dummy_hcd）" "readlink -f /sys/bus/usb/devices/usb$BUS | grep -q 3610000.usb"
echo "      协商速度：$(cat $I/speed)，最大包长：$(cat $I/max_packet)"
OUT=$(./usb_loop /dev/tusb0 5000 2>&1)
echo "$OUT" | sed 's/^/      /'
check "7 种长度回环全部正确" "[ \$(echo \"\$OUT\" | grep -c '^OK') = 7 ]"

echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
