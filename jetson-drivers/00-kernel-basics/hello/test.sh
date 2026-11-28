#!/bin/bash
# 第 0 章测试：加载、参数、sysfs、错误处理、卸载
# 运行：cd 00-kernel-basics/hello && make && ./test.sh
# 把每个 TODO 换成真正的命令，全部 PASS 就算完成。
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }

sudo rmmod hello 2>/dev/null
sudo dmesg -C

# 1. 带参数加载
# TODO 1：用 insmod 加载 hello.ko，传两个参数：name=Tutorial，period_ms=100
#   格式：sudo insmod 模块文件 参数名=值 参数名=值
#   参数名就是 hello.c 里 module_param() 的第一个参数。

check "模块已加载" "lsmod | grep -q '^hello '"
check "dmesg 里有 Hello, Tutorial!" "sudo dmesg | grep -q 'Hello, Tutorial!'"

# 2. 读模块参数
# TODO 2：把 P 设成这个模块的参数目录。
#   每个模块的参数都在 /sys/module/<模块名>/parameters/ 下，一个参数一个文件。
#   权限是 module_param() 的第三个参数：0444 只读，0644 root 可写。
P=
t1=$(cat $P/ticks 2>/dev/null || echo 0); sleep 1; t2=$(cat $P/ticks 2>/dev/null || echo 0)
check "定时器在跑（1 秒内 ticks 增加了 $((t2 - t1))，应该在 8 到 11 之间）" "[ $((t2 - t1)) -ge 8 ] && [ $((t2 - t1)) -le 11 ]"

# 3. 运行中修改参数
# TODO 3：把 period_ms 改成 500。
#   注意 `sudo echo 500 > 文件` 不行：重定向是当前 shell 做的，当前 shell 不是 root。
#   要用 `echo 500 | sudo tee 文件 > /dev/null`。

sleep 0.6
t1=$(cat $P/ticks 2>/dev/null || echo 0); sleep 1; t2=$(cat $P/ticks 2>/dev/null || echo 0)
check "改成 500 ms 后，1 秒内 ticks 增加 $((t2 - t1))，应该是 1 到 3" "[ $((t2 - t1)) -ge 1 ] && [ $((t2 - t1)) -le 3 ]"

# 4. 只读参数不能写
check "ticks 是只读的，写入应该失败" "[ -n '$P' ] && ! echo 0 | sudo tee $P/ticks > /dev/null 2>&1"

# 5. 卸载
# TODO 4：卸载模块（命令是 rmmod，跟模块名，不带 .ko）。

check "卸载后 dmesg 里有 Goodbye" "sudo dmesg | grep -q 'Goodbye, Tutorial!'"

# 6. 错误处理
# TODO 5：把下面 check 里的 true 换成「用 period_ms=5 加载 hello.ko」的命令。
#   hello_init() 发现参数不合法时返回 -EINVAL，insmod 会失败、返回非 0，
#   前面的 ! 把它反过来，这一项就通过了。加 2> /dev/null 可以隐藏错误输出。
check "period_ms=5 时加载失败" "! true"
check "加载失败后模块没有残留" "! lsmod | grep -q '^hello '"

echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
