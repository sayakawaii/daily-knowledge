// SPDX-License-Identifier: GPL-2.0
/*
 * tpci：PCIe 驱动入门。借板载的千兆网卡芯片 RTL8111H（10ec:8168）演示 PCIe 驱动的全部基本功：
 *
 *   配置空间 → 使能设备 → 映射 BAR（MMIO 寄存器）→ 读寄存器（MAC 地址、芯片型号、PHY）
 *   → 申请 MSI/MSI-X 中断，用芯片内部的定时器触发它 → 申请 DMA 内存，让芯片把统计计数器 DMA 写进来
 *
 * 不收发网络包，所以不会和网络打架；测试时把网卡从原驱动 r8168 那里临时借过来，用完还回去。
 * 寄存器偏移和用法来自内核主线的 drivers/net/ethernet/realtek/r8169_main.c。
 *
 * sysfs（在 PCI 设备目录下）：
 *   bar  mac  chip_xid  phy_id  link_state  irq_type  irq_count  last_isr  counters（读）  trigger_irq（写次数）
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/etherdevice.h>
#include <linux/mii.h>

#define R_MAC0		0x00	/* 6 字节 MAC 地址 */
#define R_CNT_LO	0x10	/* 计数器 DMA 地址低 32 位，bit3 = 开始导出 */
#define R_CNT_HI	0x14	/* 计数器 DMA 地址高 32 位 */
#define R_TCTR		0x48	/* 芯片内部定时器的计数值，写任意值 = 清零重新计数 */
#define R_IMR		0x3c	/* 中断屏蔽（16 位），置 1 = 允许 */
#define R_ISR		0x3e	/* 中断状态（16 位），写 1 清除 */
#define R_TXCFG		0x40	/* 高位里是芯片型号 XID */
#define R_GPHY_OCP	0xb8	/* 8168g 以后的芯片通过它访问 PHY 寄存器 */
#define R_INTR_MITIGATE	0xe2	/* 中断合并：攒够包数或等到超时才发中断；0 = 立即发 */
#define R_TIMERINT	0x58	/* 计数到这个值时产生超时中断；0 = 关闭定时器 */

#define CNT_DUMP	BIT(3)
#define INT_TIMEOUT	BIT(14)	/* 定时器超时 */
#define OCP_FLAG	BIT(31)
#define PHY_OCP_BASE	0xa400	/* 标准 PHY 寄存器 n 在 OCP 地址 0xa400 + 2n */

/*
 * 默认用传统 INTx。实测：只做了本驱动这点初始化时，这块芯片的 MSI 和 MSI-X 向量能申请到，
 * 但中断消息基本发不出来（触发 200 次收到 0 次）；INTx 200 次全收到。
 * 原驱动 r8168 做完整初始化后 MSI-X 是正常的，差在哪一步见 README 的练习。
 */
static int irq_mode = 3;
module_param(irq_mode, int, 0444);
MODULE_PARM_DESC(irq_mode, "0 = auto (MSI-X > MSI > INTx), 1 = MSI-X, 2 = MSI, 3 = INTx (default)");

/* 芯片 DMA 写进内存的统计计数器，64 字节，小端 */
struct tpci_tally {
	__le64 tx_packets, rx_packets, tx_errors;
	__le32 rx_errors;
	__le16 rx_missed, align_errors;
	__le32 tx_one_collision, tx_multi_collision;
	__le64 rx_unicast, rx_broadcast;
	__le32 rx_multicast;
	__le16 tx_aborted, tx_underrun;
};
static_assert(sizeof(struct tpci_tally) == 64);

struct tpci {
	struct pci_dev *pdev;
	void __iomem *mmio;
	int bar;
	int irq;
	atomic_t irq_count;
	u16 last_isr;
	struct mutex lock;		/* 串行化 PHY 访问和计数器导出 */
	struct tpci_tally *tally;	/* CPU 看到的地址 */
	dma_addr_t tally_dma;		/* 设备看到的地址（经过 SMMU 翻译的 IOVA） */
};

