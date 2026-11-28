# 第 10 章：USB（主机侧驱动、gadget、热拔插）

> 目标：
> 1. 搞懂 USB 的设备模型：描述符、接口、端点、四种传输、短包和零长度包；
> 2. 用 configfs 把板子变成一个 USB 设备（gadget）；
> 3. 写一个主机侧 USB 驱动 `tusb`：bulk 读写、异步 URB、`/dev/tusb0`、热拔插不崩；
> 4. 不用硬件就能测：内核的虚拟控制器 `dummy_hcd` 把 gadget 和主机接在同一台机器里。有一根 USB-C 线就能测真硬件。

---

## 1. 给单片机工程师的 USB 速成

如果你在单片机上写过 USB 设备（比如 STM32 的 CDC 虚拟串口），你写的是**设备侧**：回应主机的请求，把描述符交给主机。
Linux 驱动开发里更常见的是**主机侧**：板子是主机，插进来一个设备，由驱动去和它对话。这一章两边都做。

**主机说了算。** USB 总线上只有主机能发起传输，设备只能等主机来问。设备想「主动发数据」，实际做法是主机不停地来读（IN 传输），设备没数据时回 NAK。

**描述符是一棵树：**

```
设备描述符        VID:PID、USB 版本、有几个配置
└── 配置描述符     供电、有几个接口（一般只有一个配置）
    └── 接口描述符   类别（HID、大容量存储、厂商自定义 ff……）、有几个端点
        ├── 端点 0x81  IN（设备→主机），bulk，最大包长 512
        └── 端点 0x02  OUT（主机→设备），bulk，最大包长 512
```

**驱动绑定的是接口，不是设备。** 一个设备可以有多个接口，各由不同的驱动负责。比如一个 USB 耳机：音频接口给 `snd-usb-audio`，按键接口给 `usbhid`。
所以 `probe()` 拿到的是 `struct usb_interface *`，sysfs 里接口叫 `3-1:1.0`（总线 3、端口 1、配置 1、接口 0）。

**四种传输：**

| 类型 | 特点 | 典型设备 |
|---|---|---|
| 控制（control） | 每个设备都有，端点 0；枚举、设置都走它 | 全部 |
| 批量（bulk） | 有带宽就传，不保证时间，保证正确（出错重传） | U 盘、打印机、本章的回环设备 |
| 中断（interrupt） | 主机按固定间隔来问，延迟有上限 | 键盘、鼠标 |
| 等时（isochronous） | 保证带宽和时间，出错不重传 | 摄像头、声卡 |

**最大包长**：全速 64 字节，高速 512，超高速 1024。一次传输会被切成若干个包。

**短包和零长度包（ZLP）**：接收方怎么知道一次传输结束了？靠**短包**，也就是比最大包长短的包。
如果数据长度正好是包长的整数倍（512、1024……），最后一个包是满的，接收方就不知道是不是还有。这时发送方要再补一个**长度为 0 的包**。
这一章会实测：去掉 ZLP，512 字节的传输会卡住超时，剩下的数据还会粘到下一次传输上。

## 2. 板子上的 USB

```bash
lsusb -t
```

```
/:  Bus 001.Port 001: Dev 001, Class=root_hub, Driver=tegra-xusb/4p, 480M     ← USB 2.0 那一半
    |__ Port 002: Dev 002, If 0, Class=Hub, Driver=hub/4p, 480M               ← 载板上的 4 口集线器
    |__ Port 003: Dev 003, If 0, Class=Wireless, Driver=btrtl_usb, 12M        ← M.2 Wi-Fi 卡上的蓝牙（走 USB）
/:  Bus 002.Port 001: Dev 001, Class=root_hub, Driver=tegra-xusb/4p, 10000M   ← USB 3.2 那一半
    |__ Port 001: Dev 002, If 0, Class=Hub, Driver=hub/4p, 10000M
```

