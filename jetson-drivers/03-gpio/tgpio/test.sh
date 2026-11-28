#!/bin/bash
# 第 3 章测试（模拟模式，不需要硬件）：用 gpio-sim 模拟 LED 和按键
#   sim 第 0 根线 = LED（驱动输出），第 1 根线 = 按键（低电平有效，外部拉低 = 按下）
# 运行：cd 03-gpio/tgpio && make && ./test.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }
TOOLS=../../tools/gpio-sim

sudo rmmod tgpio_simboard tgpio dtdemo 2>/dev/null
[ -f $TOOLS/gpio-sim-ext.ko ] || $TOOLS/build.sh > /dev/null || exit 1
eval "$($TOOLS/sim.sh up)"            # 得到 CHIP（如 gpiochip2）和 SIM（模拟器的 sysfs 目录）
check "模拟芯片已创建" "[ -d $SIM/sim_gpio1 ]"

release() { echo pull-up | sudo tee $SIM/sim_gpio1/pull > /dev/null; sleep 0.1; }
# TODO 1：写 press 函数，模拟「按下按键」。
#   按键是低电平有效（板级文件里写的 GPIO_ACTIVE_LOW），所以按下 = 把线拉到低电平。
#   参照上面的 release。
press() { :; }

release                               # 先让按键处于松开状态，再加载驱动
sudo dmesg -C

# TODO 2：加载两个模块：驱动 tgpio.ko 和板级文件 tgpio_simboard.ko。
#   想一想：先加载哪个都行吗？为什么？（答案在 README 的「设备和驱动谁先出现都可以」）

D=/sys/bus/platform/devices/tutorial-gpio-sim
check "驱动绑定到 tutorial-gpio-sim" "[ -e $D/driver ]"
check "dmesg 有 ready" "sudo dmesg | grep -q 'led + button ready'"
check "松开时 button 读到 0" "[ \"\$(cat $D/button 2>/dev/null)\" = 0 ]"

# 1. LED
# TODO 3：通过驱动把 LED 点亮（往驱动的 led 属性写 1）。

check "驱动输出 1，模拟器那边看到 1" "[ \"\$(cat $SIM/sim_gpio0/value)\" = 1 ]"

# 2. 按一下
press
check "按下时 button 读到 1（逻辑值，已经反过来了）" "[ \"\$(cat $D/button 2>/dev/null)\" = 1 ]"
check "presses = 1" "[ \"\$(cat $D/presses 2>/dev/null)\" = 1 ]"
check "按下后 LED 翻转成 0" "[ \"\$(cat $SIM/sim_gpio0/value)\" = 0 ]"
release

# 3. 抖动：50 ms 去抖时间内来回拉 10 次，只应该算一次
echo 50 | sudo tee $D/debounce_ms > /dev/null 2>&1
e1=$(cat $D/edges 2>/dev/null || echo 0)
sudo bash -c "for i in 1 2 3 4 5 6 7 8 9 10; do echo pull-down > $SIM/sim_gpio1/pull; echo pull-up > $SIM/sim_gpio1/pull; done; echo pull-down > $SIM/sim_gpio1/pull"
sleep 0.2
e2=$(cat $D/edges 2>/dev/null || echo 0)
check "抖动产生了 $((e2 - e1)) 个中断边沿（应该 ≥ 20）" "[ $((e2 - e1)) -ge 20 ]"
check "但只算 1 次按下（presses = 2）" "[ \"\$(cat $D/presses 2>/dev/null)\" = 2 ]"
release

# 4. 用户程序用 poll 等按键，不占 CPU
# TODO 4：补完 wait_press.c 里的两处 TODO，重新 make。
./wait_press $D/presses 2 3000 > /tmp/wait_press.out 2>&1 &
WP=$!
sleep 0.3; press; release; press; release
wait $WP; rc=$?
check "wait_press 等到了 2 次按下" "[ $rc -eq 0 ] && grep -q 'press 4' /tmp/wait_press.out"

# 5. 参数检查
check "debounce_ms 写 0 被拒绝" "! echo 0 | sudo tee $D/debounce_ms > /dev/null 2>&1"

# 6. 清理：先卸载板级文件（设备消失，驱动的 remove 被调用），再卸载驱动和模拟器
sudo rmmod tgpio_simboard tgpio 2>/dev/null
check "卸载后 GPIO 已释放" "! sudo cat /sys/kernel/debug/gpio | grep -q 'tutorial-gpio-sim'"
$TOOLS/sim.sh down

echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
