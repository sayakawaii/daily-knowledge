# 第 12 章：CSI 摄像头与 V4L2

> 目标：
> 1. 搞懂 Linux 摄像头驱动的框架：V4L2（格式协商、控件、缓冲区队列）、videobuf2、media controller；
> 2. 写一个虚拟摄像头驱动 `tcam`，不接硬件就能被 `v4l2-ctl`、`v4l2-compliance` 和自己写的采集程序当成真摄像头用；
> 3. 写一个用户态采集程序，亲手走一遍 REQBUFS → mmap → QBUF → STREAMON → DQBUF 的流程；
> 4. 知道 Jetson 上真实的 CSI 摄像头是怎么接进来的：传感器子设备、CSI、VI、设备树覆盖层，以及什么时候用 V4L2、什么时候用 Argus。

虚拟摄像头部分不用接任何东西。真实摄像头部分需要一个 IMX219 模组（见第 7 节）。

---

## 1. 摄像头驱动要解决什么问题

单片机上接一个摄像头（比如 OV7670 + DCMI），流程大概是：I2C 配传感器寄存器 → 配 DCMI 和 DMA → DMA 把一帧写进 RAM → 中断通知 → 处理完再启动下一帧。

Linux 上要解决的问题一样，但多了几个约束：

- **应用程序不能直接碰 DMA 缓冲区的物理地址**，内核要把缓冲区映射给用户态，并且管好「这块缓冲区现在归谁」；
- **不能拷贝**：1080p30 的 RAW 数据每秒 120 MB，多拷一次 CPU 就浪费一截。所以缓冲区要在内核、用户态、GPU、编码器之间**传递所有权**，而不是传数据；
- **各家摄像头千差万别**，但应用程序（OpenCV、GStreamer、浏览器）只想写一套代码。

V4L2（Video for Linux 2）就是这套统一接口。所有摄像头在用户态看来都是一个 `/dev/videoN`，用同一组 ioctl 操作。

## 2. V4L2 的三件事：格式、控件、缓冲区

**格式协商**。应用说「我要 1280x720 RGB24」，驱动回答「我能给的最接近的是 ……」：

| ioctl | 作用 |
|---|---|
| `VIDIOC_ENUM_FMT` / `ENUM_FRAMESIZES` / `ENUM_FRAMEINTERVALS` | 列出支持的像素格式、分辨率、帧率 |
| `VIDIOC_TRY_FMT` | 「如果我要这个，你会给我什么？」只修正、不生效 |
| `VIDIOC_S_FMT` | 修正后生效。缓冲区已经申请了就返回 `EBUSY` |
| `VIDIOC_G_FMT` | 当前格式，包括每行字节数 `bytesperline` 和每帧字节数 `sizeimage` |

像素格式用 4 个字符的 **fourcc** 表示：`YUYV`（YUV 4:2:2，每像素 2 字节）、`RGB3`（RGB24）、`RG10`（10 位拜耳 RAW，传感器原始数据）……

**控件**（`VIDIOC_QUERYCTRL` / `S_CTRL`，或扩展的 `S_EXT_CTRLS`）：亮度、曝光、增益、翻转。驱动用 `v4l2_ctrl_new_std()` 声明，范围检查、`--list-ctrls` 显示、事件通知全由框架做。

**缓冲区队列**——最核心的部分。想象两个传送带：

```
          QBUF（空缓冲区）                       驱动填满
  用户  ──────────────────→  [ 驱动的空闲队列 ]  ─────────→  [ 完成队列 ]  ──DQBUF──→  用户
   ↑                                                                                  │
   └──────────────────────── 用户处理完，再 QBUF 还回去 ←─────────────────────────────┘
```

1. `REQBUFS`：申请 N 个缓冲区（内核分配内存）；
2. `QUERYBUF` + `mmap`：把每个缓冲区映射到用户态，之后用户直接读这块内存，没有拷贝；
3. `QBUF` 全部缓冲区：交给驱动；
4. `STREAMON`：开始采集。驱动每出一帧，就把一个缓冲区从空闲队列挪到完成队列；
5. 循环：`poll` 等待 → `DQBUF` 拿到填好的缓冲区 → 处理 → `QBUF` 还回去；
6. `STREAMOFF`：停止，驱动把手里所有缓冲区都还回来。

用户来不及还缓冲区时，驱动手里没有空缓冲区，这一帧就**丢掉**。帧号 `sequence` 照样加 1，所以用户从帧号跳变就能知道丢了几帧。

## 3. videobuf2：缓冲区队列不用自己写

上面那套队列逻辑很容易出错（并发、mmap 生命周期、DMABUF 导入导出……），所以内核有一个通用实现 **videobuf2**（vb2）。驱动只需要实现 5 个回调：

