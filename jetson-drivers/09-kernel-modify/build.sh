#!/bin/bash
# 第 2 步：编译内核镜像，以及这次新打开的两个模块。
#   ./build.sh            默认并行数：板子上用全部核心，WSL 上用一半
#   J=4 ./build.sh        自己指定
# 全程 nice -n 19：别的程序（游戏、桌面）要 CPU 时，编译自动让开。
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
source "$HERE/env.sh"
[ -f "$B/.config" ] || { echo "先运行 ./prepare.sh"; exit 1; }

echo "== 编译 Image（-j$J）"
SECONDS=0
nice -n 19 "${KMAKE[@]}" -j"$J" Image
echo "== Image 用时 $((SECONDS / 60)) 分 $((SECONDS % 60)) 秒"

# 只编这两个模块。其它几千个模块不用重编：版本号和符号校验和都没变，旧的照样能用。
echo "== 编译 gpio-sim.ko 和 i2c-stub.ko"
nice -n 19 "${KMAKE[@]}" -j"$J" drivers/gpio/gpio-sim.ko drivers/i2c/i2c-stub.ko

ls -l "$B/arch/arm64/boot/Image" "$B/drivers/gpio/gpio-sim.ko" "$B/drivers/i2c/i2c-stub.ko"
echo "编译完成，下一步 ./check_abi.sh"
