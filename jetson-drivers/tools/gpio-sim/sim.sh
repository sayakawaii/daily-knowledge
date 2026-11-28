#!/bin/bash
# 用 configfs 创建 / 删除一个 8 根线、标签为 tutorial-sim 的模拟 GPIO 芯片。
#   ./sim.sh up     创建（需要先编译 gpio-sim-ext.ko），打印芯片名和 sysfs 目录
#   ./sim.sh down   删除并卸载模块
# 模拟器的每根线在 sysfs 下有一个 sim_gpioN 目录：
#   pull  写 pull-up / pull-down，相当于外部把这根线拉高 / 拉低（输入线会产生中断边沿）
#   value 读：驱动输出的电平（输出线）或当前电平
cd "$(dirname "$0")"
C=/sys/kernel/config/gpio-sim/tutorial

case "$1" in
up)
	lsmod | grep -q '^gpio_sim_ext ' || sudo insmod gpio-sim-ext.ko || exit 1
	if [ ! -d $C ]; then
		sudo mkdir -p $C/bank0
		echo 8 | sudo tee $C/bank0/num_lines > /dev/null
		echo tutorial-sim | sudo tee $C/bank0/label > /dev/null
		echo 1 | sudo tee $C/live > /dev/null
	fi
	CHIP=$(cat $C/bank0/chip_name)
	echo "CHIP=$CHIP"
	echo "SIM=/sys/devices/platform/$(cat $C/dev_name)/$CHIP"
	;;
down)
	if [ -d $C ]; then
		echo 0 | sudo tee $C/live > /dev/null
		sudo rmdir $C/bank0 $C
	fi
	sudo rmmod gpio_sim_ext 2>/dev/null
	true
	;;
*)
	echo "用法：$0 up|down"; exit 1;;
esac
