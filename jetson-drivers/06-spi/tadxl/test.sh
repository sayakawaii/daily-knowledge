#!/bin/bash
# 第 6 章测试（模拟模式，不需要硬件）：虚拟 SPI 控制器 tspi_sim + 模拟的 ADXL345，驱动 tadxl
# 运行：cd 06-spi/tadxl && make && ./test.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }
S=/sys/devices/platform/tutorial-spi-sim

sudo rmmod tadxl tspi_sim 2>/dev/null
sudo dmesg -C

# 1. 加载
# TODO 1：加载模拟器 tspi_sim.ko 和驱动 tadxl.ko。
#   模拟器加载后会注册一条新的 SPI 总线，片选 0 上有一个名叫 tutorial-adxl345 的设备。

check "模拟器注册了虚拟 SPI 总线" "sudo dmesg | grep -q 'virtual SPI bus'"
check "驱动 probe 成功（读到了 DEVID 0xE5）" "sudo dmesg | grep -q 'ADXL345 ready'"
check "驱动用的是 SPI 模式 3" "sudo dmesg | grep 'ADXL345 ready' | grep -q 'mode 3'"
# 覆盖层把真 SPI1 片选 0 也改成了 tutorial,spi-demo，没接芯片时读到的 DEVID 不对，probe 失败，这是对的
check "真 SPI 总线上没有芯片，probe 正确地失败了" "sudo dmesg | grep 'spi0.0' | grep -q 'DEVID 0x'"

# 2. 找到 IIO 设备
# TODO 2：把 I 设成 name 是 tutorial-adxl345 的 IIO 设备目录（/sys/bus/iio/devices/iio:deviceN）。

check "IIO 设备出现" "[ -f '$I/in_accel_z_raw' ]"
check "probe 后芯片进入测量模式（POWER_CTL 0x2d = 08）" "sed -n 3p $S/regdump | awk '{print \$14}' | grep -qx 08"

# 3. 读数
check "平放时 Z = 256（1 g）" "[ \"\$(cat '$I/in_accel_z_raw' 2>/dev/null)\" = 256 ]"
echo "-100 50 -256" | sudo tee $S/accel > /dev/null     # 让模拟芯片「翻过来」
check "X = -100（负数，补码）" "[ \"\$(cat '$I/in_accel_x_raw' 2>/dev/null)\" = -100 ]"
check "Z = -256" "[ \"\$(cat '$I/in_accel_z_raw' 2>/dev/null)\" = -256 ]"

# TODO 3：用 raw × scale 算出 Z 轴加速度（m/s²），保留 2 位小数，存到 G。应该约等于 -9.79。
#   bash 不会算小数，用 awk：awk "BEGIN {printf \"%.2f\", 甲 * 乙}"
G=
check "Z 轴加速度 = -9.79 m/s²（实际 $G）" "[ '$G' = -9.79 ]"

# 4. 卸载后芯片回到待机
sudo rmmod tadxl
check "卸载后 POWER_CTL = 00（待机）" "sed -n 3p $S/regdump | awk '{print \$14}' | grep -qx 00"

# 5. SPI 模式不对会怎样
sudo dmesg -C
# TODO 4：用错误的 SPI 模式（spi_mode=0）加载驱动。模块参数写在 insmod 后面。

check "模式 0 时读到的 DEVID 是 0xff，probe 失败" "sudo dmesg | grep -q 'DEVID 0xff'"
check "模式不对时没有 IIO 设备" "! grep -qs tutorial-adxl345 /sys/bus/iio/devices/*/name"

sudo rmmod tadxl tspi_sim 2>/dev/null
echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
