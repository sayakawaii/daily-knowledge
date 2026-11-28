# 第 5 章：I2C 设备驱动（板载 EEPROM、LM75A 温度传感器）

> 目标：
> 1. 写一个只读驱动，读板载 EEPROM，和内核 at24 驱动读到的内容逐字节对比；
> 2. 写一个 LM75A 温度传感器的 hwmon 驱动，用内核的 I2C 模拟器 i2c-stub 测试，买了模块再接真硬件。
>
> 不需要外接硬件。不需要第 2 章的覆盖层（I2C 设备可以运行时创建）。

## 一、I2C 回顾，以及和单片机的区别

两根线 SCL、SDA，开漏加上拉；主机发起始条件、7 位地址加读写位，从机应答。这些你在单片机上都写过。

区别在于软件分层：

```
    设备驱动（你写的）         teeprom、tlm75、内核的 at24、ina3221……
        │  i2c_transfer() / i2c_smbus_read_byte_data() ……
        ▼
    I2C 核心                   drivers/i2c/i2c-core-*.c：总线、设备、驱动的匹配，SMBus 模拟
        │  adapter->algo->master_xfer()
        ▼
    适配器（控制器）驱动        i2c-tegra（真控制器）、i2c-stub（模拟器）
        │
        ▼
      硬件
```

单片机上「设备驱动」和「控制器驱动」常常写在一起（`HAL_I2C_Mem_Read` 里既管时序又管传感器寄存器）。Linux 把它们分开：**设备驱动只描述「和这个芯片说什么」，不碰控制器寄存器**。所以同一个 LM75 驱动，挂在 Tegra 控制器上、挂在 USB 转 I2C 上、挂在模拟器上都能用。

## 二、本板的 I2C 总线

```bash
i2cdetect -l                  # 列出所有总线
i2cdetect -y -r 0             # 扫描总线 0；UU = 地址已被驱动占用，数字 = 有设备应答但没驱动
ls /sys/bus/i2c/devices/      # 0-0050 = 总线 0 地址 0x50
```

| 总线 | 控制器 | 在哪 | 上面有什么 |
|---|---|---|---|
| i2c-0 | 3160000.i2c | 板内 | 0x50 模组 EEPROM，0x57 载板 EEPROM（都是 24C02） |
| i2c-1 | c240000.i2c | 排针 27（SDA）/ 28（SCL） | 0x25 fusb301（USB-C），0x40 INA3221（电源监测） |
| i2c-7 | c250000.i2c | **排针 3（SDA）/ 5（SCL）** | 空，外接传感器用这条 |

用户 `minghe` 在 `i2c` 组里，`i2cdetect`、`i2cget` 不用 sudo。

## 三、I2C 设备从哪来

驱动只有和设备匹配上才会 probe。I2C 设备有三种来源：

1. **设备树**：控制器节点下的子节点，比如 `eeprom@57 { compatible = "atmel,24c02"; reg = <0x57>; }`。设备名取 compatible 逗号后的部分：`24c02`。
2. **运行时用 sysfs 创建**：`echo tutorial-lm75 0x48 | sudo tee /sys/bus/i2c/devices/i2c-7/new_device`。调试和教学最方便，不用重启。
3. 驱动里自己探测（`detect` 回调），现在不推荐。

匹配规则：先比设备树 compatible 和驱动的 `of_match_table`，再比设备名和驱动的 `id_table`。

## 四、两套读写接口

| 接口 | 用在哪 | 例子 |
|---|---|---|
| `i2c_transfer(adapter, msgs, n)` | 任意 I2C 消息序列，最灵活 | EEPROM 连续读、多字节命令 |
| `i2c_smbus_read_byte_data(client, reg)` 等 | 「写寄存器地址、读/写 1~2 字节」这种最常见的模式 | 绝大多数传感器 |

SMBus 是 I2C 的子集（Intel 定义的）。控制器只支持 SMBus 时（比如 i2c-stub、很多 PC 主板），`i2c_transfer` 用不了；反过来，支持完整 I2C 的控制器，内核会用 I2C 消息模拟 SMBus 操作。所以**能用 SMBus 接口就用 SMBus 接口**，兼容性最好。probe 里用 `i2c_check_functionality()` 检查控制器支不支持你要用的操作。

### 字节序的坑

SMBus 规定「字」（word，16 位）**低字节先传**，但大多数传感器的 16 位寄存器是**高字节先传**。`i2c_smbus_read_word_data()` 按 SMBus 拼字节，对这类芯片就是反的，要用 `i2c_smbus_read_word_swapped()`。测试的 TODO 1、2 就是让你手算一次。

## 五、teeprom：板载 EEPROM 只读驱动

代码：[teeprom/teeprom.c](teeprom/teeprom.c)

EEPROM 内容（NVIDIA 格式，256 字节）：

| 偏移 | 内容 |
|---|---|
| 0x00 | 格式版本 |
| 0x14 起 | 板子料号，ASCII，比如 `699-13768-0000-400 F.1`（载板）、`699-13767-0005-300 T.1`（模组） |
| 0x4a 起 | 序列号（教程里不展示） |
| 0xff | CRC-8 校验（前 255 字节） |

校验算法是我在板子上试出来的：试了 3 种常见的 CRC-8，只有 **CRC-8/MAXIM**（多项式 0x31，按位反射，代码里写 0x8c）和两片 EEPROM 的最后一个字节都对得上。

