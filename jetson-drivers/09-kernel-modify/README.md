# 第 9 章：修改并重新编译内核

> 目标：往内核源码里加一个驱动，打开两个新选项，本机编译和交叉编译各走一遍，装上新内核，还能随时退回旧内核。

前面 8 章写的都是**模块**：编好了 `insmod`，不想要就 `rmmod`，内核本身一个字节都没动。
这一章动内核本身。具体做四件事：

1. 往内核源码里加一个**编进内核**的驱动 `tutorial_hello`，包括 Kconfig 选项和 Makefile 规则，并生成一个标准的补丁文件；
2. 打开两个原来没开的选项：`GPIO_SIM` 和 `I2C_STUB`。第 3、5 章为了绕开这件事，是在源码树外面编的。
   其中 `GPIO_SIM` 依赖的 `IRQ_SIM` **只能编进内核**，所以不重新编译内核，这个选项就打不开；
3. 两种编法都走一遍：在板子上本机编译，以及在 4080 的 WSL 上交叉编译；
4. **并存安装**：旧内核原封不动，开机菜单里随时能选回去。另外提供一键回退。

> 风险提示：这一章是整个教程里唯一可能让板子**起不来**的一章（出问题时需要显示器加键盘才能选回旧内核）。
> 按顺序做，`check_abi.sh` 没通过绝对不要装。最坏情况见文末「起不来怎么办」。

---

## 1. 什么时候必须改内核，什么时候写模块就行

| 需求 | 写模块 | 改内核 |
|---|---|---|
| 给新硬件写驱动 | ✓ | |
| 用到的内核选项只能编进内核（bool 类型，如 `IRQ_SIM`） | | ✓ |
| 开机很早就要用的驱动（根文件系统所在的存储、早期控制台） | | ✓（或放进 initrd） |
| 修改内核核心代码：调度、内存管理、改某个结构体 | | ✓ |
| 修掉内核里已有驱动的 bug | 有时可以（单独编那个模块） | 编进内核的驱动只能改内核 |

驱动开发的日常工作里九成是模块。但下面几样东西得会：看懂源码、改配置、编译、换内核、出问题能回退。
厂商发了 BSP 补丁、要打开某个功能、要追一个内核 bug，都离不开这些。

## 2. 内核源码树导览

板子上的源码在 `~/kernel/Linux_for_Tegra/source/kernel/kernel-noble`。这是 Ubuntu 24.04（noble）的 6.8 内核，加上 NVIDIA 的 tegra 补丁。

```
arch/arm64/        架构相关：启动代码、异常、内存映射；arch/arm64/boot/Image 是编出来的内核镜像
drivers/           驱动，占了源码的一大半。drivers/gpio、drivers/i2c、drivers/spi …… 前几章用到的子系统都在这
include/linux/     内核头文件。前几章 #include <linux/xxx.h> 就是这里
kernel/            核心：调度（sched/）、中断（irq/，IRQ_SIM 就在 kernel/irq/irq_sim.c）、定时器、模块加载
mm/  fs/  net/     内存管理、文件系统、网络协议栈
scripts/           编译用的工具：Kconfig 解析器、modpost、scripts/config（命令行改 .config）
Documentation/     文档。很多子系统的驱动写法都在 Documentation/driver-api/
debian*/           Ubuntu 打包用的文件，我们不用
```

不知道某个功能在哪个文件里，按下面的顺序找：

```bash
cd ~/kernel/Linux_for_Tegra/source/kernel/kernel-noble
grep -rn "config GPIO_SIM" --include=Kconfig* .     # 选项定义在哪
grep -n "GPIO_SIM" drivers/gpio/Makefile             # 对应哪个 .c
```

## 3. Kconfig 和 Kbuild：内核怎么决定编哪些文件

两套东西配合工作。

**Kconfig** 定义有哪些选项。每个目录有一个 `Kconfig` 文件，比如 `drivers/gpio/Kconfig` 里：

```
config GPIO_SIM
	tristate "GPIO Simulator Module"
	depends on CONFIGFS_FS
	select IRQ_SIM
	select IRQ_DOMAIN_HIERARCHY
```

