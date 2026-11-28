#!/bin/bash
# 第 5 章测试（LM75A 温度传感器，用 i2c-stub 模拟，不需要硬件）
# i2c-stub 在一条虚拟 SMBus 总线的 0x48 上模拟一个芯片；用 i2cset 往它的寄存器里写值，
# 就相当于「芯片测到了这个温度」，再看驱动算出来的对不对。
# 运行：cd 05-i2c/tlm75 && make && ./test.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }

sudo rmmod tlm75 i2c_stub 2>/dev/null
sudo modprobe i2c-dev
sudo insmod ../../tools/i2c-stub/i2c-stub.ko chip_addr=0x48 || exit 1
B=$(i2cdetect -l | awk '/SMBus stub/ {sub("i2c-", "", $1); print $1}')
check "模拟总线是 i2c-$B" "[ -n '$B' ]"

# 先给「芯片」设好初始寄存器：配置 = 关断，Tos = 80 ℃，Thyst = 75 ℃
i2cset -y $B 0x48 0x01 0x01
i2cset -y $B 0x48 0x03 0x0050 w
i2cset -y $B 0x48 0x02 0x004b w

# ---- 字节序 ----
# LM75 的 16 位寄存器高字节先传；SMBus 的「字」是低字节先传。
# i2cset ... w 写进 i2c-stub 的值，就是 SMBus 读字时得到的值，所以要把芯片的值高低字节交换后再写。
# 例：80 ℃ → 80 / 0.5 = 160 = 0xa0，放在高 9 位：0xa0 << 7 = 0x5000 → 交换 → 0x0050（上面那行）
#
# TODO 1：算出 25.125 ℃ 对应的 stub 字，写到 W25（0x 开头的 4 位十六进制）。
#   温度寄存器：0.125 ℃/LSB，放在高 11 位（左移 5 位）。
W25=
# TODO 2：算出 -0.5 ℃ 对应的 stub 字，写到 WNEG。负数用 16 位补码。
WNEG=

i2cset -y $B 0x48 0x00 ${W25:-0} w

# ---- 实例化设备 ----
sudo insmod tlm75.ko
# TODO 3：在 i2c-$B 上创建一个名叫 tutorial-lm75、地址 0x48 的设备。
#   没有设备树节点的 I2C 设备，可以往 /sys/bus/i2c/devices/i2c-<N>/new_device 写「名字 地址」来创建，
#   驱动的 id_table 里有这个名字就会 probe。（真硬件接在排针 3/5 脚时，总线号是 7）

H=$(dirname "$(grep -l '^tutorial_lm75$' /sys/class/hwmon/*/name 2>/dev/null | head -1)")
PROBED=0; [ -f $H/temp1_input ] && PROBED=1
check "hwmon 设备出现" "[ $PROBED = 1 ]"
check "probe 时退出了关断模式（配置寄存器 bit0 = 0）" "[ \"\$(i2cget -f -y $B 0x48 0x01)\" = 0x00 ]"
check "temp1_input = 25125（25.125 ℃）" "[ \"\$(cat $H/temp1_input 2>/dev/null)\" = 25125 ]"
check "temp1_max = 80000" "[ \"\$(cat $H/temp1_max 2>/dev/null)\" = 80000 ]"
check "temp1_max_hyst = 75000" "[ \"\$(cat $H/temp1_max_hyst 2>/dev/null)\" = 75000 ]"

i2cset -f -y $B 0x48 0x00 ${WNEG:-0} w
check "负温度：temp1_input = -500" "[ \"\$(cat $H/temp1_input 2>/dev/null)\" = -500 ]"

# ---- 写报警温度 ----
# TODO 4：通过 hwmon 把报警温度 temp1_max 设成 60 ℃（单位是毫摄氏度）。

check "芯片 Tos 寄存器变成 60 ℃（stub 字 0x003c）" "[ \"\$(i2cget -f -y $B 0x48 0x03 w)\" = 0x003c ]"
check "超出范围的值被钳位到 125 ℃" "echo 200000 | sudo tee $H/temp1_max > /dev/null && [ \"\$(cat $H/temp1_max)\" = 125000 ]"

# ---- 删除设备 ----
# TODO 5：删除这个设备（往同一个目录的 delete_device 写地址）。驱动 remove 时会让芯片进入关断模式。

check "删除后芯片回到关断模式（bit0 = 1）" "[ $PROBED = 1 ] && [ \"\$(i2cget -y $B 0x48 0x01 2>/dev/null)\" = 0x01 ]"

sudo rmmod tlm75 i2c_stub
echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
