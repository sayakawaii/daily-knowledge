// SPDX-License-Identifier: GPL-2.0
/*
 * tspi_sim：一个虚拟的 SPI 控制器，片选 0 上「挂着」一颗模拟的 ADXL345 加速度计。
 * 用来在没有硬件时测试 tadxl 驱动。它本身也是一个 SPI 控制器驱动的最小例子。
 *
 * 模拟的 ADXL345 行为：
 *   - 每条 SPI 消息的第一个字节是命令：bit7 = 1 读、0 写；bit6 = 1 多字节（地址自动加 1）；bit5~0 = 寄存器地址
 *   - 寄存器 0x00 DEVID = 0xE5，只读
 *   - 0x2D POWER_CTL 的 bit3（Measure）为 0 时，数据寄存器 0x32~0x37 读出来是 0
 *   - 必须是 SPI 模式 3（CPOL = 1，CPHA = 1），否则所有读到的字节都是 0xFF（模拟时序不对时读到乱码）
 *
 * 控制模拟器（sysfs，/sys/devices/platform/tutorial-spi-sim/）：
 *   accel     写 "x y z"（原始计数值，满分辨率下 1 = 3.9 mg），读出当前值
 *   regdump   读出 64 个寄存器
 *   xfers     收到的 SPI 消息条数
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/mutex.h>

#define ADXL_DEVID		0x00
#define ADXL_POWER_CTL		0x2d
#define ADXL_DATAX0		0x32
#define ADXL_NREGS		0x40

struct tspi_sim {
	struct mutex lock;
	u8 regs[ADXL_NREGS];
	s16 accel[3];
	unsigned long xfers;
};

static struct platform_device *sim_pdev;

/* 调用者持有 lock */
static u8 sim_read_reg(struct tspi_sim *s, u8 reg)
{
	if (reg >= ADXL_DATAX0 && reg < ADXL_DATAX0 + 6) {
		int axis = (reg - ADXL_DATAX0) / 2;
		u16 v = (s->regs[ADXL_POWER_CTL] & 0x08) ? (u16)s->accel[axis] : 0;

		return (reg & 1) ? v >> 8 : v & 0xff;	/* 低字节在前：X0 是低字节，X1 是高字节 */
	}
	return reg < ADXL_NREGS ? s->regs[reg] : 0;
}

static void sim_write_reg(struct tspi_sim *s, u8 reg, u8 val)
{
	if (reg == ADXL_DEVID || reg >= ADXL_NREGS)
		return;
	s->regs[reg] = val;
}

/*
 * SPI 核心把一条消息（若干个 spi_transfer，期间片选一直有效）交给控制器。
 * 真控制器在这里配置寄存器、启动 DMA、等中断；我们直接在内存里模拟芯片。
 */
static int tspi_sim_transfer_one_message(struct spi_controller *ctlr, struct spi_message *msg)
{
	struct tspi_sim *s = spi_controller_get_devdata(ctlr);
	bool mode_ok = (msg->spi->mode & SPI_MODE_X_MASK) == SPI_MODE_3;
	struct spi_transfer *t;
	bool have_cmd = false, rd = false, mb = false;
	u8 reg = 0;
	int i;

	mutex_lock(&s->lock);
	s->xfers++;
	list_for_each_entry(t, &msg->transfers, transfer_list) {
		const u8 *tx = t->tx_buf;
		u8 *rx = t->rx_buf;

		for (i = 0; i < t->len; i++) {
			u8 in = tx ? tx[i] : 0;	/* 只读传输时主机发的是 0 */
			u8 out = 0xff;		/* 命令字节期间芯片的 MISO 是高阻，读到 0xFF */

			if (!have_cmd) {
				have_cmd = true;
				rd = in & 0x80;
				mb = in & 0x40;
				reg = in & 0x3f;
			} else {
				if (rd)
					out = sim_read_reg(s, reg);
				else
					sim_write_reg(s, reg, in);
				if (mb)
					reg = (reg + 1) & 0x3f;
			}
			if (rx)
				rx[i] = mode_ok ? out : 0xff;
		}
		msg->actual_length += t->len;
	}
	mutex_unlock(&s->lock);

	msg->status = 0;
	spi_finalize_current_message(ctlr);	/* 告诉 SPI 核心：这条消息处理完了 */
	return 0;
}

