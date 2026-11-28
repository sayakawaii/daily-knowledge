# 第 2 章：设备树与平台驱动

> 目标：看懂设备树，把教程覆盖层装到板子上，然后写一个平台驱动，让它在设备树节点出现时被自动调用。
> 第 3 章到第 7 章的驱动都要用这一章装上的覆盖层，**这一章必须先做完**。
> 本章不需要外接硬件。

## 一、为什么要有设备树

单片机上，「LED 接在哪个脚、串口基地址是多少」都写在代码里：

```c
#define LED_PORT  GPIOC
#define LED_PIN   13
#define UART_BASE 0x40011000
```

换一块板子就要改代码重新编译。Linux 的同一个内核镜像要能跑在几百种板子上，所以把「这块板子上有什么硬件、接在哪」从代码里拿出来，写成一份单独的数据文件，这就是**设备树**（Device Tree）。

- 驱动只写「怎么操作这种芯片」，比如 SPI 控制器的寄存器怎么配；
- 设备树写「这块板子上有一个这种芯片，寄存器在 0x3210000，中断号 36，时钟用哪路」；
- 内核启动时读设备树，为每个节点创建一个设备，再去找对应的驱动。

## 二、设备树语法

这是本板设备树里真实的两个节点（`dtc -I fs -O dts /proc/device-tree` 反编译出来的，删了一部分属性）：

```dts
/ {
	bus@0 {
		spi@3210000 {                                  // 节点名@单元地址（通常就是寄存器基地址）
			compatible = "nvidia,tegra234-spi", "nvidia,tegra210-spi";   // 驱动靠它匹配，越靠前越具体
			reg = <0x00 0x3210000 0x00 0x1000>;        // 寄存器：地址 0x3210000，长度 0x1000（各占 2 个 32 位格）
			interrupts = <0x00 0x24 0x04>;             // 中断：SPI 类型、36 号、高电平触发
			status = "okay";                           // "disabled" 的节点不会创建设备
			#address-cells = <1>;                      // 子节点的 reg 用 1 格表示地址
			#size-cells = <0>;                         // 子节点的 reg 没有长度

			spi@0 {                                    // 子节点：挂在这个控制器片选 0 上的设备
				compatible = "tegra-spidev";
				reg = <0>;                             // 片选号
				spi-max-frequency = <50000000>;
			};
		};
	};

	pwm-fan {
		compatible = "pwm-fan";
		pwms = <&pwm3 0 45334>;                        // 引用另一个节点：用 pwm3 的第 0 路，周期 45334 ns
		cooling-levels = <0 88 187 255>;
	};
};
```

几个必须认识的概念：

| 概念 | 说明 |
|---|---|
| 节点 | 一个硬件（或一组配置），像目录一样可以嵌套 |
| 属性 | `名字 = 值;`，值可以是字符串、`<32 位整数列表>`、`[字节]`，也可以没有值（只看存不存在，比如 `dma-coherent;`） |
| `compatible` | 最重要的属性。格式是 `"厂商,型号"`，驱动拿它和自己的匹配表比对 |
| 标签 `pwm3:` | 给节点起的名字，别的地方用 `&pwm3` 引用它。编译后变成一个整数 **phandle** |
| `#pwm-cells`、`#gpio-cells` | 「提供者」节点声明：引用我的时候，`&我` 后面要跟几个整数。pwm3 的 `#pwm-cells = <2>`，所以 `<&pwm3 0 45334>` 后面跟 2 个数 |
| 整数是 32 位大端 | `/proc/device-tree` 里读原始字节时要注意 |

运行时，内核把整棵树展开在 `/proc/device-tree/`（就是 `/sys/firmware/devicetree/base/`）下：节点是目录，属性是文件。

```bash
ls /proc/device-tree/pwm-fan/
cat /proc/device-tree/pwm-fan/compatible; echo
od -An --endian=big -t x4 /proc/device-tree/pwm-fan/pwms     # 000001d3 00000000 0000b116：phandle、0、45334
```

## 三、设备树是怎么到内核里的

```
 源码 .dts / .dtsi ──dtc 编译──► .dtb（二进制）
                                      │
   开机：UEFI 读 /boot/extlinux/extlinux.conf
        有 FDT 行   → 读那个 .dtb 文件
        没有 FDT 行 → 用 QSPI 闪存里 kernel-dtb 分区的那份
        有 OVERLAYS 行 → 把列出的 .dtbo 依次叠加上去
                                      │
                                      ▼
                     内核拿到最终的树 → /proc/device-tree
```

