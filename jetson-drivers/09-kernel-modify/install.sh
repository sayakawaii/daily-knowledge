#!/bin/bash
# 第 4 步：并存安装。旧内核一个文件都不动：
#   /boot/Image-tutorial                         新内核（旧的 /boot/Image 保持原样）
#   /lib/modules/<版本>/updates/tutorial/         新编出来的 gpio-sim.ko、i2c-stub.ko
#   extlinux.conf 里新增 LABEL tutorial-kernel，设为默认；原来的启动项全都保留，开机菜单里可以选
# 板子上：./install.sh      WSL 上：./install.sh（自动拷贝到板子再安装）
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
NEWLABEL=tutorial-kernel
CONF=/boot/extlinux/extlinux.conf

install_local() {   # 参数：放着 Image、gpio-sim.ko、i2c-stub.ko 的目录
	local D=$1 KR; KR=$(uname -r)
	sudo install -m 644 "$D/Image" /boot/Image-tutorial
	sudo install -d /lib/modules/$KR/updates/tutorial
	sudo install -m 644 "$D/gpio-sim.ko" "$D/i2c-stub.ko" /lib/modules/$KR/updates/tutorial/
	sudo depmod "$KR"

	if grep -q "^LABEL $NEWLABEL\$" $CONF; then
		echo "启动项 $NEWLABEL 已存在，只更新了文件"
		return
	fi
	[ -f $CONF.before-kernel ] || sudo cp $CONF $CONF.before-kernel
	local OLDDEF; OLDDEF=$(awk '$1 == "DEFAULT" { print $2 }' $CONF)
	# 复制当前默认启动项（含设备树覆盖层），只换掉内核镜像和菜单名
	awk -v old="$OLDDEF" -v new="$NEWLABEL" '
		$1 == "LABEL" { inblk = ($2 == old); if (inblk) print "LABEL " new; next }
		inblk && $1 == "MENU" { print "      MENU LABEL jetson-drivers tutorial kernel"; next }
		inblk && $1 == "LINUX" { print "      LINUX /boot/Image-tutorial"; next }
		inblk { print }' $CONF > /tmp/newblock
	[ -s /tmp/newblock ] || { echo "没找到默认启动项 $OLDDEF"; exit 1; }
	sudo sh -c "cat /tmp/newblock >> $CONF"
	sudo sed -i "s/^DEFAULT .*/DEFAULT $NEWLABEL/" $CONF
	echo "$OLDDEF" | sudo tee /boot/.tutorial-kernel-prev-default > /dev/null
	echo "== 新的启动配置："
	grep -v '^\s*#' $CONF | grep -v '^\s*$'
	echo
	echo "装好了。sudo reboot 之后运行 ./test.sh。"
	echo "回退：./uninstall.sh；如果起不来，接显示器和键盘，开机 30 秒菜单里选 $OLDDEF 或 primary。"
}

if [ "$1" = --local ]; then install_local "$2"; exit; fi

source "$HERE/env.sh"
[ -f "$B/arch/arm64/boot/Image" ] || { echo "先运行 ./build.sh"; exit 1; }
"$HERE/check_abi.sh"
STAGE=$(mktemp -d)
cp "$B/arch/arm64/boot/Image" "$B/drivers/gpio/gpio-sim.ko" "$B/drivers/i2c/i2c-stub.ko" "$HERE/install.sh" "$STAGE/"
if [ $CROSS = 0 ]; then
	install_local "$STAGE"
else
	ssh "$TARGET" 'rm -rf ~/kinstall && mkdir ~/kinstall'
	scp -q "$STAGE"/* "$TARGET":kinstall/
	ssh "$TARGET" 'bash ~/kinstall/install.sh --local ~/kinstall'
fi
rm -rf "$STAGE"
