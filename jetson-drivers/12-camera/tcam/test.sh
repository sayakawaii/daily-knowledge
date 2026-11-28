#!/bin/bash
# 第 12 章测试（虚拟摄像头 tcam，不需要硬件）
# 运行：cd 12-camera/tcam && make && ./test.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; PASS=$((PASS+1)); else echo "[FAIL] $1"; FAIL=$((FAIL+1)); fi; }
# 读文件里第 $2 个字节（从 0 开始）的无符号值
byte() { od -An -tu1 -j "$2" -N1 "$1" 2>/dev/null | tr -d ' '; }
T=$(mktemp -d); trap 'rm -rf $T' EXIT

sudo rmmod tcam 2>/dev/null
sudo modprobe videobuf2-vmalloc
sudo insmod tcam.ko || exit 1
sleep 0.5

# ---- 找设备 ----
# 每个 V4L2 设备在 /sys/class/video4linux/ 下有一个目录（video0、video1……），里面的 name 文件是驱动起的名字。
# Jetson 接了 CSI 摄像头、或者插了 USB 摄像头时，会有好几个 /dev/video*，不能假设我们的就是 video0。
#
# TODO 1：找出 name 是 tutorial-cam 的那个设备，把设备文件路径（形如 /dev/video0）存到 V。
V=
check "找到设备 $V" "[ -c '$V' ] && [ \"\$(cat /sys/class/video4linux/\$(basename '$V')/name)\" = tutorial-cam ]"
[ -c "$V" ] || V=/dev/$(basename "$(grep -lx tutorial-cam /sys/class/video4linux/*/name | head -1 | xargs dirname)")
check "驱动名是 tcam" "v4l2-ctl -d $V --info | grep -q 'Driver name *: tcam'"
COMP=$(v4l2-compliance -d $V -s 2>&1 | grep '^Total')
echo "      $COMP"
check "v4l2-compliance（含流式采集）全部通过" "echo '$COMP' | grep -q 'Failed: 0, Warnings: 0'"

# ---- 设格式 ----
v4l2-ctl -d $V --set-fmt-video=width=1280,height=720,pixelformat=RGB3    # 先故意设成别的
# TODO 2：用 v4l2-ctl 把格式设成 640x480、YUYV。
#   提示：v4l2-ctl -d 设备 --set-fmt-video=width=...,height=...,pixelformat=...

check "格式是 640x480 YUYV，每帧 614400 字节" "v4l2-ctl -d $V --get-fmt-video | grep -q \"Width/Height *: 640/480\" && v4l2-ctl -d $V --get-fmt-video | grep -q \"'YUYV'\" && v4l2-ctl -d $V --get-fmt-video | grep -q 'Size Image *: 614400'"
v4l2-ctl -d $V --set-ctrl=brightness=128,horizontal_flip=0

# ---- 采集 ----
OUT=$(./cam_capture $V 60 $T/a.yuv 2>/dev/null)
echo "      $(echo "$OUT" | tail -1)"
check "采到 60 帧，没有丢帧" "echo \"\$OUT\" | grep -q '^SUMMARY frames=60 dropped=0 '"
check "帧率 29～31 fps（按驱动打的时间戳算）" "echo \"\$OUT\" | awk '/^SUMMARY/ { split(\$4, a, \"=\"); exit !(a[2] > 29 && a[2] < 31) }'"
check "文件大小 = 60 × 614400" "[ \$(stat -c %s $T/a.yuv 2>/dev/null || echo 0) = 36864000 ]"
# YUYV：每 2 个像素 4 个字节 Y0 U Y1 V。第一个像素在白条上，Y = 235；最后一个像素（x=639）在黑条上，Y = 16
check "左上角是白色（Y=235）" "[ \"\$(byte $T/a.yuv 0)\" = 235 ]"
check "右上角是黑色（Y=16）" "[ \"\$(byte $T/a.yuv 1278)\" = 16 ]"
# 中间那一行（y=240）有个每帧右移 8 像素的灰块，所以第 0 帧和第 1 帧的这一行不一样，而第 0 行完全一样
ROW=$((240 * 1280)); F=614400
check "画面在动：第 0、1 帧中间一行不同，第一行相同" "! cmp -s <(tail -c +\$((ROW+1)) $T/a.yuv | head -c 1280) <(tail -c +\$((F+ROW+1)) $T/a.yuv | head -c 1280) && cmp -s <(head -c 1280 $T/a.yuv) <(tail -c +\$((F+1)) $T/a.yuv | head -c 1280)"

# ---- 控件 ----
# TODO 3：用 v4l2-ctl 把亮度（brightness）设成 200。
#   提示：先 v4l2-ctl -d 设备 --list-ctrls 看控件叫什么、范围多少；设置用 --set-ctrl=名字=值

./cam_capture $V 1 $T/b.yuv > /dev/null 2>&1
check "亮度 200：白色饱和到 255，黑色变成 16+72=88" "[ \"\$(byte $T/b.yuv 0)\" = 255 ] && [ \"\$(byte $T/b.yuv 1278)\" = 88 ]"
v4l2-ctl -d $V --set-ctrl=brightness=128,horizontal_flip=1
./cam_capture $V 1 $T/c.yuv > /dev/null 2>&1
check "水平翻转：左上角变成黑色" "[ \"\$(byte $T/c.yuv 0)\" = 16 ]"
v4l2-ctl -d $V --set-ctrl=horizontal_flip=0

# ---- 另一种格式、另一种读法 ----
v4l2-ctl -d $V --set-fmt-video=width=320,height=240,pixelformat=RGB3
./cam_capture $V 1 $T/d.rgb > /dev/null 2>&1
check "RGB24 320x240：一帧 230400 字节，左上角是 255,255,255" "[ \$(stat -c %s $T/d.rgb) = 230400 ] && [ \"\$(byte $T/d.rgb 0)\$(byte $T/d.rgb 1)\$(byte $T/d.rgb 2)\" = 255255255 ]"
# 不用 mmap，直接 read()：vb2 的 VB2_READ 模式，一次 read 一帧
dd if=$V of=$T/e.rgb bs=230400 count=1 status=none 2>/dev/null
check "read() 也能读到一整帧" "[ \$(stat -c %s $T/e.rgb 2>/dev/null || echo 0) = 230400 ]"

sudo rmmod tcam
check "卸载干净" "! grep -qx tutorial-cam /sys/class/video4linux/*/name 2>/dev/null"
echo; echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ] && echo "全部通过！"