- `bool`：只能是 `y`（编进内核）或 `n`；`tristate`：还可以是 `m`（编成模块）；
- `depends on`：依赖条件不满足时，这个选项根本不出现；
- `select`：打开我时，**强制**打开它。`IRQ_SIM` 是 bool，所以 `GPIO_SIM=m` 也会把 `IRQ_SIM` 变成 `y`，这就是必须换内核的原因。

所有选项的取值存在编译目录的 `.config` 文件里，一行一个：`CONFIG_GPIO_SIM=m`。

**Kbuild** 决定编哪些文件。每个目录的 `Makefile` 里有这样的规则：

```make
obj-$(CONFIG_GPIO_SIM) += gpio-sim.o
```

`CONFIG_GPIO_SIM=y` 时这一行变成 `obj-y`，这个文件就编进内核；`=m` 时变成 `obj-m`，编成模块；没设置时变成 `obj-`，就不编。前几章自己写的 Makefile 里 `obj-m += xxx.o`，用的也是这套规则。

**改配置的几种方法：**

| 方法 | 用途 |
|---|---|
| `make menuconfig` | 文本菜单，按 `/` 搜索选项，能看到依赖关系。最适合探索 |
| `scripts/config --enable X --module Y --disable Z` | 命令行修改，适合写进脚本（本章用的就是它） |
| `make olddefconfig` | 改完之后**必须**跑：补齐新出现选项的默认值，并处理 select 和 depends |
| `make savedefconfig` | 生成精简版配置（只含和默认值不同的项），适合存档 |

**为什么从 `/proc/config.gz` 开始：** 正在运行的内核把自己的配置也编了进去（`CONFIG_IKCONFIG_PROC=y`）。
以它为起点，只改需要的几项，能保证其它几千个选项和现在完全一样。如果用 `arch/arm64/configs/defconfig` 当起点，得到的是另一个内核。

## 4. 两种换内核的思路

**思路 A：完整构建（NVIDIA 官方流程）。** 用 `source/nvbuild.sh` 编内核、全部几千个模块和 NVIDIA 的树外模块（`nvidia-oot`、`nvgpu`……），再整套装进去。
在 Orin Nano 上要好几个小时。如果改动影响了内核的 ABI，比如改了公共结构体，只能走这条路。

**思路 B：只换内核镜像（本章）。** 版本号保持和现在一样，只编 `Image`，加上这次新打开的两个模块。
`/lib/modules/6.8.12-1029-tegra/` 下已有的所有模块（包括 NVIDIA 的 GPU、显示、Wi-Fi 驱动）原样复用。
能这样做是有前提的：

- **vermagic 一致**：模块里记录了编译时的内核版本和关键选项（SMP、PREEMPT、modversions），加载时必须完全匹配；
- **符号校验和一致**（`CONFIG_MODVERSIONS=y`）：模块调用的每个内核函数，在编译时都记了一个 CRC。
  这个 CRC 由函数原型以及参数里**所有结构体的完整定义**算出来。结构体多一个字段，CRC 就变了，旧模块会拒绝加载（`disagrees about version of symbol`）。

我们只**新增**了东西：一个新驱动、一个新模块和它依赖的 `IRQ_SIM`，没有改任何已有的结构体，所以这两条都满足。
`check_abi.sh` 会把旧内核导出的 15491 个符号的 CRC，和新内核的逐个对比，用数据证明这一点。
实测结果：一个都没变，新内核只多了 3 个（`irq_domain_create_sim` 等，来自 `IRQ_SIM`）。

> 练习 4 会故意破坏 ABI，看 `check_abi.sh` 能不能抓到。

## 5. Jetson 怎么启动内核

```
上电 → BootROM → MB1/MB2（NVIDIA 固件）→ UEFI → L4TLauncher
     → 读 /boot/extlinux/extlinux.conf → 按 DEFAULT 选一个 LABEL
     → 加载 LINUX（内核镜像）+ INITRD + FDT（设备树）+ OVERLAYS（第 2 章的覆盖层）
     → 用 APPEND 行做内核命令行，启动内核
```

`extlinux.conf` 里可以有多个 `LABEL`，开机时 UEFI 会显示 30 秒（`TIMEOUT 30`）的菜单，接了显示器和键盘就能选。
**并存安装**就是新增一个 LABEL 指向新镜像，旧的 LABEL 一个不删。安装后的启动项：