static ssize_t accel_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tspi_sim *s = spi_controller_get_devdata(dev_get_drvdata(dev));

	return sysfs_emit(buf, "%d %d %d\n", s->accel[0], s->accel[1], s->accel[2]);
}

static ssize_t accel_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct tspi_sim *s = spi_controller_get_devdata(dev_get_drvdata(dev));
	int x, y, z;

	if (sscanf(buf, "%d %d %d", &x, &y, &z) != 3)
		return -EINVAL;
	/* 满分辨率 ±2 g 时是 10 位有符号数 */
	if (abs(x) > 511 || abs(y) > 511 || abs(z) > 511)
		return -ERANGE;
	mutex_lock(&s->lock);
	s->accel[0] = x;
	s->accel[1] = y;
	s->accel[2] = z;
	mutex_unlock(&s->lock);
	return count;
}
static DEVICE_ATTR_RW(accel);

static ssize_t regdump_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tspi_sim *s = spi_controller_get_devdata(dev_get_drvdata(dev));
	int i, n = 0;

	mutex_lock(&s->lock);
	for (i = 0; i < ADXL_NREGS; i++)
		n += sysfs_emit_at(buf, n, "%02x%c", s->regs[i], (i % 16 == 15) ? '\n' : ' ');
	mutex_unlock(&s->lock);
	return n;
}
static DEVICE_ATTR_RO(regdump);

static ssize_t xfers_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tspi_sim *s = spi_controller_get_devdata(dev_get_drvdata(dev));

	return sysfs_emit(buf, "%lu\n", s->xfers);
}
static DEVICE_ATTR_RO(xfers);

static struct attribute *sim_attrs[] = {
	&dev_attr_accel.attr,
	&dev_attr_regdump.attr,
	&dev_attr_xfers.attr,
	NULL,
};
ATTRIBUTE_GROUPS(sim);

static int tspi_sim_probe(struct platform_device *pdev)
{
	struct spi_board_info info = {
		.modalias = "tutorial-adxl345",
		.max_speed_hz = 5000000,
		.chip_select = 0,
		.mode = SPI_MODE_0,	/* 故意不是模式 3：驱动必须自己设对 */
	};
	struct spi_controller *ctlr;
	struct tspi_sim *s;
	int ret;

	ctlr = devm_spi_alloc_host(&pdev->dev, sizeof(*s));
	if (!ctlr)
		return -ENOMEM;
	s = spi_controller_get_devdata(ctlr);
	mutex_init(&s->lock);
	s->regs[ADXL_DEVID] = 0xe5;
	s->regs[0x2c] = 0x0a;		/* BW_RATE 复位值 */
	s->accel[2] = 256;		/* 平放：Z 轴 1 g（满分辨率 256 LSB/g） */

	ctlr->bus_num = -1;		/* 自动分配总线号 */
	ctlr->num_chipselect = 1;
	ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->max_speed_hz = 10000000;
	ctlr->transfer_one_message = tspi_sim_transfer_one_message;
	platform_set_drvdata(pdev, ctlr);

	ret = devm_spi_register_controller(&pdev->dev, ctlr);
	if (ret)
		return ret;

	/* 相当于设备树里控制器节点下的一个子节点 */
	if (!spi_new_device(ctlr, &info))
		return -ENODEV;

	dev_info(&pdev->dev, "virtual SPI bus %d with simulated ADXL345 on CS0\n", ctlr->bus_num);
	return 0;
}

static struct platform_driver tspi_sim_driver = {
	.probe = tspi_sim_probe,
	.driver = {
		.name = "tutorial-spi-sim",
		.dev_groups = sim_groups,
	},
};

static int __init tspi_sim_init(void)
{
	int ret = platform_driver_register(&tspi_sim_driver);

	if (ret)
		return ret;
	sim_pdev = platform_device_register_simple("tutorial-spi-sim", PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(sim_pdev)) {
		platform_driver_unregister(&tspi_sim_driver);
		return PTR_ERR(sim_pdev);
	}
	return 0;
}

static void __exit tspi_sim_exit(void)
{
	/* 控制器注销时，SPI 核心会自动注销它下面的所有 SPI 设备 */
	platform_device_unregister(sim_pdev);
	platform_driver_unregister(&tspi_sim_driver);
}

module_init(tspi_sim_init);
module_exit(tspi_sim_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("jetson-drivers ch06: virtual SPI controller with simulated ADXL345");
