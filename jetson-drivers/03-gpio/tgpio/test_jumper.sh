#!/bin/bash
# 第 3 章测试（真实引脚）：用一根杜邦线把排针 29 脚（LED 输出）接到 31 脚（按键输入）
# 驱动自己输出的电平就是自己读到的按键电平：LED 写 0 → 31 脚低 → 按键「按下」（低电平有效）
# 运行：cd 03-gpio/tgpio && make && ./test_jumper.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }

check "覆盖层已生效" "[ -d /proc/device-tree/tutorial-gpio ]"
sudo rmmod tgpio_simboard tgpio dtdemo 2>/dev/null
# 跳线时必须关掉「按下翻转 LED」：否则 LED 翻转 → 按键松开 → 再按下 → 再翻转……停不下来
sudo insmod tgpio.ko toggle_on_press=0
D=/sys/bus/platform/devices/tutorial-gpio
check "驱动绑定到 tutorial-gpio（真实引脚）" "[ -e $D/driver ]"

echo 1 | sudo tee $D/led > /dev/null; sleep 0.1
check "LED 输出 1 时按键读到松开（0）——读到 1 说明跳线没接好" "[ \"\$(cat $D/button)\" = 0 ]"

p0=$(cat $D/presses)
for i in 1 2 3 4 5; do
	echo 0 | sudo tee $D/led > /dev/null; sleep 0.1
	echo 1 | sudo tee $D/led > /dev/null; sleep 0.1
done
p1=$(cat $D/presses)
check "5 次低脉冲算 5 次按下（实际 $((p1 - p0))）" "[ $((p1 - p0)) -eq 5 ]"
check "真实引脚的中断边沿数：$(cat $D/edges)（至少 10）" "[ \$(cat $D/edges) -ge 10 ]"

sudo rmmod tgpio
echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
