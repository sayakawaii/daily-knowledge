#!/bin/bash
# 把内核自带的两个 USB 模拟用模块编译成外部模块（本板内核没开这两个选项）：
#   dummy_hcd.ko    CONFIG_USB_DUMMY_HCD：一对「虚拟 USB 主机控制器 + 虚拟设备控制器」，
#                   插在设备控制器上的 gadget，会出现在同一台机器的虚拟主机总线上
#   usb_f_ss_lb.ko  CONFIG_USB_CONFIGFS_F_LB_SS：gadget 功能 Loopback（收到什么发回什么）和 SourceSink
# 源码不用改，直接从第 9 章的 NVIDIA 内核源码里拿（和运行的内核完全一致），没有的话从 kernel.org 下载。
set -e
cd "$(dirname "$0")"
K=${KSRC:-$HOME/kernel/Linux_for_Tegra/source/kernel/kernel-noble}
KORG="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain"

for f in drivers/usb/gadget/udc/dummy_hcd.c drivers/usb/gadget/function/f_loopback.c \
	 drivers/usb/gadget/function/f_sourcesink.c drivers/usb/gadget/function/g_zero.h \
	 drivers/usb/gadget/u_f.h; do
	b=$(basename "$f")
	if [ -f "$K/$f" ]; then
		cp "$K/$f" "$b"
	elif [ ! -f "$b" ]; then
		echo "下载 $f"
		curl -sSfL --max-time 60 -o "$b" "$KORG/$f?h=v6.8.12"
	fi
done

make
echo
echo "编译完成：$(pwd)/dummy_hcd.ko $(pwd)/usb_f_ss_lb.ko"
echo "用法：./gadget.sh up    （建一个 Loopback 设备，插到虚拟控制器上）"
