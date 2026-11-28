# 第 9 章各脚本共用的变量。不要直接运行，由其它脚本 source。
#
# 在板子上（aarch64）就是本机编译；在 x86 机器上（比如 WSL）自动切换成交叉编译。
# 都可以用环境变量覆盖：
#   SRC     原始内核源码（只读，不会被修改）
#   W       工作目录：硬链接副本 + 编译输出都在这里
#   TARGET  交叉编译时，板子的 ssh 别名（取配置、对比符号、拷贝文件都用它）
#   J       并行任务数
SRC=${SRC:-$HOME/kernel/Linux_for_Tegra/source/kernel/kernel-noble}
W=${W:-$HOME/kwork}
B=$W/build
TARGET=${TARGET:-jetson-home}

if [ "$(uname -m)" = aarch64 ]; then
	CROSS=0
	J=${J:-$(nproc)}
	KMAKE=(make -C "$W/kernel-noble" O="$B")
else
	CROSS=1
	# 交叉编译不要占满 CPU：默认用一半核心，再配合 nice，玩游戏基本无感
	J=${J:-$(( $(nproc) / 2 ))}
	CROSS_COMPILE=${CROSS_COMPILE:-$HOME/l4t-gcc/x-tools/aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-}
	KMAKE=(make -C "$W/kernel-noble" O="$B" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE")
fi

# 在目标板上执行命令：本机编译时直接执行，交叉编译时通过 ssh
on_target() { if [ $CROSS = 0 ]; then bash -c "$1"; else ssh -n "$TARGET" "bash -c '$1'"; fi; }

# LOCALVERSION 必须和板子上正在跑的内核一致（uname -r 去掉 6.8.12），
# 这样新内核才能直接使用 /lib/modules/<版本>/ 下已有的全部模块。prepare.sh 会写入这个文件。
LV=$(cat "$W/localversion" 2>/dev/null || true)
KMAKE+=(LOCALVERSION="$LV")
