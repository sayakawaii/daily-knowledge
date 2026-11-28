# 第 0 章：Linux 内核入门（写给单片机工程师）

> 目标：搞清楚「驱动在 Linux 里处在什么位置」，写出第一个内核模块，并学会看日志、传参数、卸载。
> 本章不需要任何外接硬件。

## 一、从单片机到 Linux：先对齐概念

你在单片机上写过的东西，在 Linux 里几乎都有对应，只是被拆成了更多层：

| 单片机（裸机或 RTOS） | Linux |
|---|---|
| 一个程序独占整颗芯片，`main()` 里一个大循环 | 内核管硬件，很多个进程（应用程序）轮流使用 CPU |
| 想用 GPIO 就直接写寄存器 | 应用程序**不能**碰寄存器，只能请求内核，由驱动去写 |
| 中断向量表里填函数地址 | 驱动调用 `request_irq()` 向内核登记中断处理函数 |
| 没有 MMU，地址就是物理地址 | 有 MMU，每个进程看到的是虚拟地址；驱动要先 `ioremap()` 才能访问寄存器 |
| 程序跑飞了，看门狗复位 | 应用程序崩了只死它自己；**驱动崩了整个系统可能一起崩**（内核 oops 或 panic） |
| 外设初始化代码写在 `main()` 开头 | 驱动在 `probe()` 里初始化，由内核在「设备和驱动匹配上」时调用 |
| 板子上接了什么，写死在代码里 | 板子上接了什么，写在**设备树**（Device Tree）里，驱动读设备树拿参数 |
| 改完代码要重新烧录 | 驱动可以编成**模块**（`.ko` 文件），系统运行中加载、卸载，不用重启 |

最后一行就是本教程的基础：每一章的驱动都编成模块，`insmod` 加载、`rmmod` 卸载，改一次代码几秒钟就能试一次。

## 二、用户空间和内核空间

```
┌──────────────────────────────────────────────────────┐
│ 用户空间（User Space）                                  │
│   你的应用程序、Python、gpioset、i2cdetect ...           │
│   只能通过「系统调用」请求内核做事：                        │
│   open() read() write() ioctl() poll() mmap() ...      │
├───────────────────────── 系统调用 ─────────────────────┤
│ 内核空间（Kernel Space）                                │
│   VFS（虚拟文件系统）：把 /dev/xxx 的读写转给对应驱动         │
│   子系统框架：GPIO、I2C、SPI、PWM、TTY、网络、hwmon ...      │
│   设备驱动：你要写的部分                                  │
│   进程调度、内存管理、中断管理                              │
├──────────────────────────────────────────────────────┤
│ 硬件：Tegra234 SoC 的寄存器、引脚、中断控制器               │
└──────────────────────────────────────────────────────┘
```

**Linux 的核心思想是「一切皆文件」**：应用程序打开 `/dev/ttyTHS1` 读写，就是在收发串口；打开 `/dev/i2c-7` 再调 `ioctl()`，就是在发 I2C 报文。应用程序调 `read()`，内核找到这个文件对应的驱动，调用驱动里的 `read` 函数。驱动开发，大部分时间就是在写这些「被内核回调的函数」。

为什么不在应用程序里直接写寄存器？
1. **隔离**：寄存器只有一套，多个程序同时写就会乱。由驱动统一管理，并发、互斥都在驱动里解决。
2. **安全**：普通程序不能随意改硬件。
3. **抽象**：上层只认「GPIO 第几根线」，不关心是哪家芯片的寄存器。换一颗芯片，只换驱动，应用程序不用改。

## 三、驱动不是从零写，是「填框架」

这是从单片机转过来最需要转变的一点。Linux 为每类硬件准备好了**子系统框架**，驱动只需要实现框架规定的几个回调函数：

| 硬件 | 子系统 | 驱动要实现什么 | 用户怎么用 | 本教程 |
|---|---|---|---|---|
| 任意简单设备 | 字符设备 / misc | `open` `read` `write` `ioctl` | `/dev/xxx` | 第 1 章 |
| GPIO 控制器 | gpiolib | `direction_input` `get` `set` ... | `/dev/gpiochipN` | 第 3 章用它（作为使用者） |
| PWM 控制器 | PWM 框架 | `apply` `get_state` | `/sys/class/pwm/` | 第 4 章 |
| 风扇、温度 | hwmon | `read` `write`（按属性） | `/sys/class/hwmon/` | 第 4 章 |
| I2C 外设 | I2C 核心 | `probe` `remove`，用 `i2c_smbus_*` 收发 | 由驱动决定 | 第 5 章 |
| SPI 外设和控制器 | SPI 核心 | 外设：`probe`；控制器：`transfer_one` | 由驱动决定 | 第 6 章 |
| 串口外设 | serdev | `receive_buf` 回调 | 由驱动决定 | 第 7 章 |
| CAN 控制器 | SocketCAN（网络） | `ndo_open` `ndo_start_xmit` ... | `socket(PF_CAN, ...)` | 第 8 章 |

