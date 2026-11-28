#!/bin/bash
# 第 12 章真实摄像头测试：IMX219（树莓派摄像头 v2 同款传感器）接 CSI 口
# 前提：断电接好排线，用 Jetson-IO 打开「Camera IMX219-A」并重启（README 第 7 节）
# 运行：cd 12-camera/imx219 && ./test_imx219.sh
# 注意：本脚本写成时手上没有摄像头，没有在硬件上跑过；数值依据是设备树覆盖层里的模式表
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }
T=$(mktemp -d); trap 'rm -rf $T' EXIT
make -C ../tcam cam_capture > /dev/null || exit 1
CAP=../tcam/cam_capture

# ---- 第一层：设备树和 I2C ----
# 覆盖层生效后，设备树里会多出传感器节点：挂在 cam_i2cmux 下的 I2C 地址 0x10
DT=$(ls -d /proc/device-tree/bus@0/cam_i2cmux/i2c@*/rbpcv2_imx219_*@10 2>/dev/null | head -1)
check "设备树里有 IMX219 节点（覆盖层已生效）" "[ -n '$DT' ]"
[ -n "$DT" ] && echo "      $DT"
# 驱动 probe 时会通过 I2C 读传感器的型号寄存器，读不到（排线没插好、插反）就 probe 失败，这里就是空的
I2C=$(ls -d /sys/bus/i2c/drivers/*imx219*/*-0010 2>/dev/null | head -1)
check "nv_imx219 驱动已绑定到 I2C 设备 0x10（传感器应答了）" "[ -n '$I2C' ]"

# ---- 第二层：media controller 拓扑 ----
# Jetson 的采集链路是：传感器（I2C 子设备）→ CSI 接收器 → VI（写内存）→ /dev/videoN
media-ctl -p > $T/topo.txt 2>&1
check "media 拓扑里有 imx219 实体" "grep -q 'imx219' $T/topo.txt"
check "media 拓扑里有 nvcsi 实体" "grep -qi 'nvcsi' $T/topo.txt"

# ---- 第三层：V4L2 视频节点 ----
# 和 tcam 一样，不能假设是 /dev/video0（tcam 也加载着的话就不是）。
# Tegra VI 驱动给视频节点起的名字形如「vi-output, imx219 9-0010」。
#
# TODO 1：找出 name 里含 imx219 的视频节点，把设备文件路径存到 V。

check "找到视频节点 $V" "[ -c '$V' ]"
[ -c "$V" ] || exit 1
check "支持 RG10（10 位 RGGB 拜耳）格式" "v4l2-ctl -d $V --list-formats | grep -q \"'RG10'\""

# TODO 2：用 v4l2-ctl 把格式设成 1920x1080、RG10（覆盖层里的 mode2，最高 30 fps）。
#   提示：和 tcam 测试的 TODO 2 是同一条命令，只是参数不同

check "格式是 1920x1080 RG10" "v4l2-ctl -d $V --get-fmt-video | grep -q 'Width/Height *: 1920/1080' && v4l2-ctl -d $V --get-fmt-video | grep -q \"'RG10'\""
# bypass_mode 是 NVIDIA 驱动的私有控件，默认 1 表示「交给 Argus 用」，直接用 V4L2 采集要设成 0
v4l2-ctl -d $V --set-ctrl=bypass_mode=0 2>/dev/null

# ---- 第四层：真的出帧 ----
OUT=$($CAP $V 60 $T/raw.bin 2>&1)
echo "      $(echo "$OUT" | tail -1)"
check "采到 60 帧" "echo \"\$OUT\" | grep -q '^SUMMARY frames=60 '"
check "帧率 27～31 fps" "echo \"\$OUT\" | awk '/^SUMMARY/ { split(\$4, a, \"=\"); exit !(a[2] > 27 && a[2] < 31) }'"
SZ=$(v4l2-ctl -d $V --get-fmt-video | awk '/Size Image/ {print $4}')
check "每帧大小 = Size Image（$SZ 字节）" "[ \$(stat -c %s $T/raw.bin 2>/dev/null || echo 0) = \$((60 * $SZ)) ]"
# 镜头没被挡住时画面不可能全黑：取第 30 帧中间 4 KB，统计非零字节
NZ=$(tail -c +$((29 * SZ + SZ / 2)) $T/raw.bin 2>/dev/null | head -c 4096 | od -An -tu1 -v | tr -s ' ' '\n' | grep -cv '^0*$')
check "画面不是全黑（非零字节 $NZ / 4096）" "[ '${NZ:-0}' -gt 100 ]"
dmesg | tail -50 | grep -iE "corr|err|timeout" | tail -3 | sed 's/^/      dmesg: /'

echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！看看画面：README 第 7 节有把 RAW 文件转成图片的方法"
