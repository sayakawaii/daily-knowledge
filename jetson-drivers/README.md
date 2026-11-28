# Jetson Orin Nano 驱动开发教程

写给**会单片机、没写过 Linux 驱动**的人。每个外部接口一章，每章都有：

- 原理：硬件怎么工作，Linux 里对应哪个子系统；
- **能编译运行的完整驱动代码**（在板子上实际编译、加载、测过）；
- 测试脚本 `test.sh`：需要你掌握的部分**留空**，用 `TODO` 注释提示。补完后跑通就算学会了；
- **不买硬件也能测**：用内核的模拟器（gpio-sim、i2c-stub）、虚拟控制器、芯片内部回环。买了零件以后，还有接真硬件的测试（`test_jumper.sh` 等）。

开发板：Jetson Orin Nano Super 开发者套件（p3768-0000 载板 + p3767-0005 模组），L4T R39.2.2，内核 `6.8.12-1029-tegra`。

---

## 外部接口清单

在板子上查过（`lspci`、`lsusb -t`、`/sys/class/drm`、NVIDIA 的 40 针定义 `/boot/tegra234-p3767-0000+p3509-a02-hdr40.dtbo`）：

| 接口 | 在板子上是什么 | Linux 子系统 | 章节 |
|---|---|---|---|
| 40 针 GPIO | 默认就是 GPIO 的 5 个脚（15/29/31/32/33），其它功能脚也能改成 GPIO | gpiolib、中断 | [第 3 章](03-gpio/) |
| 40 针 PWM | 15/32/33 脚可切成 PWM | PWM | [第 4 章](04-pwm-fan/) |
| 风扇接口（4 针） | PWM 调速 + 转速计 | PWM、hwmon、thermal | [第 4 章](04-pwm-fan/) |
| 40 针 I2C ×2 | 3/5 脚（i2c-7）、27/28 脚（i2c-1） | I2C、hwmon | [第 5 章](05-i2c/) |
| 板载 I2C 设备 | 模组和载板的 EEPROM（i2c-0） | I2C、nvmem | [第 5 章](05-i2c/) |
| 40 针 SPI ×2 | SPI1：19/21/23/24/26 脚；SPI3：13/16/18/22/37 脚 | SPI、IIO | [第 6 章](06-spi/) |
| 40 针 UART | 8/10 脚（TX/RX），11/36 脚（RTS/CTS），`/dev/ttyTHS1` | tty、serdev | [第 7 章](07-uart/) |
| CAN | 片上 mttcan 控制器 `can0`（需要外接收发器） | SocketCAN、netdev | [第 8 章](08-can/) |
| 40 针 I2S | 12/35/38/40 脚，7 脚是音频主时钟 | ALSA ASoC | 未覆盖（见下方路线） |
| USB 3.2 Type-A ×4 | tegra-xusb 主控，板上有一个 4 口集线器 | USB host | [第 10 章](10-usb/) |
| USB-C | 设备模式（`usb0`/`usb1` 虚拟网卡、`ttyGS0` 串口）+ 烧录 | USB gadget | [第 10 章](10-usb/)（configfs gadget） |
| 千兆网口 | RTL8111（走 PCIe） | netdev、PCIe | [第 11 章](11-pcie/) |
| M.2 Key M ×2 | PCIe，现在插着 NVMe 固态硬盘 | PCIe、NVMe | [第 11 章](11-pcie/) 讲拓扑，不碰系统盘 |
| M.2 Key E | PCIe，现在插着 RTL8822CE Wi-Fi，蓝牙走 USB | PCIe、USB | [第 11 章](11-pcie/) 讲拓扑 |
| MIPI CSI-2 ×2（22 针） | 摄像头 | V4L2、media controller | [第 12 章](12-camera/) |
| DisplayPort | 显示输出 | DRM（NVIDIA 驱动，不开源部分多） | 不打算写 |
| 12 针按键接口 | 电源、复位、恢复模式按键 | — | 只在第 9 章「起不来怎么办」里用到 |

## 40 针排针

所有引脚都是 **3.3V** 电平。下表的「本教程用法」是第 2 章覆盖层设置的，没装覆盖层时是出厂默认。

