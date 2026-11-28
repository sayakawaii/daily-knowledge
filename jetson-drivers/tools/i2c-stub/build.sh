#!/bin/bash
# 把内核自带的 I2C 模拟器 i2c-stub 编译成外部模块（本板内核没开 CONFIG_I2C_STUB）。
# i2c-stub 模拟一条 SMBus 总线，上面挂着指定地址的「芯片」，每个芯片有 256 个字节/字寄存器，
# 可以用 i2cset 预先写好寄存器值，让驱动去读。
#
# 加载：sudo insmod i2c-stub.ko chip_addr=0x48
#       i2cdetect -l | grep stub     → 看它是 i2c-几
set -e
cd "$(dirname "$0")"
K=${KSRC:-$HOME/kernel/Linux_for_Tegra/source/kernel/kernel-noble}
f=drivers/i2c/i2c-stub.c
if [ -f "$K/$f" ]; then
	cp "$K/$f" .
elif [ ! -f i2c-stub.c ]; then
	echo "下载 $f"
	curl -sSfL --max-time 60 -o i2c-stub.c \
		"https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/$f?h=v6.8.12"
fi
make
echo "编译完成：$(pwd)/i2c-stub.ko"