| LABEL | 内核 | 设备树 | 用途 |
|---|---|---|---|
| `primary` | `/boot/Image`（原装） | 原装 | 出厂状态，最后的退路 |
| `tutorial` | `/boot/Image`（原装） | 原装 + 教程覆盖层 | 第 2～8 章用的 |
| `tutorial-kernel`（新，默认） | `/boot/Image-tutorial` | 原装 + 教程覆盖层 | 本章 |

## 6. 准备

### 拿到源码

**板子上**已经有了（`~/kernel/Linux_for_Tegra/source/`）。从头做的话，到 [Jetson Linux 下载页](https://developer.nvidia.com/embedded/jetson-linux)找到和 `cat /etc/nv_tegra_release` 版本一致的 **Driver Package Sources**（`public_sources.tbz2`，约 300 MB），然后：

```bash
mkdir -p ~/kernel && cd ~/kernel
tar xjf ~/下载目录/public_sources.tbz2 Linux_for_Tegra/source/kernel_src.tbz2   # 只取内核那一包
cd Linux_for_Tegra/source && tar xjf kernel_src.tbz2                          # 得到 kernel/kernel-noble
```

**WSL 上**做法一样。另外还需要 NVIDIA 的交叉编译工具链：同一个下载页里的 **Bootlin Toolchain gcc 13.2**，解压到 `~/l4t-gcc/`。
官方内核就是用它编的，`cat /proc/version` 里能看到 `aarch64-none-linux-gnu-gcc (crosstool-NG 1.26.0) 13.2.0`。

### 编译依赖

```bash
sudo apt install build-essential flex bison bc libssl-dev libelf-dev kmod cpio rsync
```

板子上都已经装好。WSL 上缺 `flex`、`bison`、`libelf-dev`，要自己装一次。

### 空间和时间

| | 板子（6 核 A78AE） | 4080 的 WSL（16 线程，用 8 个） |
|---|---|---|
| 源码 | 1.6 GB（工作副本用硬链接，不额外占） | 同左 |
| 编译输出 | 0.9 GB（关了调试信息） | 同左 |
| 编 Image（实测） | 16.5 分钟（`-j6`） | 5 分 19 秒（`-j8`，`nice -n 19`） |

实测（2026-11-28），两种编法都完整走了一遍：编译、`check_abi.sh`、安装、重启、`test.sh` 全过（本机编译那次也测了命令行参数练习，15 项），
`uninstall.sh` 回退后 `extlinux.conf` 和安装前逐字节相同。

## 7. 动手：本机编译（在板子上）

```bash
cd ~/jd/jetson-drivers/09-kernel-modify     # 你放教程的位置
./prepare.sh          # 建工作副本 ~/kwork/kernel-noble，打补丁，生成配置
./build.sh            # 编 Image + gpio-sim.ko + i2c-stub.ko
./check_abi.sh        # 确认旧模块能用。不通过就别往下走
./install.sh          # 并存安装，新启动项设为默认
sudo reboot
./test.sh             # 重启之后
```

**`prepare.sh` 做了什么**（建议打开脚本对照着看）：

1. `cp -al` 建一个**硬链接**副本。1.6 GB 几秒就建好，几乎不占空间。
   `patch` 和 `sed -i` 修改文件时都是写一个新文件再改名，所以原始源码不受影响，随时能对比改了什么；
2. `patch -p1 < patches/0001-misc-add-tutorial_hello.patch`：给源码打补丁。`-p1` 表示去掉路径里的第一层 `a/`、`b/`；
3. 从 `/proc/config.gz` 取配置，用 `scripts/config` 改 4 处，然后 `make olddefconfig`；
4. 打印和正在运行的内核的配置差异。应该只看到下面这些：

```
> CONFIG_GPIO_SIM=m
> CONFIG_I2C_STUB=m
> CONFIG_IRQ_SIM=y                    ← 被 GPIO_SIM 的 select 拉进来的
> CONFIG_TUTORIAL_HELLO=y
< CONFIG_DEBUG_INFO=y ...             ← 关掉调试信息：编译快一倍、输出小几 GB，不影响 ABI
< CONFIG_GCC_VERSION=130200 / > 130300   ← 板子上的 gcc 是 13.3，官方用 13.2，不影响 ABI
< CONFIG_GCC_PLUGINS=y                ← 板子没装 gcc 插件开发包，所以这项自动关了。
                                        没有任何插件真正启用（STACKLEAK、RANDSTRUCT 都是关的），不影响
```

5. 检查 `make kernelrelease` 是否等于 `uname -r`。版本号来自命令行的 `LOCALVERSION=-1029-tegra`，不是 `.config` 里的 `CONFIG_LOCALVERSION`。NVIDIA 的 `kernel/Makefile` 就是这么传的。

**补丁里有什么**（`patches/0001-misc-add-tutorial_hello.patch`，一共三处改动）：

```diff
--- a/drivers/misc/Kconfig
+config TUTORIAL_HELLO
+	bool "jetson-drivers tutorial: built-in hello driver"
+	depends on PROC_FS
--- a/drivers/misc/Makefile
+obj-$(CONFIG_TUTORIAL_HELLO)	+= tutorial_hello.o
--- /dev/null
+++ b/drivers/misc/tutorial_hello.c        （新文件）
```

`tutorial_hello.c` 和第 0 章的 hello 模块几乎一样，区别在于它编进了内核：

- 没有「加载」这一步，初始化函数由 `late_initcall()` 在开机时调用。内核按级别依次调用各个初始化函数：
  core → postcore → arch → subsys → fs → device → late。`module_init()` 编进内核时属于 device 级；
- 没有退出函数，因为编进内核的驱动永远不会被卸载；
- `module_param()` 照样能用：参数写在**内核命令行**里，格式是 `tutorial_hello.greeting=xxx`，运行时在 `/sys/module/tutorial_hello/parameters/` 能看到。

**`build.sh`**：`nice -n 19 make -j6 Image`，然后单独编两个模块（`make drivers/gpio/gpio-sim.ko`，Kbuild 支持只编某一个目标）。
编完后 `~/kwork/build/vmlinux.symvers` 里有新内核导出的全部符号和 CRC。

**`install.sh`**：先自动跑一遍 `check_abi.sh`，然后：

- 拷 `/boot/Image-tutorial`；
- 拷两个模块到 `/lib/modules/<版本>/updates/tutorial/` 并 `depmod`。注意：在旧内核下 `modprobe gpio-sim` 会报 `Unknown symbol`，因为旧内核里没有 `IRQ_SIM`，这是正常的；
- 复制当前默认的启动项，只换掉 `LINUX` 一行，加为 `tutorial-kernel` 并设成默认。原配置备份在 `extlinux.conf.before-kernel`。

## 8. 动手：交叉编译（在 4080 的 WSL 上）

脚本是同一套：检测到不是 aarch64，就自动加上 `ARCH=arm64 CROSS_COMPILE=...`，配置和版本号通过 ssh 从板子上取。
```bash
bash                  # WSL 的登录 shell 是 fish，脚本要在 bash 里跑
git clone https://github.com/sayakawaii/daily-knowledge.git ~/daily-knowledge
cd ~/daily-knowledge/jetson-drivers/09-kernel-modify
ssh jetson-home uname -r      # 先确认能免密登录板子；别名不同就用 TARGET=别名 ./prepare.sh
./prepare.sh          # 源码默认在 ~/kernel/Linux_for_Tegra/source/kernel/kernel-noble，配置从板子上取
./build.sh            # 默认 -j8 加 nice -n 19，打游戏时编译会自动让出 CPU。想更省：J=4 ./build.sh
./check_abi.sh        # 通过 ssh 取板子上的 Module.symvers 来对比
./install.sh          # scp 到板子的 ~/kinstall/，再 ssh 过去安装
ssh jetson-home sudo reboot
```

和本机编译的区别：

- 交叉编译器就是官方用的 gcc 13.2，实测配置差异里只剩自己改的那几项，连编译器版本号、`GCC_PLUGINS` 都和官方一样；
  `/proc/version` 里会显示 `aarch64-none-linux-gnu-gcc (crosstool-NG 1.26.0) 13.2.0`，只有编译者和时间不同；
- `modinfo` 在 x86 上也能读 arm64 的 `.ko`，所以 `check_abi.sh` 照样能跑；
- 交叉编译出来的模块和 Image **只能拷到板子上用**，不能在 WSL 里 `insmod`。

## 9. 测试

重启后 `./test.sh`。里面有 4 个 TODO 要你自己补，提示写在注释里。测的内容：

- 正在跑的确实是新内核（`/proc/config.gz` 里有 `CONFIG_TUTORIAL_HELLO=y`）；
- 编进内核的驱动工作正常（`/proc/tutorial_hello`、`/sys/module/.../parameters`、开机日志）；
- 新打开的选项能用：**内核自带的** `gpio-sim` 和 `i2c-stub` 能用 `modprobe` 加载，不再需要 `tools/` 里在源码树外编的版本；
- 旧模块照常工作：NVIDIA GPU 驱动 `nvgpu`、以太网、Wi-Fi 都在，说明 ABI 确实兼容。

## 10. 回退

```bash
./uninstall.sh && sudo reboot
```

删除 `Image-tutorial` 和 `updates/tutorial/`，删除启动项，把 `DEFAULT` 改回安装前的值（`tutorial`）。

## 11. 起不来怎么办

1. **接显示器（DP 口）和 USB 键盘**，开机时 UEFI 会显示启动菜单（30 秒），选 `tutorial` 或 `primary`。
   进系统后跑 `./uninstall.sh`；
2. **看崩溃日志**：内核命令行里有 `pstore.backend=ramoops`，内核 panic 时会把日志存到一块重启不清空的内存。
   用旧内核起来后看 `/sys/fs/pstore/`，里面就是新内核死之前的最后输出；
3. **串口控制台**：命令行里有 `console=ttyTCU0,115200`，内核日志同时输出到调试串口。
   串口的引脚位置要查 NVIDIA 的载板手册（Carrier Board Specification）。用第 7 章买的 USB-TTL 模块（3.3 V）连上，从最早的启动阶段开始都能看到日志；
4. 实在不行就重新烧录系统（SDK Manager 或 SD 卡镜像）。所以重要的东西要提前推到 GitHub。

## 12. 练习

1. **命令行参数**：编辑 `/boot/extlinux/extlinux.conf`，在 `tutorial-kernel` 那一块的 `APPEND` 行末尾加 `tutorial_hello.greeting=你的名字`。
   重启后 `cat /proc/tutorial_hello`，`test.sh` 会自动检查这一项；
2. **改驱动再编**：给 `/proc/tutorial_hello` 加一行，显示开机以来调用了多少次 `show`。
   改的是 `~/kwork/kernel-noble/drivers/misc/tutorial_hello.c`，然后 `./build.sh && ./install.sh`。第二次编译比第一次快得多：Kbuild 只重编改过的文件，再重新链接内核。
   改完用下面的命令重新生成补丁：
   `diff -uN --label a/drivers/misc/tutorial_hello.c --label b/drivers/misc/tutorial_hello.c /dev/null ~/kwork/kernel-noble/drivers/misc/tutorial_hello.c`
3. **menuconfig**：`make -C ~/kwork/kernel-noble O=~/kwork/build menuconfig`，按 `/` 搜 `TUTORIAL_HELLO`，看它在菜单里的位置和依赖。把它改成 `n` 再存盘，`.config` 会怎么变？练完改回 `y`；
4. **故意破坏 ABI（只编不装！）**：在 `include/linux/i2c.h` 的 `struct i2c_client` 末尾加一个 `int tutorial_dummy;`，再跑 `./build.sh && ./check_abi.sh`。
   看看哪些符号的 CRC 变了。想一想：如果这时硬装上去，旧的 `at24`、`nvgpu` 加载时会发生什么？练完把这一行删掉，重新编译；
5. **思考题**：为什么 `GPIO_SIM` 可以是模块，而它 select 的 `IRQ_SIM` 只能是 bool？
   提示：去看 `kernel/irq/Kconfig`，再想想 `irq_domain_create_sim` 是被谁调用的。

## 本章文件

```
patches/0001-misc-add-tutorial_hello.patch   给内核源码的补丁（Kconfig + Makefile + 新驱动）
env.sh          公共变量；自动区分本机编译和交叉编译
prepare.sh      建工作副本、打补丁、生成配置
build.sh        编 Image 和两个模块
check_abi.sh    对比符号校验和，装之前必做
install.sh      并存安装
uninstall.sh    回退
test.sh         重启后测试（4 个 TODO）
```