| 回调 | 什么时候调用 | `tcam` 里做什么 |
|---|---|---|
| `queue_setup` | REQBUFS 时 | 告诉 vb2 每个缓冲区多大（`sizeimage`）、几个平面 |
| `buf_prepare` | QBUF 时 | 检查缓冲区够不够大 |
| `buf_queue` | 缓冲区交给驱动 | 挂到自己的空闲链表上（加自旋锁，出帧的 work 也会碰这个链表） |
| `start_streaming` | STREAMON 时 | 开始出帧 |
| `stop_streaming` | STREAMOFF 时 | 停止出帧，把手里**每一个**缓冲区都用 `vb2_buffer_done(..., ERROR)` 还回去，漏一个 vb2 就报 WARN |

出帧的时候：从链表取一个缓冲区 → `vb2_plane_vaddr()` 拿到地址填数据 → 设置时间戳和帧号 → `vb2_buffer_done(..., DONE)`。vb2 负责唤醒正在 `poll` 的用户。

ioctl 里和缓冲区有关的全部直接用 vb2 提供的现成函数：`.vidioc_reqbufs = vb2_ioctl_reqbufs` 等。

vb2 还要选一种**内存分配方式**（`mem_ops`）：

- `vb2_vmalloc_memops`：虚拟连续、物理不连续。CPU 填数据用这个，`tcam` 和 USB 摄像头（uvcvideo）都是；
- `vb2_dma_contig_memops`：物理连续（或经 IOMMU 映射成设备看来连续）。硬件 DMA 直接写的用这个，Jetson 的 `tegra_camera` 就是（`lsmod` 能看到它依赖 `videobuf2_dma_contig`）；
- `vb2_dma_sg_memops`：分散 / 聚集链表，支持 SG DMA 的硬件用。

## 4. 驱动 `tcam` 逐段讲解

代码在 `tcam/tcam.c`。

**设备从哪来**：没有硬件，模块加载时用 `platform_device_register_simple("tutorial-cam", ...)` 自己注册一个平台设备，驱动就有东西可以 probe。这和第 2 章用设备树创建平台设备，效果一样。

**probe 里建三样东西**：

```c
v4l2_device_register(dev, &c->v4l2_dev);        // 1. 顶层容器，管着下面所有东西
v4l2_ctrl_new_std(&c->ctrls, NULL, V4L2_CID_BRIGHTNESS, 0, 255, 1, 128);
v4l2_ctrl_new_std(&c->ctrls, NULL, V4L2_CID_HFLIP, 0, 1, 1, 0);   // 2. 控件
vb2_queue_init(q);                              // 3. 缓冲区队列
video_register_device(&c->vdev, VFL_TYPE_VIDEO, -1);   // 出现 /dev/videoN
```

`q->lock` 和 `vdev.lock` 设成同一把 mutex，V4L2 核心就会把所有 ioctl 串行化，驱动里大部分函数不用自己加锁。
只有出帧的 work 和 `buf_queue` 之间共享的缓冲区链表需要额外的自旋锁。

**出帧**：用一个 `delayed_work` 每 1/30 秒跑一次。下一次的时间按**绝对时间**算（`c->next += 33.3 ms`），而不是「现在 + 33.3 ms」，这样调度延迟不会累积，长期帧率精确是 30（实测 29.99）。

**画面**：8 条竖彩条（白黄青绿品红蓝黑），中间一个灰色方块每帧右移 8 像素。YUYV 用 BT.601 有限范围的值（白 Y=235，黑 Y=16），所以测试能精确检查像素值。
亮度控件给每个像素加 `(亮度 - 128)`，截断到 0～255；水平翻转把彩条左右反过来。

**格式协商**：`tcam_adjust()` 把任何请求修正到最接近的支持格式：不认识的 fourcc 变成 YUYV，分辨率用 `v4l2_find_nearest_size()` 挑最近的，然后算出 `bytesperline` 和 `sizeimage`。TRY_FMT 和 S_FMT 共用它，保证两者结果一致，这是 v4l2-compliance 会检查的。

**真实驱动和它的区别只在「帧从哪来」**。把 `tcam_work()` 换成「硬件 DMA 完成中断里调 `vb2_buffer_done()`」，`buf_queue` 里把缓冲区的 DMA 地址（`vb2_dma_contig_plane_dma_addr()`）写进硬件，就是一个真实的采集驱动。

## 5. 用户态采集程序 `cam_capture`

`tcam/cam_capture.c` 就是第 2 节那个流程的最小实现，大约 140 行，对任何 V4L2 采集设备都能用（第 7 节的真实摄像头测试也用它）。