举个例子：Tegra 的 I2C 控制器驱动（`i2c-tegra.c`）NVIDIA 已经写好了。你要给一个温湿度传感器写驱动时，只需要写「这个传感器」的部分：上电读哪个寄存器、怎么换算温度。报文怎么在线上发出去，是 I2C 核心加控制器驱动的事。

所以写驱动分两种角色：
- **外设驱动**（设备驱动）：给某个传感器、某块屏写驱动。最常见，本教程大部分是这种。
- **控制器驱动**：给 SoC 里的 I2C、SPI 控制器本身写驱动。通常芯片厂写好了，第 6 章会写一个虚拟的 SPI 控制器让你体验。

## 四、设备模型：总线、设备、驱动

内核用三个对象描述硬件：

```
  总线 (bus)：platform、i2c、spi、usb、pci ...
     │
     ├── 设备 (device)：「这里有一个 xx 设备」，来自设备树或者自动枚举
     │
     └── 驱动 (driver)：「我能驱动 xx 设备」，来自你加载的模块

  两边都注册到总线上，总线负责「配对」：
  设备的 compatible == 驱动的 of_match_table 里的某一项  →  调用驱动的 probe()
```

- **platform 总线**：SoC 内部的外设（GPIO 控制器、PWM 控制器、UART），以及设备树里自定义的节点。它们不能像 USB 那样自动枚举，所以写在设备树里。
- 配对成功，内核调用驱动的 `probe(dev)`，驱动在里面申请资源、初始化硬件。
- 设备拔掉或驱动卸载，内核调用 `remove(dev)`。
- 配对关系在 sysfs 里看得到：`/sys/bus/platform/devices/xxx/driver` 是一个指向驱动的链接。

第 2 章会细讲设备树，第 3 章开始每章都是「设备树节点 + 驱动」的组合。

## 五、内核模块

内核模块就是一段运行中加载到内核里的代码，文件后缀 `.ko`。

```bash
sudo insmod hello.ko name=Jetson   # 加载，并传参数
lsmod | grep hello                 # 看已加载的模块
modinfo hello.ko                   # 看模块信息：参数、依赖、vermagic
sudo rmmod hello                   # 卸载
sudo dmesg | tail                  # 看内核日志（驱动里 pr_info 打印的内容）
sudo dmesg -w                      # 持续跟踪日志，调试时开一个终端专门看这个
```

`modprobe` 和 `insmod` 的区别：`modprobe` 会自动加载依赖的模块，但只认装在 `/lib/modules/$(uname -r)/` 下的模块。本教程的模块都在你的工作目录里，所以用 `insmod`。

### 编译模块需要什么

模块必须用**和正在运行的内核完全匹配**的头文件编译，否则拒绝加载。这块板子上已经装好了：

```bash
ls -l /lib/modules/$(uname -r)/build
# -> /usr/src/linux-headers-6.8.12-1029-tegra-ubuntu24.04_aarch64/3rdparty/canonical/linux-noble
```

`Makefile` 只有几行，真正的编译规则在内核的构建系统（Kbuild）里：

```makefile
obj-m := hello.o                            # 把 hello.c 编成模块 hello.ko
KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules    # 进入内核头文件目录，编译 M= 指定目录下的模块
```

### 你会看到的三条警告（都正常）

| 现象 | 意思 |
|---|---|
| 编译时 `warning: the compiler differs from the one used to build the kernel` | 内核是用 gcc 13.2 编的，板子上的 gcc 是 13.3，小版本差异不影响 |
| 加载时 `module verification failed: signature and/or required key missing - tainting kernel` | 这个内核会检查模块签名，但不强制（`CONFIG_MODULE_SIG_FORCE` 没开），所以没签名的模块照样能加载，只是给内核打上 tainted（被污染）标记，方便出问题时区分是不是第三方模块引起的 |
| `loading out-of-tree module taints kernel` | 同上，NVIDIA 自己的模块也会触发，开机日志里就有 |

## 六、内核编程的规矩（和单片机最不一样的地方）

1. **没有标准 C 库**：没有 `printf` `malloc` `sleep`，用内核版本：`pr_info()` `kmalloc()` `msleep()`。
2. **不能用浮点数**：内核不保存浮点寄存器。传感器换算用整数，比如温度用「毫摄氏度」表示。
3. **栈很小**：arm64 上每个线程的内核栈只有 16 KB，大数组不要放在局部变量里。
4. **分清上下文**，这是最容易出错的：
   - **进程上下文**：用户调用 `read()` 进来、`probe()` 里，**可以睡眠**（`msleep`、加互斥锁、用 `GFP_KERNEL` 分配内存）。
   - **中断上下文**：中断处理函数、定时器回调，**绝对不能睡眠**，否则内核直接报错。本章的定时器回调里打印 `in_interrupt=1`，就是在提醒你这一点。