static irqreturn_t tpci_irq(int irq, void *data)
{
	struct tpci *t = data;
	u16 isr = ioread16(t->mmio + R_ISR);

	/* INTx 是共享中断线，不是自己的要说 IRQ_NONE；读到全 1 说明设备已经不在了 */
	if (!isr || isr == 0xffff)
		return IRQ_NONE;
	/*
	 * 和 r8169 一样，进来先清屏蔽、处理完再恢复。MSI/MSI-X 是「边沿」式的：
	 * 芯片只在（状态 & 屏蔽）从 0 变成非 0 时发一条消息，这样做能保证处理期间新来的事件不会丢。
	 */
	iowrite16(0, t->mmio + R_IMR);
	if (isr & INT_TIMEOUT)
		iowrite32(0, t->mmio + R_TIMERINT);	/* 定时器只用一次：关掉，否则还会再触发 */
	iowrite16(isr, t->mmio + R_ISR);	/* 写 1 清除 */
	t->last_isr = isr;
	atomic_inc(&t->irq_count);
	iowrite16(INT_TIMEOUT, t->mmio + R_IMR);
	return IRQ_HANDLED;
}

static int tpci_phy_read(struct tpci *t, int reg)
{
	u32 v;
	int ret;

	iowrite32((PHY_OCP_BASE + reg * 2) << 15, t->mmio + R_GPHY_OCP);
	ret = readl_poll_timeout(t->mmio + R_GPHY_OCP, v, v & OCP_FLAG, 25, 2000);
	return ret ? ret : (int)(v & 0xffff);
}

static struct tpci *to_tpci(struct device *dev)
{
	return pci_get_drvdata(to_pci_dev(dev));
}

static ssize_t bar_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tpci *t = to_tpci(dev);
	resource_size_t start = pci_resource_start(t->pdev, t->bar);

	return sysfs_emit(buf, "%d %pa %llu\n", t->bar, &start,
			  (unsigned long long)pci_resource_len(t->pdev, t->bar));
}
static DEVICE_ATTR_RO(bar);

static ssize_t mac_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tpci *t = to_tpci(dev);
	u8 mac[ETH_ALEN];
	int i;

	for (i = 0; i < ETH_ALEN; i++)
		mac[i] = ioread8(t->mmio + R_MAC0 + i);
	return sysfs_emit(buf, "%pM\n", mac);
}
static DEVICE_ATTR_RO(mac);

static ssize_t chip_xid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0x%03x\n", (ioread32(to_tpci(dev)->mmio + R_TXCFG) >> 20) & 0xfcf);
}
static DEVICE_ATTR_RO(chip_xid);

static ssize_t phy_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tpci *t = to_tpci(dev);
	int id1, id2;

	mutex_lock(&t->lock);
	id1 = tpci_phy_read(t, MII_PHYSID1);
	id2 = tpci_phy_read(t, MII_PHYSID2);
	mutex_unlock(&t->lock);
	if (id1 < 0 || id2 < 0)
		return -ETIMEDOUT;
	return sysfs_emit(buf, "%04x:%04x\n", id1, id2);
}
static DEVICE_ATTR_RO(phy_id);

static ssize_t link_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tpci *t = to_tpci(dev);
	int bmsr;

	mutex_lock(&t->lock);
	/* BMSR 的 link 位是「锁存低」：掉过线就保持 0，直到被读一次。所以读两次，第二次才是现在的状态 */
	tpci_phy_read(t, MII_BMSR);
	bmsr = tpci_phy_read(t, MII_BMSR);
	mutex_unlock(&t->lock);
	if (bmsr < 0)
		return bmsr;
	return sysfs_emit(buf, "%s\n", (bmsr & BMSR_LSTATUS) ? "up" : "down");
}
static DEVICE_ATTR_RO(link_state);

