#!/bin/bash
# 第 4 章测试（风扇）：用 tfan 替换 pwm-fan，控制转速、读转速计、测温度保护
# 测试期间风扇由我们的驱动控制；脚本结束（包括 Ctrl+C）时自动恢复 pwm_fan 和 nvfancontrol。
# 运行：cd 04-pwm-fan/tfan && make && ./test.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }
R=../../tools/regpeek/regpeek

restore() {
	sudo rmmod tfan 2>/dev/null
	sudo modprobe pwm_fan
	sudo systemctl start nvfancontrol
	echo "已恢复 pwm_fan 和 nvfancontrol"
}
trap restore EXIT

check "覆盖层已生效" "[ -d /proc/device-tree/tutorial-fan ]"
sudo rmmod dtdemo 2>/dev/null
sudo systemctl stop nvfancontrol      # NVIDIA 的风扇控制服务，会往 pwm-fan 的 hwmon 里写值
sudo rmmod pwm_fan                    # 释放 PWM3，否则我们 devm_pwm_get 会得到 -EBUSY
sudo dmesg -C
sudo insmod tfan.ko
check "驱动加载成功" "sudo dmesg | grep -q 'fan ready'"

# 1. 找到我们的 hwmon 目录
# TODO 1：把 H 设成 name 文件内容是 tutorial_fan 的那个 /sys/class/hwmon/hwmonN 目录。
#   hwmonN 的编号每次开机可能不同，不能写死。提示：grep -l 可以列出内容匹配的文件名，dirname 去掉文件名。
H=
check "找到 hwmon 目录" "[ -n '$H' ] && [ \"\$(cat $H/name 2>/dev/null)\" = tutorial_fan ]"

# 2. 全速和低速
# TODO 2：写一个函数 setpwm，参数是 0~255，写到 $H/pwm1，然后等 4 秒让风扇转速稳定。

setpwm 255 2>/dev/null; HI=$(cat $H/fan1_input 2>/dev/null || echo 0)
setpwm 60 2>/dev/null;  LO=$(cat $H/fan1_input 2>/dev/null || echo 0)
echo "      全速 $HI RPM，pwm=60 时 $LO RPM"
check "全速比低速快 1500 RPM 以上" "[ $HI -gt $((LO + 1500)) ]"

# 3. 直接看硬件寄存器
# PWM3 控制器在 0x32a0000，寄存器第 31 位是使能，第 16~24 位是占空比（256 = 100%），低 13 位是分频
REG=$(sudo $R 0x32a0000 | awk '{print $2}')
# TODO 3：从 REG 里取出占空比字段，存到 DUTY（十进制）。用 bash 算术：$(( (值 >> 位移) & 掩码 ))
#   REG 是 0x 开头的字符串，bash 算术能直接认。
DUTY=
check "pwm1=60 时寄存器占空比字段约为 60（实际 $DUTY）" "[ -n '$DUTY' ] && [ $DUTY -ge 59 ] && [ $DUTY -le 61 ]"
check "使能位是 1" "[ \$(( ($REG >> 31) & 1 )) -eq 1 ]"

# 4. 温度保护
T=$(cat $H/temp1_input 2>/dev/null || echo 0)
echo "      当前 CPU 温度 $((T / 1000)) °C"
# TODO 4：把模块参数 max_temp_mc 改成比当前温度低 10 度（单位毫摄氏度），触发温度保护。
#   模块参数文件在哪，第 0 章讲过。

sleep 2
check "温度保护接管，pwm1 读到 255" "[ \"\$(cat $H/pwm1 2>/dev/null)\" = 255 ]"
check "dmesg 里有 forced to full speed" "sudo dmesg | grep -q 'forced to full speed'"
echo 75000 | sudo tee /sys/module/tfan/parameters/max_temp_mc > /dev/null
sleep 2
check "温度恢复后回到 60" "[ \"\$(cat $H/pwm1 2>/dev/null)\" = 60 ]"

# 5. 范围检查
check "pwm1 写 256 被拒绝" "! echo 256 | sudo tee $H/pwm1 > /dev/null 2>&1"

echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