```
./cam_capture /dev/video0 60 out.yuv
FRAME 0 1067.104512 614400
...
SUMMARY frames=60 dropped=0 fps=29.99
```

帧率用驱动打的时间戳算，不是用户态的时钟，所以测的是驱动本身。`dropped` 是从帧号跳变算出来的。
TODO 有两个：DQBUF 取帧并写文件、QBUF 还回去。想一想**忘了 QBUF 会怎样**：4 个缓冲区用完以后，驱动手里没有空缓冲区，之后的帧全部丢掉，`poll` 再也不返回。

## 6. 测试

```bash
cd 12-camera/tcam
make && ./test.sh
```

TODO 一共 3 个（加上 `cam_capture.c` 里的 2 个）：找到 `tutorial-cam` 对应的 `/dev/videoN`、用 `v4l2-ctl` 设格式、用 `v4l2-ctl` 设亮度。测试会检查：

- `v4l2-compliance -s`（官方的一致性测试，含流式采集）0 失败 0 警告；
- 60 帧不丢、帧率 29～31；
- 像素值：左上角白 235、右上角黑 16；亮度 200 时白饱和到 255、黑变成 88；翻转后左上角变黑；
- 画面在动：两帧的中间一行不同、第一行相同；
- RGB24 格式和 `read()` 读法。

实测：15 项全过，`v4l2-compliance` 53 项全过。

几个好用的命令：

```bash
v4l2-ctl -d /dev/video0 --all                  # 设备、格式、控件全部信息
v4l2-ctl -d /dev/video0 --list-formats-ext     # 支持的格式、分辨率、帧率
v4l2-ctl -d /dev/video0 --list-ctrls
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=100   # 不存文件，只看帧率
v4l2-ctl -d /dev/video0 --log-status           # 驱动在 dmesg 里打印控件状态
```

想用眼睛看画面：把 `out.yuv` 拷回电脑，用 ffplay 播放：`ffplay -f rawvideo -pixel_format yuyv422 -video_size 640x480 out.yuv`。

## 7. Jetson 上真实的 CSI 摄像头

### 7.1 硬件链路

```
 IMX219 传感器 ──MIPI CSI-2（2 条数据线 + 时钟线）──→ NVCSI ──→ VI ──→ 内存 ──→ /dev/videoN（RAW 拜耳）
     ↑                                                                    │
     └── I2C（地址 0x10）：驱动配置寄存器、开关流                          └──→ ISP（去马赛克、白平衡、降噪）──→ Argus
```

- **传感器**是一个 I2C 设备，驱动 `nv_imx219` 只负责配寄存器：分辨率、曝光、增益、开始 / 停止输出。它在 V4L2 里是一个**子设备**（v4l2_subdev），没有自己的 `/dev/videoN`；
- **NVCSI** 接收 CSI-2 串行数据，解包；
- **VI**（Video Input）把数据写进内存。这部分在 Orin 上由一个专用的实时处理器（camera RTCPU）上的固件控制，Linux 驱动 `tegra_camera` 和它通信（所以 `lsmod` 里有 `tegra_camera_rtcpu`、`capture_ivc`）；
- 传感器输出的是**拜耳 RAW**：每个像素只有 R、G、B 中的一种颜色，按 RGGB 排列。要变成正常图像，需要 ISP 做去马赛克（demosaic）。

**media controller** 把这条链路描述成一张图：每个硬件块是一个实体（entity），之间用链接（link）连起来。`media-ctl -p` 打印这张图。现在没接摄像头，图是空的，只有 `tegra-capture-vi` 这个设备本身。

驱动之间怎么找到彼此？传感器驱动和 VI 驱动是两个独立的模块，加载顺序不定。设备树里用 **端点（endpoint）** 描述「传感器的 port 0 连到 NVCSI 的某个 port」，
各驱动用 `v4l2_async` 登记「我在等某个端点上的设备」，双方都到齐时框架把它们连起来。

### 7.2 V4L2 还是 Argus

| | V4L2（`/dev/videoN`） | Argus（`nvarguscamerasrc`） |
|---|---|---|
| 输出 | 传感器原始 RAW（RG10） | 经过 ISP 的 YUV（NV12） |
| 自动曝光、白平衡 | 没有，自己写 | 有 |
| 适合 | 学驱动、调试传感器、自己做图像处理 | 实际应用 |

驱动开发者主要和 V4L2 这一层打交道。确认 V4L2 能稳定出 RAW 帧，是传感器驱动「调通」的标志；Argus 能不能出好看的图，是 ISP 调参（NVIDIA 的 tuning 文件）的事。

### 7.3 设备树覆盖层