| 功能 | 脚 | 脚 | 功能 |
|---|---|---|---|
| 3.3V | 1 | 2 | 5V |
| I2C（i2c-7）SDA | 3 | 4 | 5V |
| I2C（i2c-7）SCL | 5 | 6 | GND |
| 音频 MCLK | 7 | 8 | UART TX（ttyTHS1） |
| GND | 9 | 10 | UART RX（ttyTHS1） |
| UART RTS | 11 | 12 | I2S SCLK |
| SPI3 SCK | 13 | 14 | GND |
| GPIO PN1 → **PWM**（第 4 章） | 15 | 16 | SPI3 CS1 |
| 3.3V | 17 | 18 | SPI3 CS0 |
| SPI1 MOSI（spidev0） | 19 | 20 | GND |
| SPI1 MISO | 21 | 22 | SPI3 MISO |
| SPI1 SCK | 23 | 24 | SPI1 CS0 |
| GND | 25 | 26 | SPI1 CS1 |
| I2C（i2c-1）SDA | 27 | 28 | I2C（i2c-1）SCL |
| GPIO PQ5 → **LED**（第 3 章） | 29 | 30 | GND |
| GPIO PQ6 → **按键**（第 3 章） | 31 | 32 | GPIO PG6 → **PWM**（第 4 章） |
| GPIO PH0 → **PWM**（第 4 章） | 33 | 34 | GND |
| I2S FS | 35 | 36 | UART CTS |
| SPI3 MOSI | 37 | 38 | I2S DIN |
| GND | 39 | 40 | I2S DOUT |

i2c-1 总线上，载板已经接了 fusb301（0x25）和 INA3221 电源监测（0x40）。外接设备别和它们的地址冲突。

## 购物清单（第一批，约 50～80 元）

不买也能学完全部章节（都有模拟测试）。买了就能接真硬件，跑 `test_jumper.sh` 这类测试。

| 零件 | 用在 | 参考价 | 注意 |
|---|---|---|---|
| 杜邦线：母对母、公对母各一排（40 根） | 全部 | 5 元 | |
| 面包板（400 孔） | 全部 | 5 元 | |
| LED 若干，电阻包（330 Ω、1 kΩ、10 kΩ） | 第 3 章 | 5 元 | LED **必须串电阻**；31 脚按键要外接 10 kΩ 上拉（第 3 章解释了原因） |
| 轻触按键 | 第 3 章 | 1 元 | |
| LM75A 温度传感器模块 | 第 5 章 | 4 元 | 3.3V 供电 |
| ADXL345 加速度计模块 | 第 6 章 | 10 元 | 选引出 SPI 引脚（SDO、CS）的版本 |
| USB-TTL 模块（CH340 或 CP2102） | 第 7、9 章 | 8 元 | **跳线帽切到 3.3V**；绝不能接电脑的 RS-232 DB9 口 |
| SN65HVD230 CAN 收发器模块 ×2 | 第 8 章 | 10 元 | 3.3V 的，**不要买 5V 的 TJA1050** |
| （可选）USB-CAN 适配器 | 第 8 章 | 50 元起 | CAN 总线上至少要两个节点。也可以用一块带 CAN 的单片机当第二个节点 |
| （可选）PCA9685 16 路 PWM 模块 | 进阶练习 | 10 元 | I2C 接口，可以练习写 PWM 控制器驱动（提供 PWM 的一方） |

第二批（第 10～12 章）同样不买也能学完：USB 用内核的虚拟主控 dummy_hcd，PCIe 借板载网卡芯片，摄像头用虚拟 V4L2 驱动。想接真硬件：

| 零件 | 用在 | 参考价 | 注意 |
|---|---|---|---|
| USB-C 转 USB-A 数据线 | 第 10 章 `test_cable.sh` | 10 元 | 要**数据线**，只能充电的线不行。板子自己的 USB-C 口插到自己的 USB-A 口上 |
| 网线 | 第 11 章练习 | 5 元 | 插路由器，看链路状态变化 |
| IMX219 摄像头模组 | 第 12 章 | 几十元起 | 板子上是 22 针座。树莓派原版模组是 15 针，要配「15 转 22 针」排线；Jetson 专用的模组一般自带 |

## 学习路线