**覆盖层**（overlay，`.dtbo`）是「补丁」：不改原来的 .dtb，只写要增加或修改的节点。NVIDIA 的 `jetson-io` 工具配置 40 针排针，用的就是覆盖层。

本板内核没开 `CONFIG_OF_OVERLAY`，**运行时不能加载覆盖层**，只能由 UEFI 在开机时叠加，所以改了覆盖层就要重启。（有些教程里的 `/sys/kernel/config/device-tree/overlays` 在这块板子上不存在。）

## 四、平台设备和平台驱动

SPI、I2C、USB 设备挂在各自的总线上。SoC 内部那些「直接映射到内存地址上的外设」（GPIO 控制器、PWM 控制器、SPI 控制器本身）没有真正的总线，内核给它们造了一条虚拟的**平台总线**（platform bus）。

```
设备树节点（有 compatible，status 是 okay）
        │ 内核启动时
        ▼
platform_device（/sys/bus/platform/devices/ 下的一项）
        │ 总线拿它的 compatible 和每个 platform_driver 的 of_match_table 比对
        ▼
对上了 → 调用驱动的 probe(pdev)          ← 驱动在这里申请资源、初始化硬件、注册到子系统
设备或驱动被移除 → 调用 remove(pdev)
```

和单片机的对比：单片机上 `main()` 里按顺序调用 `uart_init(); spi_init();`。Linux 上没有人「调用」你的驱动，你只是登记「我能驱动 compatible 是 xxx 的设备」，**设备和驱动谁先出现都可以**，另一个出现时内核会自动配对。

驱动的骨架：

```c
static int demo_probe(struct platform_device *pdev) { ... return 0; }
static void demo_remove(struct platform_device *pdev) { ... }

static const struct of_device_id demo_of_match[] = {
	{ .compatible = "tutorial,gpio-demo", .data = &kind_gpio },   // .data 可以带每种型号的私有信息
	{ }                                                           // 空项结尾
};
MODULE_DEVICE_TABLE(of, demo_of_match);   // 让 udev 能按 compatible 自动加载这个模块

static struct platform_driver demo_driver = {
	.probe = demo_probe,
	.remove_new = demo_remove,            // 6.8 内核里返回 void 的 remove 叫 remove_new
	.driver = {
		.name = "tutorial-dtdemo",
		.of_match_table = demo_of_match,
	},
};
module_platform_driver(demo_driver);      // 展开后就是 module_init 里注册、module_exit 里注销
```

### probe 里的三个习惯

1. **用 `devm_` 系列函数申请资源**：`devm_kzalloc`、`devm_gpiod_get`、`devm_request_irq`……它们挂在设备上，probe 失败或 remove 之后自动释放，不用自己写一长串 `goto err_xxx`。
2. **返回 `-EPROBE_DEFER` 不是错误**：你要的 GPIO 控制器、时钟还没 probe 完，就返回这个，内核过一会儿自动重试。用 `dev_err_probe(dev, ret, "...")` 返回错误，它会在 DEFER 时不刷屏。查还在等待的设备：`sudo cat /sys/kernel/debug/devices_deferred`。
3. **读属性用 `device_property_*`**：`device_property_read_u32(dev, "debounce-ms", &v)`。它同时支持设备树和 ACPI，可选属性读不到就保留默认值。

## 五、引脚复用（pinmux）

Orin 的每个引脚可以当 GPIO，也可以交给 SPI、PWM、UART 等外设。每个引脚有一组配置位：

| 设备树属性 | 意思 |
|---|---|
| `nvidia,function` | 选哪个外设功能，比如 `"spi1"`、`"gp"`（PWM） |
| `nvidia,gpio-mode` | 名字容易误会：**0 = 当 GPIO 用，1 = 交给上面选的外设** |
| `nvidia,tristate` | 1 = 关闭输出驱动（引脚高阻）。**当 GPIO 输出时必须是 0**，否则写了值引脚也不动 |
| `nvidia,enable-input` | 1 = 打开输入缓冲，要读引脚电平就必须是 1 |
| `nvidia,pull` | 0 不上下拉，1 下拉，2 上拉 |

查看当前状态：

