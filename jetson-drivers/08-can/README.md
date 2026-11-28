# 第 8 章：CAN（SocketCAN、网络设备驱动、mttcan 内部回环）

> 目标：
> 1. 用 SocketCAN 在用户态收发 CAN 帧；
> 2. 写一个虚拟 CAN 网卡驱动（一对互联的网卡），学网络设备驱动的结构；
> 3. 用板载 CAN 控制器（mttcan）的内部回环模式测真硬件。
>
> 不需要硬件，不需要覆盖层。

## 一、CAN 回顾

差分两线（CAN_H、CAN_L），多主机广播，没有地址，靠**帧 ID** 区分内容，ID 越小优先级越高（仲裁）。一帧最多 8 字节数据（CAN FD 最多 64 字节）。总线两端各一个 120 Ω 终端电阻。

单片机上你可能用过 STM32 的 bxCAN：配过滤器组、往发送邮箱写数据、在中断里读 FIFO。Linux 上这些都被包装成了**网卡**。

## 二、SocketCAN：CAN 控制器就是一块网卡

Linux 把 CAN 控制器当成网络设备（和以太网卡、WiFi 网卡同一个框架），用 socket 收发：

```
 用户程序    socket(PF_CAN, SOCK_RAW, CAN_RAW)；write/read struct can_frame
     │
 CAN 协议栈  net/can/：af_can（按 ID 分发给各个 socket、软件过滤）、raw、bcm、isotp、j1939
     │
 网络设备层  struct net_device；发送队列；统计
     │ ndo_start_xmit / netif_rx
 CAN 驱动    mttcan（真控制器）、vcan、tvcan（虚拟）
```

好处：可以有任意多个程序同时监听同一个 CAN 口，各自设过滤器；`ip`、`tcpdump`、Wireshark 都能直接用。

```bash
ip -details link show can0          # 控制器状态、位时序范围、时钟（本板 50 MHz）
sudo ip link set can0 type can bitrate 500000
sudo ip link set can0 up
candump can0                        # can-utils：抓包
cansend can0 123#DEADBEEF           # 发一帧：ID 0x123，数据 DE AD BE EF
ip -s link show can0                # 收发统计
sudo ip link set can0 down
```

CAN 网卡**必须先设波特率才能 up**（以太网是自动协商的，CAN 不是）。

### 用户态编程

代码：[tvcan/can_echo.c](tvcan/can_echo.c)

```c
int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
struct ifreq ifr; strcpy(ifr.ifr_name, "can0");
ioctl(s, SIOCGIFINDEX, &ifr);                         // 网卡名 → 编号
struct sockaddr_can addr = { .can_family = AF_CAN, .can_ifindex = ifr.ifr_ifindex };
bind(s, (struct sockaddr *)&addr, sizeof(addr));

struct can_frame f = { .can_id = 0x123, .len = 8, .data = {...} };
write(s, &f, sizeof(f));                               // 发
read(s, &f, sizeof(f));                                // 收（阻塞）

struct can_filter flt = { .can_id = 0x123, .can_mask = CAN_SFF_MASK };
setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, &flt, sizeof(flt));   // 只收 0x123
```

这和 STM32 的过滤器组是同一个概念，只是这里是每个 socket 一组、在软件里过滤（有的控制器驱动会把过滤器下放到硬件）。

## 三、网络设备驱动的结构

代码：[tvcan/tvcan.c](tvcan/tvcan.c)

```c
dev = alloc_netdev(sizeof(priv), "tvcan0", NET_NAME_USER, tvcan_setup);
// tvcan_setup 里：dev->type = ARPHRD_CAN; dev->mtu = CANFD_MTU; dev->netdev_ops = &tvcan_ops;
can_set_ml_priv(dev, &priv->ml);    // CAN 协议栈要的每网卡数据
register_netdev(dev);               // 出现在 ip link 里
```

| 回调 | 什么时候调用 | tvcan 做什么 | 真控制器驱动做什么 |
|---|---|---|---|
| `ndo_open` | `ip link set up` | 打开发送队列 | 上电、配位时序、开中断、进入工作模式 |
| `ndo_stop` | `ip link set down` | 关发送队列 | 关中断、进入配置/休眠模式 |
| `ndo_start_xmit` | 协议栈要发一帧 | 把 skb 交给对端网卡的 `netif_rx` | 写进发送缓冲区，启动发送；缓冲区满了就 `netif_stop_queue` |
| （中断） | 收到帧 | — | 分配 skb、填数据、`netif_rx`（或 NAPI 里 `netif_receive_skb`） |