驱动要点：

- **`i2c_transfer` 两条消息加重复起始条件**：先写 1 字节偏移地址，不发停止位，接着读。这是 24Cxx 的「随机读」时序，代码注释里画了波形。
- **二进制 sysfs 属性** `BIN_ATTR_RO(eeprom, 256)`：用来导出原始数据（文本属性只适合一个值一行）。at24 也是这样导出的，所以可以直接 `cmp` 对比。
- **只读**：EEPROM 里是板子身份信息，写坏了 NVIDIA 的软件可能认不出板子。驱动里没有写操作，测试里用 i2cget 读也不会改内容。
- **id_table 写的是 `24c02`**：和 at24 认同一个名字。开机时 at24 先加载先绑定；把它解绑后加载 teeprom，内核会自动把还没驱动的 0-0057 绑给 teeprom。

### 测试

```bash
cd 05-i2c/teeprom
make
./test.sh
```

3 处 TODO：从 at24 解绑、用 `dd` 从二进制文件里取出料号、用 `i2cget -f` 在用户态读一个字节。全部完成是 10 项 PASS。脚本退出时自动把 EEPROM 还给 at24。

## 六、tlm75：LM75A 温度传感器

代码：[tlm75/tlm75.c](tlm75/tlm75.c)

| 寄存器 | 内容 |
|---|---|
| 0x00 温度 | 16 位，高 11 位有效，0.125 ℃/LSB，补码 |
| 0x01 配置 | bit0 = 1 关断（省电，不测量） |
| 0x02 Thyst | 报警解除温度，高 9 位，0.5 ℃/LSB |
| 0x03 Tos | 报警温度，高 9 位，0.5 ℃/LSB，超过时 OS 引脚动作 |

换算：`温度(毫摄氏度) = ((s16)原始值 >> 5) × 125`。**先转成 `s16` 再右移**：有符号数右移是算术右移，补高位符号位，负温度才算得对；`u16` 右移补 0，-0.5 ℃ 会变成 255.5 ℃。

驱动行为：probe 时读配置寄存器（读不到 = 这个地址没芯片，probe 失败），清掉关断位开始测量；remove 时（devm action）重新进入关断模式。

### 用 i2c-stub 测试

[tools/i2c-stub/build.sh](../tools/i2c-stub/build.sh) 把内核的 `i2c-stub.c` 编译成外部模块（本板内核没开 `CONFIG_I2C_STUB`）。它模拟一条 SMBus 总线，在指定地址上放一个有 256 个寄存器的「芯片」，可以用 `i2cset` 预先写值：

```bash
cd 05-i2c/tlm75
make                     # 顺便编译 i2c-stub
./test.sh
```

5 处 TODO：手算 25.125 ℃ 和 -0.5 ℃ 对应的寄存器值（包括字节交换和补码）、用 `new_device` 创建设备、通过 hwmon 写报警温度、用 `delete_device` 删除设备。全部完成是 10 项 PASS。

### 接真 LM75A 模块

```
LM75A 模块      Jetson 排针
VCC   ───────── 1 脚（3.3V）       ← 不要接 5V
GND   ───────── 6 脚
SDA   ───────── 3 脚
SCL   ───────── 5 脚
A0/A1/A2 ────── 模块上一般已经拉到 GND → 地址 0x48
```

```bash
i2cdetect -y -r 7                                                    # 应该看到 48
sudo insmod tlm75.ko
echo tutorial-lm75 0x48 | sudo tee /sys/bus/i2c/devices/i2c-7/new_device
cat /sys/class/hwmon/hwmon*/temp1_input                              # 用手捏住芯片，数字会涨
sudo apt install lm-sensors && sensors                               # 不用认识这个芯片也能显示（hwmon 标准接口的好处）
```

想开机自动创建，把它写进设备树：在覆盖层里给 `i2c@c250000` 加子节点 `temp-sensor@48 { compatible = "tutorial,lm75"; reg = <0x48>; };`（驱动的 of_match_table 已经写好了）。

> 内核本身就有 LM75 驱动（`drivers/hwmon/lm75.c`，名字 `lm75a`），本板内核也带了。实际项目直接用它；这里自己写是为了学。写完可以对比着读一下内核的版本，看它多处理了哪些情况（几十种兼容芯片、采样时间、regmap 缓存）。

## 七、进阶练习

1. 把 tlm75 改成用 **regmap**（`devm_regmap_init_i2c`）：寄存器读写、字节序、缓存都交给 regmap，驱动更短。内核里新写的 I2C/SPI 驱动基本都用 regmap。
2. 给 LM75A 的 OS 引脚接一个 GPIO（比如排针 31 脚），在设备树里写 `interrupts`，驱动里 `devm_request_threaded_irq`，过温时 `hwmon_notify_event(..., hwmon_temp_max_alarm)`。注意中断线程里可以做 I2C 传输，硬中断里不行。
3. teeprom 加上读模组 EEPROM（0x50）：两片的料号都显示出来。不用改代码，想想为什么。
4. 用 `i2ctransfer`（i2c-tools 里的）在用户态复现 teeprom 的「写偏移 + 重复起始读」：`i2ctransfer -f -y 0 w1@0x57 0x14 r18`。
