#!/bin/bash
# 把内核自带的 GPIO 模拟器 gpio-sim 编译成外部模块 gpio-sim-ext.ko。
#
# 本板内核没开 CONFIG_GPIO_SIM，也没开它依赖的 CONFIG_IRQ_SIM，所以把两个源文件拿出来一起编译。
# irq_sim.c 里用到一个没导出的函数 irq_to_desc()，换成等价的、已导出的 generic_handle_irq()。
#
# 源文件来源（按顺序）：
#   1. 第 9 章下载的 NVIDIA 内核源码 ~/kernel/Linux_for_Tegra/source/kernel/kernel-noble（和运行的内核完全一致）
#   2. 没有的话，从 kernel.org 下载 v6.8.12 的这两个文件
set -e
cd "$(dirname "$0")"
K=${KSRC:-$HOME/kernel/Linux_for_Tegra/source/kernel/kernel-noble}
KORG="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain"

for f in drivers/gpio/gpio-sim.c kernel/irq/irq_sim.c; do
	b=$(basename "$f")
	if [ -f "$K/$f" ]; then
		cp "$K/$f" "$b"
		echo "使用本地源码 $K/$f"
	elif [ ! -f "$b" ]; then
		echo "下载 $f"
		curl -sSfL --max-time 60 -o "$b" "$KORG/$f?h=v6.8.12"
	fi
done

sed -i 's/handle_simple_irq(irq_to_desc(irqnum));/generic_handle_irq(irqnum);/' irq_sim.c
grep -q 'generic_handle_irq(irqnum);' irq_sim.c

make
echo
echo "编译完成：$(pwd)/gpio-sim-ext.ko"
echo "加载：sudo insmod $(pwd)/gpio-sim-ext.ko && ls /sys/kernel/config/gpio-sim/"
