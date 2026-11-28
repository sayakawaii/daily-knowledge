// SPDX-License-Identifier: GPL-2.0
/*
 * tadxl：ADXL345 三轴加速度计的 SPI 驱动，注册成 IIO 设备。
 *
 * ADXL345 的 SPI 协议：
 *   - 模式 3（CPOL = 1，CPHA = 1），最高 5 MHz
 *   - 第一个字节：bit7 = 1 读、0 写；bit6 = 1 多字节连续读写；bit5~0 = 寄存器地址
 *   - 0x00 DEVID = 0xE5；0x2D POWER_CTL bit3 = 开始测量；0x31 DATA_FORMAT bit3 = 满分辨率
 *   - 0x32~0x37：X0 X1 Y0 Y1 Z0 Z1，16 位，低字节在前，补码；满分辨率下 3.9 mg/LSB
 *
 * IIO（/sys/bus/iio/devices/iio:deviceN/）：
 *   in_accel_x_raw、in_accel_y_raw、in_accel_z_raw   原始值
 *   in_accel_scale                                    原始值 × scale = m/s²
 *
 * 绑定：设备树 compatible "tutorial,spi-demo"（教程覆盖层里 SPI1 片选 0），
 *       或 tspi_sim 模拟器上名为 tutorial-adxl345 的设备。
 */
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/iio/iio.h>
#include <asm/unaligned.h>

#define ADXL_DEVID		0x00
#define ADXL_DEVID_VAL		0xe5
#define ADXL_POWER_CTL		0x2d
#define ADXL_MEASURE		BIT(3)
#define ADXL_DATA_FORMAT	0x31
#define ADXL_FULL_RES		BIT(3)
#define ADXL_DATAX0		0x32

#define ADXL_READ		BIT(7)
#define ADXL_MB			BIT(6)

static int spi_mode = 3;
module_param(spi_mode, int, 0444);
MODULE_PARM_DESC(spi_mode, "SPI 模式 0~3，芯片要求 3；改成别的值可以看到模式不对时会发生什么");

struct tadxl {
	struct spi_device *spi;
};

static int tadxl_write(struct spi_device *spi, u8 reg, u8 val)
{
	u8 buf[2] = { reg & 0x3f, val };

	return spi_write(spi, buf, sizeof(buf));
}

/*
 * 读 len 个连续寄存器。spi_write_then_read：先发 1 字节命令，再读 len 字节，整个过程片选一直有效。
 * 它内部用自己的缓冲区，所以 buf 可以在栈上（直接给 SPI 控制器做 DMA 的缓冲区不能在栈上）。
 */
static int tadxl_read(struct spi_device *spi, u8 reg, u8 *buf, size_t len)
{
	u8 cmd = ADXL_READ | (len > 1 ? ADXL_MB : 0) | (reg & 0x3f);

	return spi_write_then_read(spi, &cmd, 1, buf, len);
}

static int tadxl_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			  int *val, int *val2, long mask)
{
	struct tadxl *d = iio_priv(indio_dev);
	u8 buf[2];
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = tadxl_read(d->spi, chan->address, buf, 2);
		if (ret)
			return ret;
		*val = (s16)get_unaligned_le16(buf);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		/* 3.9 mg × 9.80665 = 0.038245935 m/s²，IIO 用「整数 + 微单位」表示小数 */
		*val = 0;
		*val2 = 38246;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

#define TADXL_CHAN(axis, idx) {						\
	.type = IIO_ACCEL,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##axis,					\
	.address = ADXL_DATAX0 + 2 * (idx),				\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
}

static const struct iio_chan_spec tadxl_channels[] = {
	TADXL_CHAN(X, 0),
	TADXL_CHAN(Y, 1),
	TADXL_CHAN(Z, 2),
};

static const struct iio_info tadxl_info = {
	.read_raw = tadxl_read_raw,
};

static void tadxl_standby(void *data)
{
	tadxl_write(data, ADXL_POWER_CTL, 0);
}

static int tadxl_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct iio_dev *indio_dev;
	struct tadxl *d;
	u8 id;
	int ret;

	/* 芯片要求模式 3、最高 5 MHz；设备树或板级信息里写的不一定对，驱动自己设好 */
	spi->mode = (spi->mode & ~SPI_MODE_X_MASK) | (spi_mode & SPI_MODE_X_MASK);
	if (!spi->max_speed_hz || spi->max_speed_hz > 5000000)
		spi->max_speed_hz = 5000000;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "spi_setup\n");

	ret = tadxl_read(spi, ADXL_DEVID, &id, 1);
	if (ret)
		return ret;
	if (id != ADXL_DEVID_VAL)
		return dev_err_probe(dev, -ENODEV, "DEVID 0x%02x, expected 0x%02x\n",
				     id, ADXL_DEVID_VAL);

	indio_dev = devm_iio_device_alloc(dev, sizeof(*d));
	if (!indio_dev)
		return -ENOMEM;
	d = iio_priv(indio_dev);
	d->spi = spi;

	ret = tadxl_write(spi, ADXL_DATA_FORMAT, ADXL_FULL_RES);	/* 满分辨率，±2 g */
	if (ret)
		return ret;
	ret = tadxl_write(spi, ADXL_POWER_CTL, ADXL_MEASURE);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, tadxl_standby, spi);
	if (ret)
		return ret;

	indio_dev->name = "tutorial-adxl345";
	indio_dev->info = &tadxl_info;
	indio_dev->channels = tadxl_channels;
	indio_dev->num_channels = ARRAY_SIZE(tadxl_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return ret;

	dev_info(dev, "ADXL345 ready, %u Hz SPI mode %u\n", spi->max_speed_hz,
		 (u32)(spi->mode & SPI_MODE_X_MASK));
	return 0;
}

static const struct spi_device_id tadxl_ids[] = {
	{ "tutorial-adxl345" },
	{ "spi-demo" },		/* 设备树创建的设备名是 compatible 逗号后的部分 */
	{ }
};
MODULE_DEVICE_TABLE(spi, tadxl_ids);

static const struct of_device_id tadxl_of_match[] = {
	{ .compatible = "tutorial,spi-demo" },
	{ }
};
MODULE_DEVICE_TABLE(of, tadxl_of_match);

static struct spi_driver tadxl_driver = {
	.driver = {
		.name = "tutorial-adxl345",
		.of_match_table = tadxl_of_match,
	},
	.probe = tadxl_probe,
	.id_table = tadxl_ids,
};
module_spi_driver(tadxl_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("jetson-drivers ch06: ADXL345 accelerometer over SPI (IIO)");
