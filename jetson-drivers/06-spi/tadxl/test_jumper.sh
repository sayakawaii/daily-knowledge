#!/bin/bash
# 第 6 章测试（真 SPI 控制器）：用杜邦线把排针 19 脚（MOSI）和 21 脚（MISO）短接
# 通过 spidev 在用户态收发，发出去的字节应该原样收回来。用的是片选 1（/dev/spidev0.1，排针 26 脚），
# 片选 0 已经被覆盖层换成了 tadxl 的设备。
# 先补完 spi_loop.c 里的 TODO，make，再运行：./test_jumper.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }

check "/dev/spidev0.1 存在" "[ -e /dev/spidev0.1 ]"
for hz in 1000000 10000000 25000000; do
	out=$(sudo ./spi_loop /dev/spidev0.1 $hz); rc=$?
	echo "      $hz Hz: $out"
	check "$((hz / 1000000)) MHz 回环一致" "[ $rc -eq 0 ]"
done
echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