static ssize_t irq_type_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	return sysfs_emit(buf, "%s\n", pdev->msix_enabled ? "MSI-X" : pdev->msi_enabled ? "MSI" : "INTx");
}
static DEVICE_ATTR_RO(irq_type);

static ssize_t irq_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&to_tpci(dev)->irq_count));
}
static DEVICE_ATTR_RO(irq_count);

static ssize_t last_isr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0x%04x\n", to_tpci(dev)->last_isr);
}
static DEVICE_ATTR_RO(last_isr);

/*
 * 写一个次数 n：让芯片产生 n 次中断，每次都等中断处理函数真的执行了再发下一次。
 * 中断源用芯片内部的定时器：设好比较值，把计数器清零，大约 100 µs 后芯片发出中断。
 * （这块芯片还有一个「强制软件中断」位 TxPoll bit0，但在本驱动这点初始化下不起作用，实测过。）
 */
static ssize_t trigger_irq_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct tpci *t = to_tpci(dev);
	unsigned int n, i;
	int before, now;

	if (kstrtouint(buf, 0, &n) || n < 1 || n > 1000)
		return -EINVAL;
	for (i = 0; i < n; i++) {
		before = atomic_read(&t->irq_count);
		iowrite32(12500, t->mmio + R_TIMERINT);
		iowrite32(0, t->mmio + R_TCTR);
		if (read_poll_timeout(atomic_read, now, now != before, 10, 10000, false, &t->irq_count))
			return -ETIMEDOUT;
	}
	return count;
}
static DEVICE_ATTR_WO(trigger_irq);

/* 读一次就让芯片导出一次计数器：把 DMA 地址告诉芯片，置 CNT_DUMP，芯片写完内存后自己清掉这一位 */
static ssize_t counters_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tpci *t = to_tpci(dev);
	struct tpci_tally *c = t->tally;
	u32 lo = lower_32_bits(t->tally_dma), v;
	bool written;
	int ret;

	mutex_lock(&t->lock);
	memset(c, 0xaa, sizeof(*c));	/* 先填上标记，导出后还是 0xaa 就说明芯片没写 */
	iowrite32(upper_32_bits(t->tally_dma), t->mmio + R_CNT_HI);
	iowrite32(lo, t->mmio + R_CNT_LO);
	iowrite32(lo | CNT_DUMP, t->mmio + R_CNT_LO);
	ret = readl_poll_timeout(t->mmio + R_CNT_LO, v, !(v & CNT_DUMP), 10, 10000);
	written = memchr_inv(c, 0xaa, sizeof(*c)) != NULL;
	mutex_unlock(&t->lock);
	if (ret)
		return ret;

	return sysfs_emit(buf, "dma_addr %pad\ndma_written %d\ntx_packets %llu\nrx_packets %llu\n"
			  "rx_broadcast %llu\nrx_multicast %u\n", &t->tally_dma, written,
			  le64_to_cpu(c->tx_packets), le64_to_cpu(c->rx_packets),
			  le64_to_cpu(c->rx_broadcast), le32_to_cpu(c->rx_multicast));
}
static DEVICE_ATTR_RO(counters);

static struct attribute *tpci_attrs[] = {
	&dev_attr_bar.attr, &dev_attr_mac.attr, &dev_attr_chip_xid.attr, &dev_attr_phy_id.attr,
	&dev_attr_link_state.attr, &dev_attr_irq_type.attr, &dev_attr_irq_count.attr,
	&dev_attr_last_isr.attr, &dev_attr_trigger_irq.attr, &dev_attr_counters.attr, NULL
};
ATTRIBUTE_GROUPS(tpci);

static void tpci_mask_irqs(void *data)
{
	struct tpci *t = data;

	iowrite32(0, t->mmio + R_TIMERINT);
	iowrite16(0, t->mmio + R_IMR);
	iowrite16(0xffff, t->mmio + R_ISR);
}

