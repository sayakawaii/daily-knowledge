#!/bin/bash
# 回退：删掉 install.sh 装的所有东西，恢复原来的默认启动项。在板子上运行，然后 sudo reboot。
# 正在跑新内核时也可以运行：删的是磁盘上的文件，下次开机才生效。
set -e
CONF=/boot/extlinux/extlinux.conf
PREV=$(cat /boot/.tutorial-kernel-prev-default 2>/dev/null || echo tutorial)

# 删掉 LABEL tutorial-kernel 整块（到下一个 LABEL 或文件结尾）
sudo awk '$1 == "LABEL" { skip = ($2 == "tutorial-kernel") } !skip' $CONF > /tmp/extlinux.new
sudo sed -i "s/^DEFAULT .*/DEFAULT $PREV/" /tmp/extlinux.new
sudo install -m 644 /tmp/extlinux.new $CONF
sudo rm -f /boot/Image-tutorial /boot/.tutorial-kernel-prev-default
sudo rm -rf /lib/modules/$(uname -r)/updates/tutorial
sudo depmod
echo "已回退，默认启动项：$PREV。sudo reboot 生效。"
grep -E '^(DEFAULT|LABEL)' $CONF