```bash
sudo cat /sys/kernel/debug/pinctrl/2430000.pinmux/pinmux-pins | grep PQ5
sudo grep -A10 "group: soc_gpio32_pq5" /sys/kernel/debug/pinctrl/2430000.pinmux/pinconf-groups
```

排针引脚号、GPIO 名、内部名称的对应关系见总 README 的接口表。

## 六、装上教程覆盖层

[overlay/tutorial-overlay.dts](overlay/tutorial-overlay.dts) 一次性准备好后面几章需要的东西：

| 片段 | 内容 | 给哪章用 | 副作用 |
|---|---|---|---|
| fragment@0 | 29 脚 GPIO 输出、31 脚 GPIO 输入（上拉），15/32/33 脚 PWM，19/21/23/24/26 脚 SPI1 | 3、4、6 | 无 |
| fragment@1 | `tutorial-gpio`、`tutorial-pwm`、`tutorial-fan` 三个新节点 | 2、3、4 | 无，没有驱动时它们只是闲着 |
| fragment@2 | 给转速计节点加标签 `tach`，fan 节点才能引用它 | 4 | 无 |
| fragment@3 | SPI1 片选 0 的设备改成 `tutorial,spi-demo` | 6 | `/dev/spidev0.0` 消失，`spidev0.1` 还在 |
| fragment@4 | ttyTHS2（uarte）下挂一个 serdev 设备 | 7 | `/dev/ttyTHS2` 消失（本来就没接出来） |

> fragment@2 是个小技巧：基础设备树里转速计节点没有标签，覆盖层里没法写 `&xxx` 引用它。给一个空的 `__overlay__` 加标签，叠加时这个标签会落到目标节点上，编译器还会自动给目标节点分配 phandle。

### 1. 编译，并先在电脑上试叠加

```bash
cd 02-device-tree/overlay
./build.sh                      # cpp 展开 #include 和宏，dtc -@ 编译（-@ 保留标签，覆盖层必须加）

B=/boot/dtb/kernel_tegra234-p3768-0000+p3767-0005-nv-super.dtb
fdtoverlay -i $B -o /tmp/merged.dtb tutorial-overlay.dtbo && echo 叠加成功
fdtget /tmp/merged.dtb /tutorial-gpio compatible
```

**一定先用 fdtoverlay 试**：如果覆盖层有错（引用了不存在的标签之类），UEFI 叠加失败时可能直接用不带覆盖层的树启动，也可能卡住。

### 2. 加一个新的启动项（原来的保留做后备）

```bash
sudo cp tutorial-overlay.dtbo /boot/
sudo cp /boot/extlinux/extlinux.conf /boot/extlinux/extlinux.conf.before-tutorial
sudo nano /boot/extlinux/extlinux.conf
```

在文件末尾加一段（`APPEND` 那一行从 `primary` 里原样复制），并把开头的 `DEFAULT primary` 改成 `DEFAULT tutorial`：

```
LABEL tutorial
      MENU LABEL jetson-drivers tutorial overlay
      LINUX /boot/Image
      FDT /boot/dtb/kernel_tegra234-p3768-0000+p3767-0005-nv-super.dtb
      INITRD /boot/initrd
      APPEND ${cbootargs} root=PARTUUID=...（照抄 primary 的）
      OVERLAYS /boot/tutorial-overlay.dtbo
```

然后 `sudo reboot`。

### 3. 验证

```bash
ls /proc/device-tree/ | grep tutorial               # tutorial-fan tutorial-gpio tutorial-pwm
ls /sys/bus/platform/devices/ | grep tutorial
cat /proc/device-tree/bus@0/spi@3210000/spi@0/compatible; echo      # tutorial,spi-demo
ls /sys/bus/serial/devices/                         # serial0-0 是新的 serdev 设备
sudo cat /sys/kernel/debug/pinctrl/2430000.pinmux/pinmux-pins | grep -E "PQ5|PQ6|PN1|PZ5"
cat /sys/class/hwmon/*/rpm                          # 风扇还在正常转
```

### 4. 出问题怎么回退

- 能 ssh 上去：把 `DEFAULT` 改回 `primary`，重启。
- 起不来：接显示器（DP 口）和 USB 键盘，开机时 extlinux 会等 30 秒（`TIMEOUT 30`），在菜单里选 `primary kernel`。
- 以后如果用 `jetson-io` 改排针配置，它会改写 extlinux.conf 并生成自己的覆盖层，和本覆盖层的引脚设置可能冲突。两者只用一个。