static int tpci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct tpci *t;
	int ret, bar;

	t = devm_kzalloc(dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->pdev = pdev;
	mutex_init(&t->lock);
	atomic_set(&t->irq_count, 0);
	pci_set_drvdata(pdev, t);

	/* 1. 使能：让设备进入 D0，打开配置空间命令寄存器里的 Memory Space 位。pcim_ 开头的都会在 remove 时自动撤销 */
	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	/* 2. 找第一个内存型 BAR（这块芯片的 BAR0 是 I/O 端口，BAR2 才是 4 KB 的寄存器） */
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++)
		if ((pci_resource_flags(pdev, bar) & IORESOURCE_MEM) && pci_resource_len(pdev, bar) >= 256)
			break;
	if (bar == PCI_STD_NUM_BARS)
		return dev_err_probe(dev, -ENODEV, "no MMIO BAR\n");
	t->bar = bar;
	ret = pcim_iomap_regions(pdev, BIT(bar), "tutorial-pci");
	if (ret)
		return dev_err_probe(dev, ret, "cannot map BAR%d\n", bar);
	t->mmio = pcim_iomap_table(pdev)[bar];

	/* 3. DMA：允许设备当总线主控，告诉内核设备能访问 64 位地址，再申请一块 CPU 和设备都能看到的内存 */
	pci_set_master(pdev);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "no usable DMA mask\n");
	t->tally = dmam_alloc_coherent(dev, sizeof(*t->tally), &t->tally_dma, GFP_KERNEL);
	if (!t->tally)
		return -ENOMEM;

	/* 4. 中断：先在芯片里屏蔽并清掉所有中断，再申请向量。优先 MSI-X，其次 MSI，最后传统 INTx */
	tpci_mask_irqs(t);
	/* 原驱动 r8168 打开了中断合并（0xe2 = 0x5f51），我们要每次触发都立即收到，关掉 */
	iowrite16(0, t->mmio + R_INTR_MITIGATE);
	ret = pci_alloc_irq_vectors(pdev, 1, 1, irq_mode == 1 ? PCI_IRQ_MSIX : irq_mode == 2 ? PCI_IRQ_MSI :
				    irq_mode == 3 ? PCI_IRQ_LEGACY : PCI_IRQ_ALL_TYPES);
	if (ret < 0)
		return dev_err_probe(dev, ret, "no irq vector\n");
	t->irq = pci_irq_vector(pdev, 0);
	ret = devm_request_irq(dev, t->irq, tpci_irq, IRQF_SHARED, "tutorial-pci", t);
	if (ret)
		return ret;
	/* 在 request_irq 之后注册，remove 时就先于 free_irq 执行：先让芯片闭嘴，再释放中断 */
	ret = devm_add_action_or_reset(dev, tpci_mask_irqs, t);
	if (ret)
		return ret;
	iowrite16(INT_TIMEOUT, t->mmio + R_IMR);

	dev_info(dev, "BAR%d mapped, chip xid 0x%03x, irq %d (%s), dma %pad\n", bar,
		 (ioread32(t->mmio + R_TXCFG) >> 20) & 0xfcf, t->irq,
		 pdev->msix_enabled ? "MSI-X" : pdev->msi_enabled ? "MSI" : "INTx", &t->tally_dma);
	return 0;
}

static const struct pci_device_id tpci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x8168) },
	{ }
};
/*
 * 故意不写 MODULE_DEVICE_TABLE：不让 udev 在开机时自动加载它去抢网卡。
 * 测试时用 driver_override 明确指定由谁驱动这个设备。
 */

static struct pci_driver tpci_driver = {
	.name = "tutorial-pci",
	.id_table = tpci_ids,
	.probe = tpci_probe,
	.dev_groups = tpci_groups,
};
module_pci_driver(tpci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("jetson-drivers tutorial: PCIe basics on the RTL8111H");
