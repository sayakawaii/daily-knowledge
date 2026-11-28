# 第 6 章：SPI（设备驱动、虚拟控制器、IIO、spidev）

> 目标：
> 1. 写 ADXL345 加速度计的 SPI 驱动，注册成 IIO 设备；
> 2. 写一个虚拟 SPI 控制器，在内存里模拟一颗 ADXL345，没有硬件也能测驱动；
> 3. 用 spidev 在用户态做 SPI 回环（需要一根杜邦线）。
>
> 前提：第 2 章的覆盖层已装好（SPI1 引脚复用、片选 0 换成教程设备）。

## 一、SPI 回顾

四根线：SCLK、MOSI、MISO、CS（片选，低有效）。**全双工**：每个时钟主机发一位，同时收一位。没有地址，靠片选选芯片；没有应答，读到的数据对不对，只能靠芯片 ID 寄存器之类的去验证。

**模式**（CPOL、CPHA）决定时钟空闲电平和在哪个边沿采样：

| 模式 | CPOL（空闲电平） | CPHA（采样边沿） |
|---|---|---|
| 0 | 低 | 第一个边沿（上升沿） |
| 1 | 低 | 第二个边沿（下降沿） |
| 2 | 高 | 第一个边沿（下降沿） |
| 3 | 高 | 第二个边沿（上升沿） |

模式不对，最常见的现象是读到的全是 0xFF、0x00，或者数据错一位。ADXL345 要求模式 3，测试里有一项就是故意用模式 0 加载，看驱动怎么失败。

## 二、Linux SPI 子系统

```
  SPI 设备驱动                 tadxl（你写的）、spidev（通用用户态接口）、内核的各种 SPI 芯片驱动
     │ spi_write / spi_write_then_read / spi_sync(spi_message)
     ▼
  SPI 核心                     drivers/spi/spi.c：消息排队、片选、DMA 映射、匹配
     │ ctlr->transfer_one / transfer_one_message
     ▼
  SPI 控制器驱动               spi-tegra114（真 SPI1、SPI3）、tspi_sim（虚拟）
```

和 I2C 一样分层，所以**同一个 tadxl 驱动**可以挂在真控制器上，也可以挂在虚拟控制器上。

数据结构：

- `spi_transfer`：一段收发（tx_buf、rx_buf、len，可以单独设速度）；
- `spi_message`：一串 transfer，**整个过程片选一直有效**。芯片的「命令 + 数据」必须在同一条消息里；
- 便捷函数：`spi_write()`（只发）、`spi_write_then_read()`（发命令再读，片选不断）、`spi_w8r8()`（发 1 字节收 1 字节）。

本板的 SPI：

| 控制器 | 排针 | Linux 设备 |
|---|---|---|
| spi@3210000（SPI1） | 19 MOSI、21 MISO、23 SCK、24 CS0、26 CS1 | spi0.0（覆盖层：tutorial,spi-demo）、spi0.1 → `/dev/spidev0.1` |
| spi@3230000（SPI3） | 37 MOSI、22 MISO、13 SCK、18 CS0、16 CS1 | spi1.0、spi1.1 → `/dev/spidev1.*`（本教程覆盖层没有配它的引脚复用） |

## 三、tadxl：ADXL345 驱动

代码：[tadxl/tadxl.c](tadxl/tadxl.c)

ADXL345 协议：第一个字节 bit7 = 读写、bit6 = 多字节、bit5~0 = 寄存器地址，后面跟数据。

```
probe
 ├─ spi->mode = 模式 3，max_speed_hz ≤ 5 MHz，spi_setup()     ← 驱动自己保证模式对
 ├─ 读 DEVID（0x00），不是 0xE5 就返回 -ENODEV                 ← 没芯片、接错线、模式错都能发现
 ├─ DATA_FORMAT = 满分辨率；POWER_CTL = 测量
 ├─ devm_add_action(卸载时 POWER_CTL = 0 待机)
 └─ devm_iio_device_register()
```

**读 DEVID 是 SPI 驱动的基本功**：SPI 没有应答，不读 ID 的话，没接芯片 probe 也会「成功」。你会在 dmesg 里看到真 SPI1 片选 0 上的 probe 失败（`DEVID 0x00`，MISO 悬空），这正是对的。

**DMA 缓冲区不能在栈上**：SPI 控制器可能用 DMA 直接读写你给的缓冲区，栈内存不保证能 DMA。`spi_write_then_read()` 内部会拷到自己的缓冲区，所以它的参数可以在栈上；自己组 `spi_message` 时，缓冲区要用 `kmalloc` 或放在 `devm_kzalloc` 的结构体里（还要注意缓存行对齐，见 `____cacheline_aligned`）。