| 章 | 内容 | 要接硬件吗 |
|---|---|---|
| [0](00-kernel-basics/) | 内核是什么、第一个模块、日志、参数 | 不用 |
| [1](01-char-device/) | 字符设备：read/write/ioctl/poll、阻塞 | 不用 |
| [2](02-device-tree/) | 设备树、覆盖层、平台驱动；**装教程覆盖层**（后面各章都依赖它） | 不用，但要重启 |
| [3](03-gpio/) | GPIO、中断、去抖、sysfs 通知 | 可选：LED + 按键 |
| [4](04-pwm-fan/) | PWM、风扇 hwmon 驱动、转速计、温度保护 | 风扇就是现成的硬件 |
| [5](05-i2c/) | I2C：读板载 EEPROM、LM75A 驱动 | 可选：LM75A |
| [6](06-spi/) | SPI 设备驱动、虚拟 SPI 控制器、IIO、spidev | 可选：ADXL345，或一根跳线 |
| [7](07-uart/) | termios、内部回环、serdev 驱动 | 可选：USB-TTL |
| [8](08-can/) | SocketCAN、虚拟 CAN 网卡驱动、mttcan 回环 | 可选：收发器 + 第二个节点 |
| [9](09-kernel-modify/) | 改内核：Kconfig、编进内核的驱动、本机/交叉编译、并存安装、回退 | 不用，但要重启 |
| [10](10-usb/) | USB：描述符、URB、主机侧驱动、configfs gadget、dummy_hcd 虚拟主控 | 不用；可选一根 USB-C 转 A 数据线 |
| [11](11-pcie/) | PCIe：配置空间、BAR、INTx/MSI、DMA 与 SMMU、驱动绑定（借板载网卡芯片） | 不用 |
| [12](12-camera/) | V4L2、videobuf2、虚拟摄像头、采集程序、media controller、CSI 摄像头 | 不用；可选 IMX219 |

第 0～2 章是基础，按顺序学。3～8 章和 10～12 章互相独立，可以按兴趣挑。第 9 章建议放到最后（第 10 章的练习会用到它）。

I2S 音频（ASoC）没有覆盖，如果需要可以再加。

## 开始之前

```bash
# 1. 把教程放到板子上（任选一种）
git clone https://github.com/sayakawaii/daily-knowledge.git && cd daily-knowledge/jetson-drivers

# 2. 编译依赖（JetPack 自带的系统一般都有，缺什么装什么）
sudo apt install build-essential nvidia-l4t-kernel-headers i2c-tools can-utils gpiod

# 3. 第 2 章：编译并安装教程覆盖层，重启（第 3～8 章的硬件节点都在里面）
# 4. 模拟器：第 3 章用 tools/gpio-sim，第 5 章用 tools/i2c-stub，第 10 章用 tools/usb-sim，各章 README 里写了怎么编
```

每章的用法都一样：

```bash
cd 03-gpio/tgpio
make            # 编驱动和测试程序
./test.sh       # 先直接跑：TODO 没补的地方会 FAIL，这是正常的
# 打开 test.sh，按注释补完 TODO，再跑，直到「全部通过！」
```

## 安全须知

- **排针是 3.3V**：不要把 5V 信号接到任何 GPIO、I2C、SPI、UART 引脚上；1、17 脚（3.3V）和 2、4 脚（5V）不要短路到 GND。
- **不要读没开时钟的外设寄存器**：在 Jetson 上通过 `/dev/mem` 读一个时钟关着的外设，总线会报超时错误（CBB TIMEOUT），之后**这个外设一直不可用，直到重启**。第 4 章踩过这个坑。`tools/regpeek` 只读，但也只该在外设已启用时用。
- **风扇**：第 4 章的测试会接管风扇。脚本退出时会自动恢复系统的风扇控制；如果中途被强行杀掉，运行 `sudo systemctl restart nvfancontrol`。
- **改启动配置前先备份**：第 2、9 章都会改 `/boot/extlinux/extlinux.conf`。脚本会自动备份并保留原启动项，开机菜单（接 DP 显示器和键盘）随时能选回去。
- 驱动出 bug 导致内核崩溃（oops/panic）是正常的学习过程。最坏情况就是重启。**重要的修改要随时提交到 git。**

## 目录

```
00-kernel-basics/ … 12-camera/   各章
tools/usb-sim/      在源码树外编译 dummy_hcd 虚拟 USB 主控和 loopback gadget 功能（第 10 章）
tools/gpio-sim/     在源码树外编译内核自带的 gpio-sim 模拟器（第 3 章）
tools/i2c-stub/     在源码树外编译 i2c-stub 模拟器（第 5 章）
tools/regpeek/      只读的寄存器查看工具（第 4 章）
```