### 5. 改了覆盖层之后

重新 `./build.sh`，`sudo cp tutorial-overlay.dtbo /boot/`，重启。extlinux.conf 不用再改。

## 七、动手：dtdemo

[dtdemo/dtdemo.c](dtdemo/dtdemo.c) 匹配三个 `tutorial-*` 节点，probe 里做四件事：

1. `device_get_match_data()` 拿到匹配表里的 `.data`，知道自己匹配的是哪一种；
2. `for_each_property_of_node()` 把所有属性打印出来；
3. `of_count_phandle_with_args(np, "pwms", "#pwm-cells")` 数 `pwms` 里引用了几路 PWM（fan 节点是 2 路）；
4. `gpiod_count(dev, "led")` 数 `led-gpios` 有几个 GPIO，`device_property_read_u32()` 读 `debounce-ms`。

结果通过 `.dev_groups` 注册的 sysfs 属性 `summary` 给用户看。`.dev_groups` 是驱动核心提供的：probe 成功后自动创建这些文件，remove 时自动删掉，比在 probe 里自己 `device_create_file` 安全。

```bash
cd 02-device-tree/dtdemo
make
sudo insmod dtdemo.ko
sudo dmesg | tail -15
cat /sys/bus/platform/devices/tutorial-fan/summary
# kind=fan props=4 phandles=2 led=0 button=0 debounce_ms=10
ls /sys/bus/platform/drivers/tutorial-dtdemo/
sudo rmmod dtdemo
```

板子上实测的 dmesg：

```
tutorial-dtdemo tutorial-fan: probe: node /tutorial-fan, kind fan
tutorial-dtdemo tutorial-fan:   property pwm-names      9 bytes
tutorial-dtdemo tutorial-fan:   property pwms           24 bytes
tutorial-dtdemo tutorial-gpio: probe: node /tutorial-gpio, kind gpio
tutorial-dtdemo tutorial-gpio:   property debounce-ms    4 bytes
tutorial-dtdemo tutorial-gpio:   property button-gpios   12 bytes
```

`pwms` 是 24 字节：两组 `<phandle 通道 周期>`，每组 3 个 32 位整数。

## 八、测试

```bash
./test.sh
```

要填 5 处 TODO：加载模块、找到驱动在 sysfs 里的目录、用 `od` 直接读设备树原始数据、手动解绑和重新绑定设备。全部填对是 13 项 PASS。

手动 `bind`/`unbind` 在调试时很有用：改了驱动不用重启，也不用卸载模块，就能让 probe 再跑一遍。

## 九、probe 没被调用，怎么查

按顺序检查：

1. 节点在不在：`ls /proc/device-tree/...`。不在就是覆盖层没生效：看 extlinux.conf 里 `DEFAULT` 是不是 `tutorial`、`OVERLAYS` 路径对不对，`/boot/tutorial-overlay.dtbo` 是不是最新编译的。
2. `status` 是不是 `okay`。
3. compatible 是不是一个字母都不差：`cat /proc/device-tree/.../compatible`。
4. 设备是不是已经被别的驱动绑走了：`ls -l /sys/bus/platform/devices/tutorial-gpio/driver`。一个设备同一时间只能绑一个驱动。第 3、4 章的驱动和 dtdemo 匹配同样的节点，测试脚本开头会先卸载它们。
5. 是不是在等别的资源：`sudo cat /sys/kernel/debug/devices_deferred`。
6. probe 返回了错误：`sudo dmesg | grep tutorial`。

## 十、进阶练习

1. 在覆盖层里给 `tutorial-gpio` 加一个字符串属性 `label = "my-led";`，在 dtdemo 里用 `device_property_read_string()` 读出来，显示到 summary 里。（要重启。）
2. 把 `tutorial-pwm` 的 `status` 改成 `"disabled"`，重启后看 `/sys/bus/platform/devices/` 里还有没有它。
3. 用 `driver_override`（`/sys/bus/platform/devices/<设备>/driver_override`）把一个 compatible 根本不匹配的设备强制绑到 dtdemo 上，看 `device_get_match_data()` 返回什么，probe 会怎样。想想这就是为什么 probe 里要检查 `!d->kind`。