### IIO 子系统

加速度计、陀螺仪、ADC、DAC、光照、压力这类「采样型」传感器，标准做法是注册成 IIO（Industrial I/O）设备，而不是 hwmon（hwmon 管的是温度、风扇这些监控量）。

```bash
ls /sys/bus/iio/devices/iio:device0/
# in_accel_x_raw  in_accel_y_raw  in_accel_z_raw  in_accel_scale  name
```

驱动只需描述通道（`iio_chan_spec`：类型 ACCEL、修饰 X/Y/Z、有 RAW 和 SCALE），实现 `read_raw`，文件名由 IIO 核心按标准生成。用户程序拿 `raw × scale` 得到 m/s²。IIO 还支持缓冲区和触发器做连续高速采样（`/dev/iio:deviceN`），这是进阶内容。

## 四、tspi_sim：虚拟 SPI 控制器

代码：[tadxl/tspi_sim.c](tadxl/tspi_sim.c)

它同时演示了**SPI 控制器驱动**怎么写：

```c
ctlr = devm_spi_alloc_host(&pdev->dev, sizeof(*s));
ctlr->num_chipselect = 1;
ctlr->mode_bits = SPI_CPOL | SPI_CPHA;          // 支持哪些模式
ctlr->transfer_one_message = tspi_sim_transfer_one_message;
devm_spi_register_controller(&pdev->dev, ctlr);
spi_new_device(ctlr, &info);                    // 在片选 0 上「挂」一个设备，相当于设备树子节点
```

`transfer_one_message` 拿到一整条消息：真控制器在这里配寄存器、启动 DMA、等完成中断；我们直接逐字节模拟 ADXL345（命令字节解析、寄存器读写、地址自动加 1、未测量时数据为 0、**模式不对时全部返回 0xFF**）。处理完调用 `spi_finalize_current_message()`。

模拟器的 sysfs（`/sys/devices/platform/tutorial-spi-sim/`）：`accel` 写入 `x y z` 改变「测到的」加速度，`regdump` 看 64 个寄存器，`xfers` 看收到了几条消息。

## 五、测试

### 模拟模式

```bash
cd 06-spi/tadxl
make
./test.sh
```

4 处 TODO：加载两个模块、找 IIO 设备目录、用 awk 算 raw × scale、用错误的 SPI 模式加载看它失败。全部完成是 13 项 PASS。

### spidev 回环（需要一根杜邦线）

spidev 是内核的通用驱动，把 SPI 设备变成 `/dev/spidevB.C`，用户程序用 `ioctl(SPI_IOC_MESSAGE)` 收发。快速验证硬件、或者芯片太简单不值得写驱动时用它。

```bash
# 杜邦线短接排针 19（MOSI）和 21（MISO）
# 先补完 spi_loop.c 里的 TODO（填 struct spi_ioc_transfer）
make
./test_jumper.sh          # 1、10、25 MHz 各测一次
```

不接线时收到全 0，3 项 FAIL，正常。

### 接真 ADXL345 模块

```
ADXL345 模块     Jetson 排针
VCC  ─────────── 1 脚（3.3V）
GND  ─────────── 6 脚
SDA（SDI）────── 19 脚 MOSI
SDO ──────────── 21 脚 MISO
SCL（SCLK）───── 23 脚 SCK
CS ───────────── 24 脚 CS0
```

模块上 CS 接地时芯片工作在 I2C 模式，接到 CS0 才是 SPI 模式。接好后 `sudo insmod tadxl.ko`，dmesg 里 spi0.0 应该是 `ADXL345 ready`，转动模块看 `in_accel_*_raw` 变化。

> 这一步我没有硬件实测。如果读到的 DEVID 是 0xE5 以外的值：先查接线，再把速度降到 1 MHz（设备树 `spi-max-frequency`），再查模块是不是 3.3V 版本。

## 六、进阶练习

1. 改用 regmap（`devm_regmap_init_spi`，`read_flag_mask = 0x80`），对比代码量。
2. 支持 IIO 缓冲区：用 `iio_triggered_buffer_setup` 加一个 hrtimer 触发器，100 Hz 连续采样，用户态读 `/dev/iio:deviceN`。
3. 给 tspi_sim 加「随机噪声」和「自由落体」模式，再给 tadxl 加 ADXL345 的 FREE_FALL 中断（接 INT1 到一个 GPIO，用第 3 章学的中断）。
4. 读 `drivers/spi/spi-tegra114.c` 的 `tegra_spi_transfer_one`：真控制器是怎么在 PIO 和 DMA 之间选择的？