**sk_buff**（skb）是网络栈里「一个包」的统一表示。CAN 帧放在 `skb->data` 里，就是一个 `struct can_frame` 或 `canfd_frame`。`ndo_start_xmit` 返回 `NETDEV_TX_OK` 表示这个 skb 归驱动了（发出去还是丢掉都由驱动负责释放），协议栈不再碰它。

**统计计数**：`dev->stats.tx_packets` 等由驱动自己加，`ip -s link`、`/sys/class/net/*/statistics/` 读的就是它们。测试里用它们验证驱动的行为。

**IFF_ECHO**：真 CAN 控制器驱动设这个标志，帧真正发到总线上以后，驱动把它「回送」给本机的其他 socket（`can_put_echo_skb` / `can_get_echo_skb`），这样本机其他程序看到的帧顺序和总线上一致。tvcan 不设，由协议栈在软件里回送。

### 测试

```bash
cd 08-can/tvcan
# 先补完 can_echo.c 里的 2 处 TODO：建 socket 并绑定、设接收过滤器
make
./test.sh                # 虚拟网卡对：收发、统计、过滤、candump、对端关闭时丢帧
./test_mttcan.sh         # 真控制器内部回环
```

`test.sh` 有 2 处 TODO（打开网卡、关闭对端），全部完成 13 项 PASS。`test_mttcan.sh` 有 1 处 TODO（用 `ip link` 配置波特率和回环），全部完成 6 项 PASS，脚本结束时会把 can0 恢复成关闭、非回环。

## 四、mttcan：板载 CAN 控制器

Orin 的 CAN 控制器是 Bosch M_CAN IP（支持 CAN FD），NVIDIA 的驱动叫 mttcan（`c310000.mttcan`）。

**内部回环**（`loopback on`）：控制器把发出的帧在芯片内部送回接收端，不经过 TX/RX 引脚。所以**不接 CAN 收发器也能测控制器、驱动、协议栈**。测试里 `tx_packets` 加 1，说明帧确实经过了控制器的发送逻辑。

正常模式下，单独一个节点在总线上发帧，没有别的节点应答（ACK），控制器会不断重发，错误计数涨到一定程度进入 `ERROR-PASSIVE`，再涨进入 `BUS-OFF`。用 `ip -details link show can0` 能看到状态和错误计数，`restart-ms 100` 可以让它自动从 BUS-OFF 恢复。

### 接真 CAN 总线

需要一个 **CAN 收发器模块**：SoC 出来的是 3.3V 的 TX/RX 逻辑电平，收发器把它转成 CAN_H/CAN_L 差分信号。选 **3.3V 供电的 SN65HVD230 模块**（几块钱）；常见的 TJA1050 是 5V 的，不要直接接。

- 开发板的 CAN TX/RX 不在 40 针排针上，在载板上单独的一个小排针上。具体位置和引脚顺序请查 NVIDIA 的 *Jetson Orin Nano Developer Kit Carrier Board Specification*（我没核实过具体位号）。
- 最便宜的完整测试：两个 SN65HVD230 模块 + 一个 USB-CAN 适配器（或者另一块有 CAN 的板子），总线两端各 120 Ω（很多模块板载了，注意别多接）。
- 和汽车 OBD 口相连之前，先确认波特率（常见 500 k）并用 `listen-only on` 只听不发。

## 五、进阶练习

1. 给 tvcan 加 CAN FD 支持的验证：`sudo ip link set tvcan0 mtu 72`，用 `cansend tvcan0 123##1.0011223344556677889900`（FD 帧语法）发一个 12 字节的帧。
2. 给 tvcan 加一个模块参数 `error_rate`，按比例随机丢帧并发一个错误帧（`alloc_can_err_skb`）给接收端，用 `candump -e` 观察。
3. 把 tvcan 改成用 `alloc_candev` + `register_candev`（can-dev 框架），让 `ip link set tvcan0 type can bitrate ...` 能用，并支持 `IFF_ECHO`。可以参考 `drivers/net/can/vcan.c` 和 `vxcan.c`。
4. 在 mttcan 回环模式下试 `cangen can0 -g 1`（每毫秒一帧）跑一分钟，看 `ip -s link` 有没有丢帧、`top` 里软中断占用多少。
