#!/bin/bash
# 第 9 章测试：装好新内核、sudo reboot 之后，在板子上运行。
# 运行：cd 09-kernel-modify && ./test.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }

# ---- 确认跑的是新内核 ----
# 版本号故意和旧内核一样，所以 uname -r 分辨不出来。/proc/version 里有编译器和编译时间，
# 而内核配置本身也编进了内核（CONFIG_IKCONFIG_PROC），可以从 /proc/config.gz 读出来。
#
# TODO 1：从正在运行的内核里取出 CONFIG_TUTORIAL_HELLO 这一行，存到 CFG。
#   提示：/proc/config.gz 是 gzip 压缩的文本。
CFG=
check "正在运行的内核里 CONFIG_TUTORIAL_HELLO=y" "[ '$CFG' = CONFIG_TUTORIAL_HELLO=y ]"
check "版本号没变（$(uname -r)），旧模块才能用" "[ -d /lib/modules/$(uname -r)/kernel ]"

# ---- 编进内核的驱动 ----
check "/proc/tutorial_hello 存在" "[ -r /proc/tutorial_hello ]"
GREET=$(cat /sys/module/tutorial_hello/parameters/greeting 2>/dev/null)
check "编进内核的模块也有 /sys/module/<名字>/parameters" "[ -n '$GREET' ]"
check "/proc/tutorial_hello 第一行就是参数 greeting" "[ \"\$(head -1 /proc/tutorial_hello 2>/dev/null)\" = '$GREET' ]"
# TODO 2：从内核日志里找出 tutorial_hello 开机时打印的那一行，存到 LOG。
#   提示：开机日志在内核的环形缓冲区里，普通用户读不了。
LOG=
check "开机日志里有 tutorial_hello 的输出" "echo '$LOG' | grep -q 'tutorial_hello: '"

# 内核命令行参数：编进内核的模块，参数要写在 extlinux.conf 的 APPEND 行里
if grep -q 'tutorial_hello.greeting=' /proc/cmdline; then
	WANT=$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^tutorial_hello.greeting=//p')
	check "命令行里的 greeting 生效了" "[ '$GREET' = '$WANT' ]"
else
	echo "[跳过] 命令行参数：按 README 的练习在 APPEND 行加 tutorial_hello.greeting=你的名字，重启再测"
fi

# ---- 新打开的选项：IRQ_SIM 编进了内核，GPIO_SIM 是模块 ----
check "内核里有 irq_domain_create_sim（IRQ_SIM 编进去了）" "sudo grep -qw irq_domain_create_sim /proc/kallsyms"
sudo rmmod gpio_sim_ext 2>/dev/null
# TODO 3：用 modprobe 加载内核自带的 gpio-sim（不是 tools/ 里那个 gpio-sim-ext.ko）。
#   modprobe 会去 /lib/modules/<版本>/ 下按名字找，install.sh 已经把它放进去并 depmod 过了。

check "gpio_sim 模块已加载" "lsmod | grep -qw gpio_sim"
check "gpio-sim 的 configfs 目录出现" "[ -d /sys/kernel/config/gpio-sim ]"
# 建一个 4 线的模拟 GPIO 芯片，看能不能用
G=/sys/kernel/config/gpio-sim/k9
sudo mkdir -p $G/bank0 2>/dev/null && echo 4 | sudo tee $G/bank0/num_lines > /dev/null \
	&& echo k9-sim | sudo tee $G/bank0/label > /dev/null && echo 1 | sudo tee $G/live > /dev/null
check "模拟 GPIO 芯片 k9-sim 出现" "grep -qx k9-sim /sys/class/gpio/gpiochip*/label 2>/dev/null || gpiodetect 2>/dev/null | grep -q k9-sim"
echo 0 | sudo tee $G/live > /dev/null 2>&1; sudo rmdir $G/bank0 $G 2>/dev/null

sudo rmmod i2c_stub 2>/dev/null
# TODO 4：用 modprobe 加载 i2c-stub，让它在地址 0x50 模拟一个芯片。
#   提示：参数写法和 insmod 一样，跟在模块名后面。

check "i2c-stub 总线出现" "i2cdetect -l | grep -q 'SMBus stub'"
sudo rmmod i2c_stub 2>/dev/null

# ---- 旧模块照常工作 ----
check "NVIDIA 的 GPU 驱动 nvgpu 正常加载（旧模块和新内核兼容）" "lsmod | grep -qw nvgpu"
check "以太网网卡（驱动是模块）出现" "ls /sys/class/net | grep -q '^enP'"
check "无线网卡（驱动是模块）出现" "ls /sys/class/net | grep -q '^wl'"

echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
