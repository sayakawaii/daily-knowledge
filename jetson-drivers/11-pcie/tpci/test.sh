#!/bin/bash
# 第 11 章测试（PCIe 驱动，用板载网卡芯片 RTL8111H，不需要接任何东西）
# 测试期间以太网口不可用（网卡从 r8168 借给 tpci），退出时自动还回去。
# 请通过 Wi-Fi 或 USB 登录板子再跑；如果你是通过网线 ssh 进来的，跑到一半会断线。
# 运行：cd 11-pcie/tpci && make && ./test.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }

# 脚本自己用另一种方法（sysfs）找设备，用来核对你的答案和做清理
ME=$(for d in /sys/bus/pci/devices/*; do
	[ "$(cat $d/vendor)" = 0x10ec ] && [ "$(cat $d/device)" = 0x8168 ] && basename $d; done | head -1)
[ -n "$ME" ] || { echo "没找到 RTL8168 网卡"; exit 1; }
S=/sys/bus/pci/devices/$ME
NET=$(ls $S/net 2>/dev/null | head -1)
MAC_OS=$(cat /sys/class/net/$NET/address 2>/dev/null)
if [ -n "$NET" ] && ip route get 1.1.1.1 2>/dev/null | grep -q "dev $NET "; then
	echo "默认路由走的是 $NET（网线），跑这个测试会断网。换 Wi-Fi 或 USB 登录再试。"; exit 1
fi

restore() {
	[ -e $S/driver ] && [ "$(basename "$(readlink $S/driver)")" = tutorial-pci ] && echo $ME | sudo tee $S/driver/unbind > /dev/null
	echo | sudo tee $S/driver_override > /dev/null
	sudo rmmod tpci 2>/dev/null
	[ -e $S/driver ] || echo $ME | sudo tee /sys/bus/pci/drivers/r8168/bind > /dev/null
}
trap restore EXIT

# ---- 找设备、读配置空间 ----
# PCI 设备的地址叫 BDF：域:总线:设备.功能，比如 0008:01:00.0。Jetson 有好几个 PCIe 控制器，所以域号不是 0。
#
# TODO 1：用 lspci 找出厂商 10ec、设备 8168 的网卡的完整地址（带域号），存到 BDF。
#   提示：lspci 的 -D 显示域号，-d 厂商:设备 只列这个型号；第一列就是地址。
BDF=
check "lspci 找到网卡 $BDF" "[ '$BDF' = '$ME' ]"
# TODO 2：用 setpci 从配置空间读出厂商 ID，存到 VID（4 位十六进制，不带 0x）。
#   提示：配置空间偏移 0 处的 16 位就是厂商 ID；setpci -s <地址> <寄存器名或偏移>.w（要 sudo）。
VID=
check "配置空间里的厂商 ID 是 10ec（Realtek）" "[ '$VID' = 10ec ]"
check "类别码 0x020000：网络控制器 / 以太网" "[ \"\$(cat $S/class)\" = 0x020000 ]"

# ---- 从 r8168 借过来 ----
sudo rmmod tpci 2>/dev/null
sudo insmod tpci.ko || exit 1
[ -e $S/driver ] && echo $ME | sudo tee $S/driver/unbind > /dev/null
# 这时网卡没有驱动。tpci 的 id_table 里有 10ec:8168，但内核不会自动重新匹配一个已经解绑的设备。
# TODO 3：指定这个设备只能由 tutorial-pci 驱动，然后让内核重新给它找驱动。
#   提示：往设备目录下的 driver_override 写驱动名，再往 /sys/bus/pci/drivers_probe 写设备地址。

sleep 0.5
check "设备绑定到 tutorial-pci" "[ \"\$(basename \"\$(readlink $S/driver)\" 2>/dev/null)\" = tutorial-pci ]"
check "BAR 和 sysfs 的 resource 文件一致" "[ \"\$(cut -d' ' -f2 $S/bar 2>/dev/null)\" = \"\$(sed -n \"\$((\$(cut -d' ' -f1 $S/bar 2>/dev/null)+1))p\" $S/resource | cut -d' ' -f1)\" ]"
check "从寄存器读出的 MAC 就是系统里网卡的 MAC（$MAC_OS）" "[ -n '$MAC_OS' ] && [ \"\$(cat $S/mac 2>/dev/null)\" = '$MAC_OS' ]"
check "芯片型号 XID 读出来了（$(cat $S/chip_xid 2>/dev/null)）" "! grep -qE '^0x(000|fcf)$' $S/chip_xid 2>/dev/null && [ -r $S/chip_xid ]"
check "PHY ID 是 Realtek 的（001c:）" "grep -q '^001c:' $S/phy_id 2>/dev/null"
check "链路状态能读（$(cat $S/link_state 2>/dev/null)，没插网线就是 down）" "grep -qE '^(up|down)$' $S/link_state 2>/dev/null"
check "中断类型 INTx" "[ \"\$(cat $S/irq_type 2>/dev/null)\" = INTx ]"

# ---- 中断 ----
IRQ_BEFORE=$(awk '/tutorial-pci/ { s = 0; for (i = 2; i <= NF; i++) if ($i ~ /^[0-9]+$/) s += $i; print s }' /proc/interrupts)
# TODO 4：让芯片产生 100 次中断。驱动的 sysfs 属性 trigger_irq 接受一个次数（要 sudo，用 tee 写）。

check "驱动数到 100 次中断" "[ \"\$(cat $S/irq_count 2>/dev/null)\" = 100 ]"
IRQ_AFTER=$(awk '/tutorial-pci/ { s = 0; for (i = 2; i <= NF; i++) if ($i ~ /^[0-9]+$/) s += $i; print s }' /proc/interrupts)
check "/proc/interrupts 里也多了 100 次" "[ \$((${IRQ_AFTER:-0} - ${IRQ_BEFORE:-0})) = 100 ]"
check "中断状态是定时器超时（0x4000）" "[ \"\$(cat $S/last_isr 2>/dev/null)\" = 0x4000 ]"

# ---- DMA ----
CNT=$(cat $S/counters 2>/dev/null)
echo "$CNT" | sed 's/^/      /'
check "芯片把计数器 DMA 写进了内存" "echo \"\$CNT\" | grep -q '^dma_written 1'"

restore; trap - EXIT
sleep 1
check "网卡还给了 r8168，网口 $NET 回来了" "[ \"\$(basename \"\$(readlink $S/driver)\")\" = r8168 ] && [ -e /sys/class/net/$NET ]"

echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