- **主机控制器** `3610000.usb`（驱动 `tegra-xusb`，是一个 xHCI 控制器）：4 个 USB-A 口都在它下面。USB 3 的口在 Linux 里是**两条总线**，一条 USB 2、一条 USB 3，同一个物理口两边各占一个端口；
- **设备控制器** `3550000.usb`（驱动 `tegra-xudc`）：就是 USB-C 口。平时 NVIDIA 的 `l4t` gadget 占着它，`/sys/kernel/config/usb_gadget/l4t/` 里有 4 个功能：`acm`（`/dev/ttyGS0` 串口）、`ncm` 和 `rndis`（`usb0`、`usb1` 虚拟网卡，电脑插 USB-C 线就能 `ssh 192.168.55.1`）、`mass_storage`（一个小 U 盘，镜像文件在 `/opt/nvidia/l4t-usb-device-mode/`）。

## 3. Linux USB 软件栈

```
 主机侧（本章写的 tusb）                     设备侧（gadget）
 ┌─────────────────────────────┐            ┌─────────────────────────────┐
 │ USB 驱动（usb_driver）       │            │ 功能（function）：Loopback、 │
 │  tusb、usb-storage、usbhid  │            │  acm、ncm、mass_storage……   │
 ├─────────────────────────────┤            ├─────────────────────────────┤
 │ USB 核心：枚举、匹配、URB    │            │ composite + configfs        │
 ├─────────────────────────────┤            ├─────────────────────────────┤
 │ 主机控制器驱动（HCD）        │            │ 设备控制器驱动（UDC）        │
 │  tegra-xusb 或 dummy_hcd    │ ←─ 线缆 ─→ │  tegra-xudc 或 dummy_udc    │
 └─────────────────────────────┘            └─────────────────────────────┘
```

- **URB**（USB Request Block）：主机侧所有传输的基本单位。驱动填好「哪个端点、哪块缓冲区、多长、完成后调哪个函数」，交给 USB 核心，传完了在中断上下文里回调。
  `usb_bulk_msg()` 是同步版：内部就是「提交一个 URB、等它完成」；
- **gadget** 是 Linux 里「设备侧」的叫法。configfs 让你不写代码，用 `mkdir`、`echo`、`ln -s` 就能拼出一个设备，见 `tools/usb-sim/gadget.sh`。

## 4. 没有硬件怎么测：dummy_hcd

`dummy_hcd` 在一个模块里同时实现了一个**虚拟主机控制器**和一个**虚拟设备控制器**，它们之间用内存「连线」：

```
gadget（Loopback 功能）→ dummy_udc.0 ═══ 内存 ═══ dummy_hcd（总线 3）→ USB 核心枚举 → tusb 驱动
```

所以在同一块板子上，主机侧和设备侧的代码都是真的在跑，枚举过程和真插一个设备完全一样，只是没有线。

本板内核没开 `CONFIG_USB_DUMMY_HCD` 和 `CONFIG_USB_CONFIGFS_F_LB_SS`，`tools/usb-sim/build.sh` 把这两部分源码从第 9 章的内核源码里拿出来，在源码树外编译，一行都不用改。

设备侧用 Loopback 功能：它在 OUT 端点上一次收最多 4096 字节，收到后原样从 IN 端点发回去。VID:PID 用 `1209:0001`，这是 pid.codes 给测试用的号，内核里没有别的驱动会抢它。

## 5. 驱动 `tusb` 逐段讲解

代码在 `tusb/tusb.c`，大约 300 行。

**匹配**：

```c
{ USB_DEVICE_INTERFACE_CLASS(0x1209, 0x0001, USB_CLASS_VENDOR_SPEC) }
```

VID、PID 对上，并且接口类别是 `ff`（厂商自定义）才匹配。`MODULE_DEVICE_TABLE(usb, ...)` 会把这张表写进模块信息。
装进 `/lib/modules` 并 `depmod` 后，设备一插，内核发出带 `MODALIAS=usb:v1209p0001...` 的 uevent，udev 按它自动 `modprobe` 对应的模块（`cat /sys/bus/usb/devices/3-1:1.0/modalias` 可以看到）。

