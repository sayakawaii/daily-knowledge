#!/bin/bash
# 用 configfs 建一个 USB 设备（gadget），功能是 Loopback：主机往 OUT 端点写什么，就从 IN 端点读回什么。
#   ./gadget.sh up [UDC]   默认插到虚拟控制器 dummy_udc.0；插真线时用 3550000.usb（板子的 USB-C 口）
#   ./gadget.sh down       拔掉并删除
# VID:PID 用 1209:0001，这是 pid.codes 给测试用的号，内核里没有别的驱动会抢它。
cd "$(dirname "$0")"
G=/sys/kernel/config/usb_gadget/tloop
w() { echo "$2" | sudo tee "$1" > /dev/null; }

case "$1" in
up)
	UDC=${2:-dummy_udc.0}
	sudo modprobe libcomposite
	if [ "$UDC" = dummy_udc.0 ] && ! lsmod | grep -qw dummy_hcd; then
		sudo insmod ./dummy_hcd.ko || exit 1
	fi
	lsmod | grep -qw usb_f_ss_lb || sudo insmod ./usb_f_ss_lb.ko || exit 1
	[ -d $G ] && { echo "gadget 已存在，先 ./gadget.sh down"; exit 1; }

	# 一个 gadget = 设备描述符 + 字符串 + 若干配置；每个配置里链接若干功能（function）
	sudo mkdir -p $G/strings/0x409 $G/configs/c.1/strings/0x409 $G/functions/Loopback.0
	w $G/idVendor 0x1209
	w $G/idProduct 0x0001
	w $G/bcdUSB 0x0200
	w $G/strings/0x409/manufacturer "jetson-drivers"
	w $G/strings/0x409/product "tutorial loopback"
	w $G/strings/0x409/serialnumber "0001"
	w $G/configs/c.1/strings/0x409/configuration "loopback"
	w $G/configs/c.1/MaxPower 100
	sudo ln -s $G/functions/Loopback.0 $G/configs/c.1/
	# 写 UDC = 把这个设备「插」到某个设备控制器上，主机这时开始枚举
	w $G/UDC "$UDC"
	echo "gadget 已插到 $UDC"
	;;
down)
	[ -d $G ] || exit 0
	w $G/UDC ""
	sudo rm -f $G/configs/c.1/Loopback.0
	sudo rmdir $G/configs/c.1/strings/0x409 $G/configs/c.1 $G/functions/Loopback.0 $G/strings/0x409 $G
	sudo rmmod usb_f_ss_lb 2>/dev/null
	lsmod | grep -qw dummy_hcd && sudo rmmod dummy_hcd
	echo "gadget 已删除"
	;;
*)
	echo "用法：$0 up [UDC] | down"; exit 1 ;;
esac