板子上已经有 NVIDIA 做好的覆盖层，`/boot/tegra234-p3767-camera-p3768-imx219-A.dtbo` 等。反编译看一下（`dtc -I dtb -O dts 文件`），关键内容：

- 传感器节点挂在 `cam_i2cmux/i2c@0` 下，地址 `0x10`，`compatible = "sony,imx219"`（驱动靠这个匹配，见 `modinfo nv_imx219` 的 alias）；
- `num_lanes = "2"`、`tegra_sinterface = "serial_b"`：用 CSI 的哪个口、几条数据线；
- 5 个模式（mode0～mode4），每个写明分辨率和最高帧率：

| 模式 | 分辨率 | 最高帧率 |
|---|---|---|
| mode0 | 3280x2464 | 21 fps |
| mode1 | 3280x1848 | 28 fps |
| mode2 | 1920x1080 | 30 fps |
| mode3 | 1640x1232 | 30 fps |
| mode4 | 1280x720 | 60 fps |

这就是「换一个传感器要做什么」的答案：写传感器驱动（寄存器表）+ 写这样一个设备树描述 + 写 ISP 调参文件。

### 7.4 动手：接上 IMX219

**购物**：IMX219 摄像头模组（树莓派摄像头 v2 用的就是这颗传感器，国产兼容模组几十元起）。
注意排线：Orin Nano 开发板上的摄像头座是 **22 针 0.5 mm 间距**，树莓派原版模组是 15 针 1 mm 间距，需要一根「15 转 22 针」排线。买「Jetson 专用」的 IMX219 模组一般自带 22 针排线，省事。

**步骤**：

1. **断电**，插排线。排线金属触点朝哪边看卖家的图，插反不会烧，但传感器不会应答；
2. 开机，用 Jetson-IO 同时打开本教程的覆盖层和摄像头：

   ```bash
   sudo /opt/nvidia/jetson-io/config-by-hardware.py -n 1="jetson-drivers tutorial" 2="Camera IMX219-A"
   ```

   它会在 `/boot/extlinux/extlinux.conf` 里新增一个 `JetsonIO` 启动项（两个覆盖层用逗号连起来写在 `OVERLAYS` 一行），并设成默认。重启生效；
3. 跑测试：`cd 12-camera/imx219 && ./test_imx219.sh`。TODO 2 个：找视频节点、设 1920x1080 RG10 格式；
4. 传感器没应答（第二项失败）时：先查排线方向；再试另一个摄像头座，对应覆盖层 `Camera IMX219-C`；
5. 用完想回到原来的启动项：把 `extlinux.conf` 里的 `DEFAULT JetsonIO` 改回 `DEFAULT tutorial`（第 9 章讲过启动项）。

> 这一节写成时手上没有摄像头：Jetson-IO 的命令和覆盖层内容在板子上核对过，`test_imx219.sh` 在不接摄像头时跑过（正确地报失败并退出），但接上摄像头的实际结果没有验证过。你跑通后，有和这里写的不一样的地方，告诉我改。

**看画面**：最简单的是走 Argus，让 ISP 处理好再存 JPEG：

```bash
gst-launch-1.0 nvarguscamerasrc num-buffers=1 ! nvjpegenc ! filesink location=test.jpg
```

V4L2 采到的 RAW 文件要自己去马赛克才能看，这是练习 3。

## 8. 练习

1. **加一个控件**：给 `tcam` 加对比度（`V4L2_CID_CONTRAST`），改 `tcam_fill()` 让它生效，再跑一遍 `v4l2-compliance` 确认还是全过；
2. **改用 dma_contig**：把 `tcam` 的 `mem_ops` 换成 `vb2_dma_contig_memops`（`q->dev` 要设成 `&pdev->dev`），对比两者在 `cat /proc/meminfo` 里的差别，想一想真实硬件为什么必须用它；
3. **看 RAW 图像**：用 `test_imx219.sh` 采到的 RG10 文件，写一个小程序做最简单的去马赛克（每 2x2 的 RGGB 块合成一个像素），输出 PPM 图片。
   先要搞清楚 Tegra VI 把 10 位数据怎么放进 16 位里（看 `v4l2-ctl --get-fmt-video` 的 `bytesperline`，再看数据的数值范围）；
4. **丢帧实验**：改 `cam_capture`，处理每一帧时 `usleep(50000)`（比 33 ms 长），看 `dropped` 怎么变化。再把缓冲区数从 4 改成 8，结果会变吗？为什么？

## 本章文件

```
tcam/tcam.c             虚拟摄像头驱动（V4L2 + vb2）
tcam/cam_capture.c      用户态采集程序（2 个 TODO）
tcam/test.sh            测试（3 个 TODO）
imx219/test_imx219.sh   真实摄像头测试（2 个 TODO，未在硬件上验证）
```