5. **处处是并发**：这颗 SoC 有 6 个 CPU 核，内核还可以抢占。两个进程可能同时进入你的 `write()`，中断也可能随时打断。共享数据必须加锁。
6. **错误码是负数**：成功返回 0，失败返回 `-EINVAL`、`-ENOMEM`、`-EBUSY` 这类负的 errno。用户程序里 `errno` 看到的就是它的正数。
7. **申请的资源都要释放**：模块卸载时，定时器、内存、中断都要还给内核。本章的 `hello_exit()` 里如果忘了 `timer_delete_sync()`，模块卸载后定时器还会去调用已经不存在的函数，内核会崩。后面的章节会用 `devm_` 开头的函数，由内核自动释放。

## 七、调试手段

| 工具 | 用来做什么 |
|---|---|
| `dmesg`、`pr_info()` / `dev_info()` | 最常用：在驱动里打印，在 dmesg 里看 |
| `/sys/module/<模块>/parameters/` | 读写模块参数 |
| `/sys/bus/<总线>/devices/`、`drivers/` | 看设备和驱动有没有配对上 |
| `/sys/kernel/debug/`（debugfs） | 内核各子系统的调试信息，比如 `gpio`、`pwm`、`pinctrl` |
| `/proc/device-tree/` | 当前生效的设备树 |
| 动态调试 `dynamic_debug` | 运行中打开某个文件里的 `pr_debug()` 输出 |
| `ftrace`（`/sys/kernel/tracing/`） | 跟踪内核函数调用、中断时序 |

**内核 oops** 时 dmesg 会打出一大段调用栈，最关键的是第一行（出了什么错，比如 `Unable to handle kernel NULL pointer dereference`）和 `pc :` 那一行（在哪个函数的第几个字节出的错）。

## 八、这块板子上的内核

| 项目 | 值 |
|---|---|
| 内核版本 | `6.8.12-1029-tegra`，基于 Ubuntu 24.04 的 6.8 内核，加上 NVIDIA 的补丁 |
| NVIDIA 驱动 | 大部分以「树外模块」（out-of-tree）的形式放在 `/lib/modules/$(uname -r)/updates/` |
| 时钟节拍 | `HZ=250`，也就是 1 个 jiffy 是 4 ms，内核定时器的精度就是 4 ms |
| 启动流程 | UEFI → 读 `/boot/extlinux/extlinux.conf` → 加载 `/boot/Image`（内核）、`/boot/initrd` 和设备树 → 启动内核 |
| 内核配置 | `zcat /proc/config.gz` 能看到完整配置 |

## 九、动手：第一个模块

代码在 [`hello/hello.c`](hello/hello.c)，功能：
- 加载时打印 `Hello, <name>!`；
- 每隔 `period_ms` 毫秒触发一次定时器，`ticks` 加 1；
- 卸载时打印 `Goodbye`；
- `period_ms` 小于 10 时拒绝加载。

```bash
cd ~/jetson-drivers/00-kernel-basics/hello
make
sudo insmod hello.ko name=Minghe period_ms=200
sudo dmesg | tail -5
cat /sys/module/hello/parameters/ticks
sudo rmmod hello
```

我的实测输出：

```
hello: Hello, Minghe! HZ=250
hello: tick 1, in_interrupt=1, jiffies=4296162952
hello: tick 2, in_interrupt=1, jiffies=4296163003
...
hello: Goodbye, Minghe! ticks=8
```

相邻两次 jiffies 差 51，也就是 204 ms，不是正好 200 ms。原因是 200 ms 换算成 jiffies 后向上取整，再加上定时器在下一个节拍才触发。

### 测试

```bash
./test.sh
```

[`test.sh`](hello/test.sh) 里有 5 个 TODO，按注释把命令补上，8 项全部 PASS 就完成了。

## 十、进阶练习（可选）

1. 把 `period_ms` 改成 1，观察 jiffies 的间隔。为什么不是 1 ms？（提示：`HZ`）
2. 在定时器回调里调用 `msleep(1)`，加载后看 dmesg 报什么错。这就是「中断上下文不能睡眠」。看完记得改回来。
3. 把定时器换成高精度定时器 `hrtimer`，再看间隔能精确到多少。

## 本章要记住的

1. 应用程序不能碰硬件，要通过驱动；驱动在内核里，崩了会拖垮整个系统。
2. 驱动大多是「填框架的回调」，不是从零写。
3. 先分清自己在进程上下文还是中断上下文，再决定能不能睡眠。
