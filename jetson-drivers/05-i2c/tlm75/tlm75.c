// SPDX-License-Identifier: GPL-2.0
/*
 * tlm75：LM75A 温度传感器的 hwmon 驱动（淘宝 LM75A 模块几块钱一个）。
 *
 * LM75A 寄存器（都是高字节在前）：
 *   0x00 温度   16 位，高 11 位有效，0.125 ℃/LSB，补码
 *   0x01 配置   8 位，bit0 = 1 关断
 *   0x02 Thyst  16 位，高 9 位有效，0.5 ℃/LSB（过温报警解除温度）
 *   0x03 Tos    16 位，高 9 位有效，0.5 ℃/LSB（过温报警温度，OS 引脚动作）
 *
 * hwmon：temp1_input（只读）、temp1_max（Tos，读写）、temp1_max_hyst（Thyst，读写）
 *
 * 没有硬件时用 i2c-stub 模拟，见 test.sh。
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/mutex.h>

#define LM75_REG_TEMP	0x00
#define LM75_REG_CONF	0x01
#define LM75_REG_HYST	0x02
#define LM75_REG_TOS	0x03

struct tlm75 {
	struct i2c_client *client;
	struct mutex lock;	/* 读-改-写配置寄存器时用 */
};

/*
 * SMBus 规定「字」是低字节先传，LM75 是高字节先传。
 * i2c_smbus_read_word_swapped() = 读一个字再交换高低字节，得到正确的 16 位值。
 */
static int tlm75_read_reg(struct tlm75 *d, u8 reg, long *mc, int shift, int lsb_mc)
{
	int raw = i2c_smbus_read_word_swapped(d->client, reg);

	if (raw < 0)
		return raw;
	/* 先转成有符号 16 位，再算术右移，负温度的符号位才能保留 */
	*mc = ((s16)raw >> shift) * lsb_mc;
	return 0;
}

static int tlm75_write_limit(struct tlm75 *d, u8 reg, long mc)
{
	s16 v;

	mc = clamp_val(mc, -55000, 125000);		/* 芯片的测量范围 */
	v = DIV_ROUND_CLOSEST(mc, 500) << 7;		/* 0.5 ℃/LSB，放在高 9 位 */
	return i2c_smbus_write_word_swapped(d->client, reg, (u16)v);
}

static umode_t tlm75_is_visible(const void *data, enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	if (type != hwmon_temp)
		return 0;
	switch (attr) {
	case hwmon_temp_input:
		return 0444;
	case hwmon_temp_max:
	case hwmon_temp_max_hyst:
		return 0644;
	default:
		return 0;
	}
}

static int tlm75_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	struct tlm75 *d = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_temp_input:
		return tlm75_read_reg(d, LM75_REG_TEMP, val, 5, 125);
	case hwmon_temp_max:
		return tlm75_read_reg(d, LM75_REG_TOS, val, 7, 500);
	case hwmon_temp_max_hyst:
		return tlm75_read_reg(d, LM75_REG_HYST, val, 7, 500);
	default:
		return -EOPNOTSUPP;
	}
}

static int tlm75_write(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long val)
{
	struct tlm75 *d = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_temp_max:
		return tlm75_write_limit(d, LM75_REG_TOS, val);
	case hwmon_temp_max_hyst:
		return tlm75_write_limit(d, LM75_REG_HYST, val);
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops tlm75_ops = {
	.is_visible = tlm75_is_visible,
	.read = tlm75_read,
	.write = tlm75_write,
};

static const struct hwmon_channel_info * const tlm75_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MAX_HYST),
	NULL
};

static const struct hwmon_chip_info tlm75_chip = {
	.ops = &tlm75_ops,
	.info = tlm75_info,
};

static void tlm75_shutdown_chip(void *data)
{
	struct tlm75 *d = data;
	int conf;

	mutex_lock(&d->lock);
	conf = i2c_smbus_read_byte_data(d->client, LM75_REG_CONF);
	if (conf >= 0)
		i2c_smbus_write_byte_data(d->client, LM75_REG_CONF, conf | 0x01);
	mutex_unlock(&d->lock);
}

static int tlm75_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device *hwmon;
	struct tlm75 *d;
	int conf, ret;

	/* 用到的是 SMBus 读写字节、读写字，先确认适配器支持 */
	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA))
		return -EOPNOTSUPP;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->client = client;
	mutex_init(&d->lock);

	/* 读配置寄存器：读不到说明这个地址上没有芯片，probe 失败 */
	conf = i2c_smbus_read_byte_data(client, LM75_REG_CONF);
	if (conf < 0)
		return dev_err_probe(dev, conf, "no chip at 0x%02x\n", client->addr);

	/* 退出关断模式，开始测量 */
	ret = i2c_smbus_write_byte_data(client, LM75_REG_CONF, conf & ~0x01);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, tlm75_shutdown_chip, d);
	if (ret)
		return ret;

	hwmon = devm_hwmon_device_register_with_info(dev, "tutorial_lm75", d, &tlm75_chip, NULL);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);

	dev_info(dev, "lm75 at 0x%02x on %s\n", client->addr, client->adapter->name);
	return 0;
}

static const struct i2c_device_id tlm75_ids[] = {
	{ "tutorial-lm75" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tlm75_ids);

static const struct of_device_id tlm75_of_match[] = {
	{ .compatible = "tutorial,lm75" },
	{ }
};
MODULE_DEVICE_TABLE(of, tlm75_of_match);

static struct i2c_driver tlm75_driver = {
	.driver = {
		.name = "tutorial-lm75",
		.of_match_table = tlm75_of_match,
	},
	.probe = tlm75_probe,
	.id_table = tlm75_ids,
};
module_i2c_driver(tlm75_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("jetson-drivers ch05: LM75A temperature sensor");