**probe**：

1. `usb_find_common_endpoints()` 在接口的端点里找第一个 bulk IN 和 bulk OUT。不要把端点地址写死，同一个设备的不同固件版本可能不一样；
2. 缓冲区用 `kmalloc`。USB 控制器要对缓冲区做 DMA，**栈上的数组和 vmalloc 的内存都不能用**；
3. `usb_register_dev()` 注册 `/dev/tusb0`。USB 字符设备共用主设备号 180，次设备号从 `minor_base`（192）开始分配。

**write：异步 URB**

```c
usb_fill_bulk_urb(urb, udev, usb_sndbulkpipe(udev, ep_out), buf, len, tusb_out_complete, t);
if (len % mps_out == 0 && len < 4096)
	urb->transfer_flags |= URB_ZERO_PACKET;   // 见第 1 节「零长度包」
usb_submit_urb(urb, GFP_KERNEL);
wait_for_completion_interruptible_timeout(&t->out_done, ...);
```

完成回调 `tusb_out_complete()` 在**中断上下文**里执行，不能睡眠，只记下 `urb->status`，再 `complete()` 唤醒 `write`。
超时或被信号打断时要 `usb_kill_urb()`。它返回时能保证回调已经执行完，之后才能安全地动缓冲区。

**read：同步 `usb_bulk_msg`**

总是按整个缓冲区（4096）去收。如果主机给的长度比设备实际发来的包小，会得到 `-EOVERFLOW`（俗称 babble）。

**ZLP 实验（实测数据）**：把补零长度包那两行改成 `if (0)`，重新编译跑 `usb_loop`：

```
OK 1
OK 100
OK 511
BAD 512 (got -1)        ← 512 正好一个满包，设备不知道传完了，一直等，主机读超时
BAD 513 (got 1025)      ← 上一次的 512 字节还在设备里，和这次的 513 拼成一次传输回来
BAD 2048 (got -1)
OK 4096                 ← 正好填满设备的 4096 字节缓冲区，不需要 ZLP
```

**热拔插和对象生命周期**：设备随时会被拔掉，用户程序却可能还开着 `/dev/tusb0`。

- `struct tusb` 用 `kref` 计数：probe 持有一份，每次 open 再加一份，最后一个 `kref_put` 的人负责释放；
- disconnect 时：先 `usb_deregister_dev()`，之后就不会再有新的 open；然后在 `io_mutex` 保护下设置 `disconnected = true`，之后的 read/write 都返回 `-ENODEV`；
- USB 核心在调用 `disconnect()` **之前**，已经终止了这个接口上所有的 URB，所以正在等待的 write 会立刻醒来，不会卡住 disconnect。

这一套是所有可热拔插设备驱动的通用模式，内核里的 `drivers/usb/usb-skeleton.c` 就是标准范本，值得通读一遍。

**sysfs**：`usb_driver.dev_groups` 把 `ep_in`、`max_packet`、`speed`、`tx_bytes` 等属性挂到接口目录下。

## 6. 测试

```bash
cd 10-usb/tusb
make            # 编 tusb.ko、usb_loop，以及 tools/usb-sim 里的 dummy_hcd.ko、usb_f_ss_lb.ko
./test.sh
```

TODO 一共 4 个：`test.sh` 里 3 个（在 sysfs 里找设备、读端点描述符里的包长、模拟拔线），`usb_loop.c` 里 1 个（一次写、一次读）。
`usb_loop` 测 7 种长度：1、100、511、512、513、2048、4096，覆盖「不满一个包」「正好一个包」「多一点点」「正好填满缓冲区」这几种边界。

