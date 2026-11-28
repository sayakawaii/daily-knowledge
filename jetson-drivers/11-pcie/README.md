# 第 11 章：PCIe 与以太网（配置空间、BAR、中断、DMA）

> 目标：
> 1. 搞懂 PCIe 设备在 Linux 里长什么样：BDF 地址、配置空间、BAR、能力表、中断（INTx / MSI / MSI-X）、DMA 和 IOMMU；
> 2. 写一个 PCI 驱动 `tpci`，借板载千兆网卡芯片 RTL8111H 把这些全部实际做一遍：映射寄存器、读 MAC 和 PHY、触发并处理中断、让芯片 DMA 写内存；
> 3. 学会驱动绑定的手动操作：`unbind`、`bind`、`driver_override`；
> 4. 知道一个真正的网卡驱动比这多了什么。

不用接任何东西。测试期间网口不可用，所以要通过 **Wi-Fi 或 USB** 登录板子（脚本会检查，走网线时拒绝运行）。

---

## 1. 给单片机工程师的 PCIe 速成

在单片机里，外设寄存器在固定地址上，`*(volatile uint32_t *)0x40021000 = x` 就能写。PCIe 设备也是这样用的：**读写一段内存地址就是读写设备寄存器**。区别在于：

- **地址不是固定的。** 开机时主机（root complex）扫描总线、逐个询问设备「你要多大的地址窗口」，再给它分配一段。这个窗口叫 **BAR**（Base Address Register）；
- **中间隔着一条串行链路。** CPU 的一次读写被打包成 TLP（事务层包），经过 PCIe 链路发给设备，设备再回一个包。所以读一次 PCIe 寄存器比读片上外设慢得多，大约要几百纳秒到一微秒；
- **设备也能主动读写内存**（总线主控 / DMA）。网卡收到包，自己把数据写进内存，再通知 CPU。

**BDF 地址**：`0008:01:00.0` = 域 0008 : 总线 01 : 设备 00 . 功能 0。Jetson 有好几个独立的 PCIe 控制器，每个控制器一个域。

**配置空间**：每个设备有一块 256 字节（PCIe 扩展到 4 KB）的标准寄存器，开机枚举、驱动匹配都靠它：

| 偏移 | 内容 | 本板网卡的值 |
|---|---|---|
| 0x00 | 厂商 ID | 0x10ec（Realtek） |
| 0x02 | 设备 ID | 0x8168 |
| 0x04 | 命令寄存器：Memory Space、Bus Master、INTx Disable…… | 驱动使能后置位 |
| 0x09 | 类别码 | 0x020000（网络控制器 / 以太网） |
| 0x10～0x24 | 6 个 BAR | BAR0 I/O 256 B，BAR2 内存 4 KB，BAR4 内存 16 KB |
| 0x34 起 | 能力链表 | 电源管理、MSI、PCIe、MSI-X…… |

`sudo lspci -vvv -s 0008:01:00.0` 能看到全部解码后的内容。

**三种中断：**

- **INTx**：模拟老 PCI 的 4 根中断线。PCIe 上没有真的线，设备发一个「拉低 / 释放」的消息。是电平式的，可以几个设备共享；
- **MSI**：设备往一个特定的内存地址写一个特定的数值，中断控制器看到这次写，就产生中断。是边沿式的，不共享；
- **MSI-X**：MSI 的加强版，每个设备可以有很多个向量（这块网卡有 4 个），每个向量的地址和数值放在 BAR 里的一张表中。多队列网卡、NVMe 靠它让每个 CPU 核有自己的中断。

**DMA 和 IOMMU**：设备 DMA 用的地址不是物理地址，而是经过 **SMMU**（ARM 的 IOMMU）翻译的 **IOVA**。
驱动必须用 DMA API（`dma_alloc_coherent`、`dma_map_single`）拿到「设备看到的地址」再交给设备。设备访问没映射的地址，会被 SMMU 拦下来报错，而不是乱写内存（第 5 节有实测）。

## 2. 板子上的 PCIe

```
$ lspci -tv
-[0001:00]---00.0-[01-ff]----00.0  Realtek RTL8822CE 802.11ac PCIe Wireless   ← M.2 Key E：Wi-Fi，你的 ssh 可能走这个
-[0004:00]---00.0-[01-ff]----00.0  MAXIO NVMe SSD Controller                  ← M.2 Key M：系统盘
-[0008:00]---00.0-[01-ff]----00.0  Realtek RTL8111/8168 Gigabit Ethernet      ← 板载网口，本章用它
```

每个控制器下面是一个根端口（NVIDIA 的桥，`00.0`），再下面是设备。网卡的链路：根端口支持 8 GT/s x2，网卡只支持 **2.5 GT/s x1**（Gen1 一条通道），协商结果就是 Gen1 x1。
扣掉 8b/10b 编码，有效带宽是 2 Gbit/s，跑千兆网络足够。

