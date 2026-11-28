#!/bin/bash
# 第 5 章测试（板载 EEPROM，只读，不需要外接硬件）
# 把载板 EEPROM（I2C 总线 0，地址 0x57）从 at24 驱动换到 teeprom，对比两边读到的内容。
# 脚本结束时（包括 Ctrl+C）自动换回 at24。
# 运行：cd 05-i2c/teeprom && make && ./test.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }
DEV=/sys/bus/i2c/devices/0-0057

restore() {
	[ -e $DEV/driver ] && [ "$(basename $(readlink $DEV/driver))" != at24 ] && \
		echo 0-0057 | sudo tee $DEV/driver/unbind > /dev/null
	sudo rmmod teeprom 2>/dev/null
	[ -e $DEV/driver ] || echo 0-0057 | sudo tee /sys/bus/i2c/drivers/at24/bind > /dev/null
	echo "0-0057 已换回 $(basename $(readlink $DEV/driver))"
}
trap restore EXIT

sudo rmmod teeprom 2>/dev/null
check "0x57 现在由 at24 管理" "[ \"\$(basename \$(readlink $DEV/driver))\" = at24 ]"
sudo cat $DEV/eeprom > /tmp/ee_at24.bin          # at24 读到的内容，作为标准答案

# 1. 从 at24 解绑
# TODO 1：把 0-0057 从 at24 驱动上解绑。
#   每个驱动在 /sys/bus/<总线>/drivers/<驱动名>/ 下有 unbind 文件，往里写设备名。（第 2 章做过）

check "解绑后没有驱动" "[ ! -e $DEV/driver ]"

# 2. 加载 teeprom
sudo dmesg -C
sudo insmod teeprom.ko
# 不用手动 bind：模块加载时，内核拿 id_table 去比对所有还没绑定的 I2C 设备，名字 24c02 对上了就自动 probe
check "teeprom 自动绑定到 0-0057" "[ \"\$(basename \$(readlink $DEV/driver 2>/dev/null))\" = tutorial-eeprom ]"
check "0x50 不受影响，还是 at24" "[ \"\$(basename \$(readlink /sys/bus/i2c/devices/0-0050/driver))\" = at24 ]"

# 3. 内容对比
check "256 字节和 at24 读到的完全一样" "sudo cmp -s $DEV/eeprom /tmp/ee_at24.bin"
check "CRC 校验正确" "[ \"\$(cat $DEV/crc_ok 2>/dev/null)\" = 1 ]"
check "料号以 699-13768 开头（载板 P3768）" "grep -q '^699-13768' $DEV/part_number 2>/dev/null"

# TODO 2：不经过驱动，从 /tmp/ee_at24.bin 里直接取出料号：偏移 0x14（十进制 20），18 个字符，存到 PN。
#   提示：dd 的 bs=1 skip= count=，错误输出扔到 /dev/null
PN=
check "自己取的料号和驱动给的一致（$PN）" "[ -n '$PN' ] && grep -q \"^$PN\" $DEV/part_number"

# 4. 用户态直接访问 I2C
# 0x57 被驱动占用（i2cdetect 里显示 UU），i2cget 默认拒绝访问，要加 -f 强制。
# 读操作只是「先写 1 字节地址、再读 1 字节」，不会改 EEPROM 内容。
# TODO 3：用 i2cget 读总线 0、地址 0x57、偏移 0x14 的一个字节，存到 B（会是 0x36，字符 '6'）。
#   格式：i2cget -f -y 总线号 设备地址 寄存器地址
B=
check "i2cget 读到 0x36" "[ '$B' = 0x36 ]"
check "i2cdetect 里 0x57 显示 UU（被驱动占用）" "i2cdetect -y -r 0 2>/dev/null | grep '^50:' | awk '{print \$9}' | grep -q UU"

echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
