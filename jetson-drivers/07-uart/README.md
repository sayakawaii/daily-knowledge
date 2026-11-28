# 第 7 章：UART（termios 用户态编程、serdev 内核驱动）

> 目标：
> 1. 用 termios 在用户态正确地配置和使用串口；
> 2. 写一个 serdev 驱动：在内核里独占一个串口，给用户提供更高层的接口。
>
> 两个测试都用 UART 的**内部回环**（TX 在芯片内部直接接到 RX），不需要接线。
> 第 2 部分需要第 2 章的覆盖层。

## 一、Linux 串口的层次

单片机上串口就是几个寄存器加一个中断。Linux 上一个字节从 `write()` 到 TX 引脚要经过好几层：

```
 用户程序   write(fd) / read(fd) / tcsetattr()
    │
 tty 核心   /dev/ttyTHS1 这个字符设备；termios 设置；
    │
 线路规程   默认 N_TTY：行缓冲、回显、把 Ctrl+C 变成 SIGINT、\n 转 \r\n ……  ← 「原始模式」就是把这些都关掉
    │
 serial 核心  drivers/tty/serial/serial_core.c：uart_port、uart_ops 的通用部分
    │
 UART 驱动   serial-tegra（ttyTHS*）、8250（ttyS*）、amba-pl011（ttyAMA*）
    │
  硬件
```

serdev 驱动是在 tty 层和 UART 驱动之间「截胡」：串口不再出现在 `/dev`，数据直接交给内核里的驱动。

## 二、本板的串口

| 设备 | 控制器 | 在哪 | 用途 |
|---|---|---|---|
| `/dev/ttyTCU0` | TCU（多个处理器共用的调试串口） | 载板调试接口 | 内核控制台，有 getty，**别动** |
| `/dev/ttyTHS1` | serial@3100000（UARTA） | **排针 8（TX）/ 10（RX）**，11 RTS / 36 CTS | 本章第 1 部分 |
| `/dev/ttyTHS2` → serdev | serial@3140000（UARTE） | 没有引出来 | 本章第 2 部分（覆盖层把它变成了 serdev） |
| `/dev/ttyAMA0` | serial@31d0000（`arm,sbsa-uart`） | 未确认 | 没有程序占用，本教程不用 |
| `/dev/ttyGS0` | USB gadget | USB-C 口 | 插电脑时的 USB 串口，有 getty |
| `/dev/ttyS0~3` | 8250 | 不存在的硬件 | 内核默认占位，忽略 |

`ttyTHS` 的 THS = Tegra High Speed，支持 DMA，实测内部回环最高 3 Mbps 正常。

权限：`/dev/ttyTHS1` 属于 `dialout` 组，当前用户不在这个组里，测试用 sudo。长期用的话 `sudo usermod -aG dialout $USER` 后重新登录。

## 三、termios：用户态串口编程

最常见的错误是**没有设原始模式**：默认的线路规程会把收到的 `\r` 变成 `\n`、把 0x03 当成 Ctrl+C、等到收到换行才让 `read` 返回……二进制协议全乱。

```c
int fd = open("/dev/ttyTHS1", O_RDWR | O_NOCTTY);   // O_NOCTTY：别把它当成本进程的控制终端
struct termios tio;
tcgetattr(fd, &tio);
cfmakeraw(&tio);                     // 原始模式：关掉上面说的所有处理
cfsetispeed(&tio, B115200);
cfsetospeed(&tio, B115200);
tio.c_cflag |= CLOCAL | CREAD;       // 忽略调制解调器控制线、打开接收
tio.c_cflag &= ~(CRTSCTS | CSTOPB | PARENB);   // 无硬件流控、1 位停止位、无校验 → 8N1
tio.c_cc[VMIN] = 0;                  // read 的返回条件，见下表
tio.c_cc[VTIME] = 0;
tcsetattr(fd, TCSANOW, &tio);
tcflush(fd, TCIOFLUSH);              // 清掉打开之前残留的数据
```

`VMIN` 和 `VTIME` 决定 `read()` 什么时候返回（原始模式下）：

| VMIN | VTIME | read 的行为 |
|---|---|---|
| 0 | 0 | 有多少返回多少，没有就返回 0（配合 `poll` 用，本章的写法） |
| N | 0 | 至少收到 N 字节才返回 |
| 0 | T | 等最多 T × 0.1 秒，收到任何数据就返回 |
| N | T | 收到 N 字节，或者两个字节之间间隔超过 T × 0.1 秒 |

内部回环：`ioctl(fd, TIOCMBIS, &bits)`，`bits = TIOCM_LOOP`（0x8000）。它设置的是 UART 的 MCR 寄存器的 LOOP 位，和 16550 一样。glibc 的头文件里没有这个宏，代码里自己定义了。

命令行工具：`stty -F /dev/ttyTHS1 -a` 看当前设置，`picocom -b 115200 /dev/ttyTHS1` 当终端用（`sudo apt install picocom`）。

### 测试

