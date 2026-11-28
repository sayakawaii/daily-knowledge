#!/bin/bash
# 第 1 步：建工作副本、打补丁、生成配置。
#   板子上：  ./prepare.sh
#   WSL 上：  TARGET=jetson ./prepare.sh     （配置从板子上取）
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
source "$HERE/env.sh"

[ -f "$SRC/Makefile" ] || { echo "找不到内核源码：$SRC（见 README「拿到源码」一节）"; exit 1; }
[ $CROSS = 0 ] || [ -x "${CROSS_COMPILE}gcc" ] || { echo "找不到交叉编译器：${CROSS_COMPILE}gcc"; exit 1; }
for t in make gcc flex bison bc perl openssl; do
	command -v $t > /dev/null || { echo "缺少 $t（见 README「编译依赖」）"; exit 1; }
done
mkdir -p "$B"

# 硬链接副本：1.6 GB 的源码几乎不占额外空间，几秒就能建好。
# patch 和 sed -i 都是「写新文件再改名」，所以改动不会影响到原始源码。
if [ ! -d "$W/kernel-noble" ]; then
	echo "== 建工作副本 $W/kernel-noble"
	cp -al "$SRC" "$W/kernel-noble"
fi

cd "$W/kernel-noble"
if [ -f drivers/misc/tutorial_hello.c ]; then
	echo "== 补丁已经打过，跳过"
else
	echo "== 打补丁"
	patch -p1 < "$HERE/patches/0001-misc-add-tutorial_hello.patch"
fi

echo "== 取正在运行的内核配置和版本"
on_target 'zcat /proc/config.gz' > "$B/.config"
KR=$(on_target 'uname -r')
echo "${KR#6.8.12}" > "$W/localversion"
source "$HERE/env.sh"      # 重新读一次，让 LOCALVERSION 生效

echo "== 修改配置"
# 只改这几项。其它全部保持和正在运行的内核一样，这是新内核能复用旧模块的前提。
scripts/config --file "$B/.config" \
	--enable TUTORIAL_HELLO \
	--module GPIO_SIM \
	--module I2C_STUB \
	--disable DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT --enable DEBUG_INFO_NONE
"${KMAKE[@]}" -s olddefconfig

echo "== 和正在运行的内核比较配置（只应该看到上面改的几项，以及编译器版本号）"
diff <(on_target 'zcat /proc/config.gz' | grep -v '^#' | sort) <(grep -v '^#' "$B/.config" | sort) || true

NEW=$("${KMAKE[@]}" -s kernelrelease)
echo "== 新内核版本 $NEW，板子上 $KR"
[ "$NEW" = "$KR" ] || { echo "版本不一致！"; exit 1; }
echo "准备完成，下一步 ./build.sh"