> 只用网卡做实验。**绝对不要**解绑 NVMe（系统盘，立刻死机），也不要动 Wi-Fi（ssh 会断）。

## 3. 驱动绑定的手动操作

驱动和设备怎么配对，平时由内核自动完成，但每一步都可以手动做：

```bash
S=/sys/bus/pci/devices/0008:01:00.0
readlink $S/driver                                          # 现在是谁在驱动：r8168
echo 0008:01:00.0 | sudo tee /sys/bus/pci/drivers/r8168/unbind     # 解绑：网口 enP8p1s0 消失
echo tutorial-pci | sudo tee $S/driver_override             # 指定：这个设备只能由 tutorial-pci 驱动
echo 0008:01:00.0 | sudo tee /sys/bus/pci/drivers_probe     # 让内核重新给它找驱动
# 还回去：
echo 0008:01:00.0 | sudo tee /sys/bus/pci/drivers/tutorial-pci/unbind
echo | sudo tee $S/driver_override                          # 清掉指定
echo 0008:01:00.0 | sudo tee /sys/bus/pci/drivers/r8168/bind
```

`tpci` 故意**不写** `MODULE_DEVICE_TABLE`，这样 udev 不会在开机时自动加载它去抢网卡。这种「接管别人的设备做实验」的方法，调试真实驱动时也很常用，比如把设备交给 `vfio-pci` 再在用户态访问。

## 4. 驱动 `tpci` 逐段讲解

代码在 `tpci/tpci.c`。寄存器偏移全部来自内核主线的 `drivers/net/ethernet/realtek/r8169_main.c`，遇到不懂的寄存器就去那里搜。

**使能和映射**：

```c
pcim_enable_device(pdev);                    // 进入 D0 电源状态，打开 Memory Space
pcim_iomap_regions(pdev, BIT(2), "tutorial-pci");   // 申请并映射 BAR2
t->mmio = pcim_iomap_table(pdev)[2];
```

`pcim_` 开头的函数都是自动管理的，remove 时自动撤销，和第 2 章的 `devm_` 一样。
BAR 不要写死成 2，代码里是循环找第一个内存型、至少 256 字节的 BAR。这块芯片的 BAR0 是 I/O 端口，ARM 上基本不用。

**读寄存器**：`ioread8/16/32`、`iowrite8/16/32`。不要直接对指针解引用：这些函数保证访问宽度、字节序和顺序。

- **MAC 地址**：寄存器 0x00～0x05。和 `ip link` 显示的一致，说明映射对了；
- **芯片型号**：寄存器 0x40 的高位，`(val >> 20) & 0xfcf` = 0x541，在 r8169 里对应 RTL8168H；
- **PHY**：网卡芯片里集成了一个 PHY（模拟收发部分），有自己的一套 MII 标准寄存器。新一代芯片要通过 0xb8 这个「窗口」寄存器间接访问：
  把地址写进去，轮询最高位，等它变成 1，再读低 16 位。`readl_poll_timeout()` 就是专门做这种轮询的。
  读出 PHY ID `001c:c8xx`，001c 是 Realtek 的 OUI。链路状态在 BMSR 寄存器里，要**读两次**：这一位是「锁存低」，掉过线会一直保持 0，直到被读一次。

**中断**：

```c
pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_LEGACY);   // 或 PCI_IRQ_MSIX / PCI_IRQ_MSI / PCI_IRQ_ALL_TYPES
irq = pci_irq_vector(pdev, 0);
devm_request_irq(dev, irq, tpci_irq, IRQF_SHARED, "tutorial-pci", t);
```

中断源用芯片内部的定时器：往 0x58 写一个比较值，计数器开始从 0 计数（实测约 5 ms 走 61 万），计到这个值就在状态寄存器里置 bit14，并发出中断。
处理函数里读状态寄存器、写 1 清除、关掉定时器。INTx 是共享的，所以状态是 0 时要返回 `IRQ_NONE`，告诉内核「不是我的」。

**实测发现**：在本驱动这点初始化下，MSI 和 MSI-X 向量都能申请到，但触发 200 次，一次中断都收不到；INTx 200 次全收到。所以 `tpci` 默认用 INTx（模块参数 `irq_mode=3`）。
原驱动 r8168 用的是 MSI-X 而且工作正常，说明 MSI 消息的发送还依赖某些初始化步骤，具体差在哪一步留作练习 1。
另外，芯片的「强制软件中断」位（TxPoll bit0）在这种状态下也不起作用，所以才改用定时器。

**DMA**：

