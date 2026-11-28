#!/bin/bash
# 编译教程覆盖层：先用 C 预处理器展开 #include 和宏，再用 dtc 编译成 .dtbo
set -e
cd "$(dirname "$0")"
KH=/lib/modules/$(uname -r)/build

cpp -nostdinc -undef -x assembler-with-cpp -D__DTS__ \
    -I "$KH/include" -I "$KH/arch/arm64/boot/dts" \
    tutorial-overlay.dts > tutorial-overlay.pp.dts
dtc -@ -q -I dts -O dtb -o tutorial-overlay.dtbo tutorial-overlay.pp.dts
rm -f tutorial-overlay.pp.dts
echo "生成 $(pwd)/tutorial-overlay.dtbo"
