#!/bin/bash
# 第 4 章测试（排针 PWM）：排针 15 脚，PWM1 控制器（0x3280000）
# 不需要硬件：直接读 PWM 控制器的寄存器，确认驱动真的把硬件配置好了。
# 运行：cd 04-pwm-fan/tpwm && make && ./test.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }
R=../../tools/regpeek/regpeek
D=/sys/bus/platform/devices/tutorial-pwm
# PWM 关闭时控制器时钟是关的，这时读寄存器会让总线桥把控制器封掉直到重启，所以先检查 enable
reg() { if [ "$(cat $D/enable 2>/dev/null)" = 1 ]; then sudo $R 0x3280000 | awk '{print $2}'; else echo 0x0; fi; }

check "覆盖层已生效" "[ -d /proc/device-tree/tutorial-pwm ]"
sudo rmmod tpwm dtdemo 2>/dev/null
sudo insmod tpwm.ko
check "驱动绑定" "[ -e $D/driver ]"
check "周期是设备树里写的 1000000 ns" "[ \"\$(cat $D/period_ns)\" = 1000000 ]"

# 1. 输出 25% 占空比
# TODO 1：用驱动的 sysfs 属性把占空比设成 25%，然后打开输出。（属性名看 tpwm.c 里的 DEVICE_ATTR_RW）

V=$(reg)
check "使能位是 1（寄存器 $V）" "[ \$(( ($V >> 31) & 1 )) -eq 1 ]"
# TODO 2：从 V 里取出占空比字段（第 16~24 位，256 = 100%），存到 DUTY。

check "占空比字段是 64（25% × 256，实际 $DUTY）" "[ '$DUTY' = 64 ]"

# 2. 硬件能做到的周期范围
# 计算：时钟 408 MHz，一个周期分 256 份，分频器 13 位
#   最短周期 ≈ 256 / 408 MHz ≈ 628 ns；最长周期 ≈ 256 × 8192 / 408 MHz ≈ 5.1 ms
check "5 ms（200 Hz）可以" "echo 5000000 | sudo tee $D/period_ns > /dev/null 2>&1"
check "20 ms（50 Hz，舵机常用）做不到，返回错误" "! echo 20000000 | sudo tee $D/period_ns > /dev/null 2>&1"
check "500 ns 太短，返回错误" "! echo 500 | sudo tee $D/period_ns > /dev/null 2>&1"
# TODO 3：把周期改回 1 ms。
#   周期变了以后，驱动会按 duty_pct 重新算占空比的纳秒数，所以占空比还是 25%。

check "改回 1 ms 后占空比字段还是 64" "[ \$(( (\$(reg) >> 16) & 0x1ff )) -eq 64 ]"

# 3. 呼吸灯：工作队列每 20 ms 改一次占空比
echo 1 | sudo tee $D/breathe > /dev/null
d1=$(cat $D/duty_pct); sleep 0.5; d2=$(cat $D/duty_pct)
echo 0 | sudo tee $D/breathe > /dev/null
check "呼吸模式下占空比在变（$d1 → $d2）" "[ $d1 != $d2 ]"

# 4. 关闭
# 关闭后驱动会关掉控制器的时钟，这时再读寄存器会触发总线超时，所以改看 PWM 子系统的记录
echo 0 | sudo tee $D/enable > /dev/null
check "关闭后 enable 读到 0" "[ \"\$(cat $D/enable)\" = 0 ]"
check "debugfs 里 tutorial-pwm 不再是 enabled" "! sudo grep 'tutorial-pwm' /sys/kernel/debug/pwm | grep -q enabled"
sudo rmmod tpwm

echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