```c
pci_set_master(pdev);                                   // 允许设备发起 DMA
dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));       // 设备能访问 64 位地址
t->tally = dmam_alloc_coherent(dev, 64, &t->tally_dma, GFP_KERNEL);
// t->tally：CPU 用的地址；t->tally_dma：设备用的地址（IOVA，实测 0xffffe000）
```

读 `counters` 时：先把缓冲区填满 0xaa，把 `tally_dma` 写进寄存器 0x14/0x10，置「导出」位。
芯片把 64 字节统计计数器 DMA 写进这块内存，然后自己清掉这一位。最后检查 0xaa 被覆盖了，就证明 DMA 真的发生了。
没插过网线，所以计数都是 0。

## 5. 实测踩到的坑：失效的 DMA 地址

调试时我写过一个临时模块，短暂打开了网卡的接收器。r8168 解绑前把它的接收描述符地址（IOVA 0xfffe8000）留在了芯片寄存器里，而这个地址已经随 r8168 一起被取消映射了。芯片立刻去读，SMMU 当场拦截：

```
arm-smmu 8000000.iommu: Unhandled context fault: fsr=0x402, iova=0xfffe8000, fsynr=0x230003, cbfrsynra=0x809, cb=20
```

两个教训：

1. **IOMMU 是你的安全网**：没有它，芯片会按一个过期的地址往内存里乱写，现象是莫名其妙的内存损坏，极难查；
2. **接管一个别人用过的设备，要假设寄存器里全是垃圾**：使能 DMA 引擎之前，先把所有地址寄存器设成自己申请的缓冲区。`tpci` 从来不打开收发器，就是这个原因。

## 6. 测试

```bash
cd 11-pcie/tpci
make && ./test.sh
```

TODO 一共 4 个：用 `lspci` 找设备、用 `setpci` 读配置空间、用 `driver_override` 绑定、通过 sysfs 触发 100 次中断。
脚本退出时（包括中途出错、Ctrl-C）一定会把网卡还给 r8168。

实测：15 项全过。中断 100 次全收到，`/proc/interrupts` 也多了 100 次；DMA 地址 0xffffe000 被芯片写入；最后网口恢复。

## 7. 一个真正的网卡驱动还多了什么

`tpci` 只摸到了网卡的「外壳」。r8169（`r8169_main.c` 一个文件就有 5536 行）还要做这些：

| 部分 | 做什么 | 本教程里的相关章节 |
|---|---|---|
| 网络设备 | `alloc_etherdev`、`ndo_open`、`ndo_start_xmit` | 第 8 章的虚拟 CAN 网卡是同一套接口 |
| 描述符环 | TX、RX 各一个环形数组，每项是「缓冲区 DMA 地址 + 长度 + OWN 位」。驱动和芯片靠 OWN 位交接 | 本章的 DMA |
| NAPI | 中断只用来「叫醒」，然后关中断、批量轮询收包，收完再开中断。高负载时比每包一个中断高效得多 | 本章的中断 |
| PHY 管理 | 自协商、速率、双工、掉线检测，一般交给内核的 phylib | 本章的 PHY 读 |
| ethtool | `ethtool enP8p1s0` 看速率，`ethtool -S` 看统计计数器（就是本章 DMA 出来的那些），`ethtool -i` 看驱动 | — |

本板实际用的不是主线的 r8169，而是 Realtek 官方的 `r8168`（`ethtool -i enP8p1s0` 显示 8.053.00-NAPI），由 NVIDIA 打包在 `/lib/modules/.../updates/` 里。

## 8. 练习

1. **MSI 之谜**：用 `sudo insmod tpci.ko irq_mode=1`（MSI-X）重新测，确认收不到中断。读 r8169 的 `rtl_open()` 和 `rtl_hw_start()`，找出是哪一步初始化让 MSI 能正常发出。这道题我没做出来，欢迎你来解；
2. **插网线**：网口接到路由器，`cat link_state` 应该变成 `up`。再给 IMR 加上 LinkChg（bit5），实现拔插网线时触发中断。PHY 可能处于省电状态，需要先写 BMCR 寄存器把它唤醒；
3. **计数器对比**：插着网线、让 r8168 驱动跑一会儿网络，再借给 tpci，读 `counters`，和刚才 `ethtool -S enP8p1s0` 的数比较；
4. **算带宽**：从 `lspci -vvv` 的 LnkCap、LnkSta 读出链路速率和宽度，算一算 Gen1 x1 的有效带宽，够不够跑满千兆网？NVMe 那个控制器的链路又是多少？

## 本章文件

```
tpci/tpci.c     PCI 驱动（BAR、寄存器、PHY、中断、DMA）
tpci/test.sh    测试（4 个 TODO），自动借用和归还网卡
```
