#!/bin/bash
# 第 3 步（装之前必做）：确认新内核能直接使用板子上已有的模块。
#
# 加载模块时，内核会检查两样东西：
#   1. vermagic：版本号 + SMP/PREEMPT 等关键选项，必须完全一致；
#   2. 符号校验和（CONFIG_MODVERSIONS）：模块用到的每个内核函数，编译时都记了一个 CRC，
#      这个 CRC 由函数原型和所有相关结构体的定义算出来。结构体多一个字段，CRC 就变了，模块就拒绝加载。
# vmlinux.symvers 是新内核导出的全部符号和 CRC，和板子上旧内核的 Module.symvers 逐个比对即可。
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
source "$HERE/env.sh"
[ -f "$B/vmlinux.symvers" ] || { echo "先运行 ./build.sh"; exit 1; }

KR=$(on_target 'uname -r')
OLD=$(mktemp); trap 'rm -f $OLD' EXIT
on_target "cat /lib/modules/$KR/build/Module.symvers" | awk '$3 == "vmlinux"' > "$OLD"

# 输出：校验和不同的符号、旧内核有而新内核没有的符号
BAD=$(awk 'NR == FNR { crc[$2] = $1; next }
	($2 in crc) { if (crc[$2] != $1) print "校验和不同：" $2; delete crc[$2] }
	END { for (s in crc) print "新内核缺少：" s }' "$OLD" "$B/vmlinux.symvers")
NEWONLY=$(awk 'NR == FNR { old[$2] = 1; next } !($2 in old) { print $2 }' "$OLD" "$B/vmlinux.symvers")

echo "旧内核导出符号 $(wc -l < "$OLD") 个，新内核 $(wc -l < "$B/vmlinux.symvers") 个"
echo "新内核多出来的符号：$(echo $NEWONLY)"
VM_NEW=$(modinfo -F vermagic "$B/drivers/i2c/i2c-stub.ko")
VM_OLD=$(on_target "modinfo -F vermagic \$(find /lib/modules/$KR/kernel -name \"*.ko*\" | head -1)")
echo "vermagic 新：$VM_NEW"
echo "vermagic 旧：$VM_OLD"

if [ -n "$BAD" ]; then
	echo "$BAD" | head -20
	echo "不兼容！别装。检查配置差异（./prepare.sh 打印的 diff）"
	exit 1
fi
[ "$VM_NEW" = "$VM_OLD" ] || { echo "vermagic 不一致！别装。"; exit 1; }
echo "兼容：旧模块可以直接用。下一步 ./install.sh"
