#!/bin/bash
# 第 2 章测试：设备树节点 → 平台设备 → 驱动匹配 → probe / remove
# 前提：已经按 README 装好教程覆盖层并重启（ls /proc/device-tree/ | grep tutorial 有三项）
# 运行：cd 02-device-tree/dtdemo && make && ./test.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }

check "覆盖层已生效" "[ -d /proc/device-tree/tutorial-gpio ]"
# 第 3、4 章的驱动也匹配这些节点，先卸掉，免得设备已经被别的驱动占了
sudo rmmod dtdemo tgpio tpwm tfan 2>/dev/null
sudo dmesg -C

# 1. 加载
# TODO 1：加载 dtdemo.ko。加载时内核会拿驱动的 of_match_table 去比对所有还没绑定的设备，
#   compatible 对上的就调 probe。

check "模块已加载" "lsmod | grep -q '^dtdemo '"

# 2. 看驱动绑定了哪些设备
# TODO 2：把 D 设成这个驱动在 sysfs 里的目录。
#   平台驱动都在 /sys/bus/platform/drivers/<驱动名>/ 下，驱动名是 dtdemo.c 里 .driver.name 的值。
#   这个目录里，每个绑定的设备是一个符号链接，另外还有 bind、unbind 两个只写文件。
D=
check "绑定了 tutorial-gpio" "[ -n '$D' ] && [ -e $D/tutorial-gpio ]"
check "绑定了 tutorial-pwm"  "[ -n '$D' ] && [ -e $D/tutorial-pwm ]"
check "绑定了 tutorial-fan"  "[ -n '$D' ] && [ -e $D/tutorial-fan ]"

# 3. 读驱动导出的属性
S=/sys/bus/platform/devices/tutorial-fan/summary
check "fan 节点的 pwms 有 2 项（风扇 PWM + 转速计）" "grep -q 'phandles=2' $S 2>/dev/null"
check "gpio 节点读到 debounce-ms = 20" "grep -q 'debounce_ms=20' /sys/bus/platform/devices/tutorial-gpio/summary 2>/dev/null"

# 4. 不经过驱动，直接读设备树原始数据
# TODO 3：把 RAW 设成 tutorial-gpio 节点 debounce-ms 属性的值（十进制）。
#   /proc/device-tree/ 下每个节点是目录、每个属性是文件，文件内容是原始字节。
#   设备树的整数一律是 32 位大端，用 od 读：od -An -t u1 文件 能看到 4 个字节，
#   想办法让它直接输出一个整数（提示：od 的 --endian 选项，加 -t u4）。
RAW=
check "原始数据里 debounce-ms = 20" "[ '$RAW' = 20 ]"

# 5. 手动解绑、重新绑定
# TODO 4：把 tutorial-pwm 从驱动上解绑（往 $D/unbind 写设备名），驱动的 remove 会被调用。

check "解绑后链接消失" "[ -n '$D' ] && [ ! -e $D/tutorial-pwm ]"
check "remove 被调用了" "sudo dmesg | grep -q 'tutorial-pwm: remove'"

# TODO 5：再把它绑回去（写 $D/bind），probe 会再跑一次。

check "重新绑定成功" "[ -n '$D' ] && [ -e $D/tutorial-pwm ]"
check "tutorial-pwm 的 probe 跑了 2 次" "[ \$(sudo dmesg | grep -c 'tutorial-pwm: probe') -eq 2 ]"

# 6. 卸载：所有设备的 remove 都会被调用
sudo rmmod dtdemo 2>/dev/null
check "卸载后 3 个设备都 remove 了" "[ \$(sudo dmesg | grep -c ': remove') -ge 4 ]"

echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
