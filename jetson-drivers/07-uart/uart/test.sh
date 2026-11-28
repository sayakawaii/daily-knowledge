#!/bin/bash
# 第 7 章测试（用户态串口）：ttyTHS1（排针 8 TX / 10 RX）的 UART 内部回环，不需要接线
# 先补完 uart_loop.c 里的 2 处 TODO，再：cd 07-uart/uart && make && ./test.sh
# 普通用户不在 dialout 组，打开 /dev/ttyTHS1 要 sudo（或者 sudo usermod -aG dialout $USER 后重新登录）
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }

check "uart_loop 已编译" "[ -x ./uart_loop ]"
check "没有程序占用 ttyTHS1" "! sudo fuser /dev/ttyTHS1 > /dev/null 2>&1"
for b in 9600 115200 921600 3000000; do
	out=$(sudo ./uart_loop /dev/ttyTHS1 $b 2>&1); rc=$?
	echo "      $out"
	check "$b 波特内部回环收发一致" "[ $rc -eq 0 ]"
done
check "不支持的波特率被拒绝" "! sudo ./uart_loop /dev/ttyTHS1 12345 > /dev/null 2>&1"

echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