```bash
cd 07-uart/uart
# 先补完 uart_loop.c 里的两处 TODO：termios 设置、打开内部回环
make
./test.sh                # 9600、115200、921600、3000000 四种波特率
```

全部完成是 7 项 PASS。没补完时编译有一个 `unused variable 'tio'` 警告，正常。

接线的外部回环：杜邦线短接排针 8 和 10，`sudo ./uart_loop /dev/ttyTHS1 115200 ext`。

## 四、serdev：在内核里用串口

什么时候需要：串口上接的是一个**芯片**（蓝牙、GPS、串口屏、指纹模块、电机驱动器），你想让它像普通设备一样被使用，而不是让每个应用程序自己去开串口、拼协议。典型例子是串口蓝牙芯片：内核的 `hci_uart` 是 serdev 驱动，在串口上跑 HCI 协议，用户看到的是 `hci0`，不是串口。（本板无线网卡的蓝牙走的是 USB，`hci0` 在 `usb1/1-3` 下面，不是这种情况。）

设备树里把设备写成 UART 节点的**子节点**（覆盖层 fragment@4）：

```dts
&uarte {
	tutorial-serdev {
		compatible = "tutorial,serdev-demo";
		current-speed = <115200>;
	};
};
```

UART 有了子节点，serial 核心就不再创建 `/dev/ttyTHS2`，而是创建一个 serdev 控制器（`/sys/bus/serial/devices/serial0`）和设备（`serial0-0`），等驱动来匹配。

代码：[tserdev/tserdev.c](tserdev/tserdev.c)

```
probe
 ├─ serdev_device_set_client_ops(receive_buf, write_wakeup)
 ├─ devm_serdev_device_open()                 相当于 open("/dev/ttyTHS2")
 ├─ set_baudrate(current-speed)、关流控、无校验
 ├─ set_tiocm(TIOCM_LOOP)                     内部回环（ttyTHS2 没引出来，只能这样测）
 ├─ msleep(20)，清掉开串口时收到的噪声
 └─ misc_register("tutorial_serdev")          第 1 章的字符设备

收到数据：tty 层 → tserdev_receive_buf() → kfifo → 唤醒等待的 read
用户 write：→ serdev_device_write() → UART
```

几个要点：

- **`receive_buf` 的返回值**是「接收了多少字节」。返回少了，tty 层会留着稍后再给；这里选择全收、放不下就丢弃并计数（`rx_dropped`），避免读者不读的时候堵住整个 tty 层。不同设备的取舍不一样。
- **kfifo 单读单写不需要锁**：`receive_buf` 是唯一写者，用户 `read` 是唯一读者（用 `read_lock` 保证同一时间一个读者）。
- **清噪声那一步是实测出来的**：最早的版本里，测试偶尔会多收到一个 0x00 字节。原因是从打开串口到切到内部回环之间，悬空的 RX 引脚有噪声。真实接了设备的串口一般不会这样，但接线松动、设备上电时序也会产生类似的垃圾字节，**串口协议一定要有帧头和校验**，不能假设收到的第一个字节就是有效数据。

### 测试

```bash
cd 07-uart/tserdev
make
./test.sh
```

3 处 TODO：写入并读回、通过 sysfs 改波特率、关掉内部回环。全部完成是 12 项 PASS。

## 五、接 USB 转 TTL 模块和电脑通信

```
USB-TTL 模块（3.3V 电平）     Jetson 排针
GND ────────────────────────── 6 脚
RXD ────────────────────────── 8 脚（Jetson TX）
TXD ────────────────────────── 10 脚（Jetson RX）
                                ← VCC 不接，两边各自供电
```

> **安全**：排针是 **3.3V TTL** 电平。买 USB-TTL 模块要选能切到 3.3V 的（CH340、CP2102 都有，注意跳线帽）；**绝对不要接 RS-232**（电脑 DB9 串口，±12V），会烧引脚。

电脑上用串口助手、Jetson 上用 `picocom -b 115200 /dev/ttyTHS1`，两边能互相打字就对了。

## 六、进阶练习

1. 给 tserdev 加一个简单的帧协议：`0xAA 长度 数据 校验和`。`receive_buf` 里做状态机解帧，`read` 每次返回一个完整的帧，校验错的帧丢掉并计数。这就是大部分串口设备驱动的核心。
2. 用 `ttyTHS1` 的 RTS/CTS（排针 11、36）测试硬件流控：外部短接 RTS 和 CTS，打开 `CRTSCTS`，再断开，看 `write` 会不会阻塞。
3. 读 `drivers/tty/serdev/serdev-ttyport.c`：serdev 是怎么借用 tty 层的？它为什么能在 tty 层之下拿到数据？
4. 读 `drivers/bluetooth/hci_serdev.c` 和 `hci_h4.c`：串口蓝牙是怎么用 serdev 把 UART 变成 `hci0` 的？对比本板 USB 蓝牙的路径：`readlink -f /sys/class/bluetooth/hci0`。