实测（dummy_hcd）：14 项全过，吞吐约 30 MB/s。这个数字只代表 CPU 搬内存的速度，因为驱动一次只挂一个 URB，读写还是串行的；练习 1 会改进它。

**接真硬件**（`test_cable.sh`，还没实测）：一根 USB-C 转 USB-A 的**数据线**，C 头插板子的 USB-C 口，A 头插板子自己的 USB-A 口。
板子自己当自己的设备，数据真的走 USB PHY 和线缆。脚本会临时借用 USB-C 口（停掉 `l4t` gadget），退出时自动恢复。
没插线时运行，会报「没找到设备」并正常恢复（这一点实测过）。

## 7. 什么时候写内核驱动，什么时候用 libusb

一个厂商自定义的 USB 设备，也可以完全在用户态驱动：用 libusb（`sudo apt install libusb-1.0-0-dev`），通过 `/dev/bus/usb/` 直接发 URB，不用写一行内核代码。

| | 内核驱动 | libusb |
|---|---|---|
| 开发调试 | 要编模块，出错可能 oops | 普通程序，gdb 随便调 |
| 给别的程序用 | 提供标准接口：网卡、串口、输入设备、V4L2…… | 只有你自己的程序能用 |
| 延迟和吞吐 | 更好 | 大多数场景够用 |
| 典型例子 | U 盘、网卡、键盘、摄像头 | 烧录器、调试器、厂商私有的采集设备 |

**判断标准**：要让系统里现成的程序（`ip`、`cat /dev/ttyX`、`v4l2-ctl`）能直接用这个设备，就写内核驱动；只有自己一个程序用，先考虑 libusb。

## 8. 调试工具

| 工具 | 看什么 |
|---|---|
| `lsusb -t` | 总线拓扑、每个接口绑定了哪个驱动、速度 |
| `sudo lsusb -v -d 1209:0001` | 全部描述符 |
| `sudo cat /sys/kernel/debug/usb/devices` | 内核视角的设备和端点列表 |
| `/sys/bus/usb/devices/3-1:1.0/ep_81/` | 单个端点的描述符字段 |
| `dmesg -w` | 枚举过程、驱动的 `dev_info` |
| usbmon + Wireshark | 抓 USB 包。**本板内核没开 `CONFIG_USB_MON`**，见练习 3 |

## 9. 练习

1. **提高吞吐**：现在一次只挂一个 URB。改成同时挂 4 个读 URB（用 `usb_anchor` 管理），收到的数据放进 kfifo，`read()` 从 kfifo 取。跑 `usb_loop` 对比吞吐；
2. **组合设备**：改 `gadget.sh`，在同一个配置里再加一个 `acm` 功能（虚拟串口，需要 `usb_f_acm`，板子上已经有）。插上后 `lsusb -t` 会看到两个接口，分别绑 `tusb` 和 `cdc_acm`。这正是「驱动绑定的是接口」的例子；
3. **打开 usbmon**（复习第 9 章）：`CONFIG_USB_MON` 是 tristate，编成模块不影响 ABI，所以不用换内核。
   在 `~/kwork` 里 `scripts/config --module USB_MON`，`make olddefconfig`，然后单独编 `drivers/usb/mon/usbmon.ko` 并 `insmod`。
   之后 `sudo cat /sys/kernel/debug/usb/usbmon/3u` 就能看到 `usb_loop` 发出的每个 URB，ZLP 也看得到。这个练习我没有实测；
4. **libusb 版**：用 libusb 写一个用户态程序做同样的回环（先 `rmmod tusb`，否则接口被驱动占着），对比代码量和吞吐。

## 本章文件

```
tusb/tusb.c          主机侧驱动
tusb/usb_loop.c      回环和吞吐测试程序（1 个 TODO）
tusb/test.sh         dummy_hcd 测试（3 个 TODO）
tusb/test_cable.sh   真线缆测试
../tools/usb-sim/    dummy_hcd + Loopback 功能的源码树外编译，gadget.sh 建设备
```
