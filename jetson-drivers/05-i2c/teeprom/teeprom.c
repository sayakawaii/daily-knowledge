// SPDX-License-Identifier: GPL-2.0
/*
 * teeprom：Jetson 板载 EEPROM（24C02，256 字节）的只读驱动。
 * 本板 I2C 总线 0 上有两片：0x50 是模组的，0x57 是载板的，平时由内核的 at24 驱动管理。
 * 测试时把 0x57 从 at24 上解绑，再绑到这个驱动上。
 *
 * sysfs（/sys/bus/i2c/devices/0-0057/）：
 *   eeprom       256 字节原始数据（二进制属性）
 *   part_number  NVIDIA 板子料号（偏移 0x14 起的字符串）
 *   crc_ok       最后一个字节的 CRC-8 校验是否正确
 *
 * 只读！这个驱动没有任何写 EEPROM 的代码。EEPROM 里是板子身份信息，写坏了 NVIDIA 的软件可能认不出板子。
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/sysfs.h>
#include <linux/string.h>

#define EE_SIZE		256
#define EE_CHUNK	32		/* 每次 I2C 读多少字节 */
#define EE_PN_OFF	0x14
#define EE_PN_LEN	30

struct teeprom {
	u8 data[EE_SIZE];
	bool crc_ok;
	char part_number[EE_PN_LEN + 1];
};

/*
 * 从 offset 开始读 len 字节。一次 i2c_transfer 发两条消息：
 *   消息 1（写）：1 字节，告诉 EEPROM 从哪个地址开始
 *   消息 2（读）：len 字节
 * 两条消息之间是「重复起始条件」（repeated start），不发停止位，总线不会被别的主机抢走。
 * 波形：S 0x57+W A offset A Sr 0x57+R A data0 A data1 A ... dataN NA P
 */
static int teeprom_read(struct i2c_client *client, u8 offset, u8 *buf, u16 len)
{
	struct i2c_msg msgs[2] = {
		{ .addr = client->addr, .flags = 0,        .len = 1,   .buf = &offset },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;	/* 返回值是成功传输的消息条数 */
}

/* CRC-8/MAXIM：多项式 x^8+x^5+x^4+1，按低位先算（反射），所以用 0x31 反过来的 0x8c */
static u8 teeprom_crc8(const u8 *p, size_t len)
{
	u8 crc = 0;
	int i;

	while (len--) {
		crc ^= *p++;
		for (i = 0; i < 8; i++)
			crc = (crc & 1) ? (crc >> 1) ^ 0x8c : crc >> 1;
	}
	return crc;
}

static ssize_t eeprom_read(struct file *filp, struct kobject *kobj,
			   struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{
	struct teeprom *ee = dev_get_drvdata(kobj_to_dev(kobj));

	/* 内核已经按 attr->size 把 off、count 截好了，这里直接拷 */
	memcpy(buf, ee->data + off, count);
	return count;
}
static BIN_ATTR_RO(eeprom, EE_SIZE);

static ssize_t part_number_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct teeprom *ee = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", ee->part_number);
}
static DEVICE_ATTR_RO(part_number);

static ssize_t crc_ok_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct teeprom *ee = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", ee->crc_ok);
}
static DEVICE_ATTR_RO(crc_ok);

static struct attribute *teeprom_attrs[] = {
	&dev_attr_part_number.attr,
	&dev_attr_crc_ok.attr,
	NULL,
};
static struct bin_attribute *teeprom_bin_attrs[] = {
	&bin_attr_eeprom,
	NULL,
};
static const struct attribute_group teeprom_group = {
	.attrs = teeprom_attrs,
	.bin_attrs = teeprom_bin_attrs,
};
static const struct attribute_group *teeprom_groups[] = {
	&teeprom_group,
	NULL,
};

static int teeprom_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct teeprom *ee;
	int off, ret, i;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(dev, -EOPNOTSUPP, "adapter can't do plain I2C transfers\n");

	ee = devm_kzalloc(dev, sizeof(*ee), GFP_KERNEL);
	if (!ee)
		return -ENOMEM;

	for (off = 0; off < EE_SIZE; off += EE_CHUNK) {
		ret = teeprom_read(client, off, ee->data + off, EE_CHUNK);
		if (ret)
			return dev_err_probe(dev, ret, "read at 0x%02x\n", off);
	}

	ee->crc_ok = teeprom_crc8(ee->data, EE_SIZE - 1) == ee->data[EE_SIZE - 1];

	/* 料号是定长字段，没用完的部分是 0，可能没有结束符；只保留可打印字符 */
	for (i = 0; i < EE_PN_LEN && isprint(ee->data[EE_PN_OFF + i]); i++)
		ee->part_number[i] = ee->data[EE_PN_OFF + i];
	ee->part_number[i] = '\0';

	i2c_set_clientdata(client, ee);
	dev_info(dev, "part %s, crc %s\n", ee->part_number, ee->crc_ok ? "ok" : "BAD");
	return 0;
}

/*
 * 按名字匹配。从设备树创建的 I2C 设备，名字取 compatible 逗号后面的部分，
 * 所以 "atmel,24c02" 的设备名字叫 24c02。at24 驱动也认这个名字，开机时它先绑上了。
 */
static const struct i2c_device_id teeprom_ids[] = {
	{ "24c02" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, teeprom_ids);

static struct i2c_driver teeprom_driver = {
	.driver = {
		.name = "tutorial-eeprom",
		.dev_groups = teeprom_groups,
	},
	.probe = teeprom_probe,
	.id_table = teeprom_ids,
};
module_i2c_driver(teeprom_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("jetson-drivers ch05: read-only Jetson board EEPROM");
