// SPDX-License-Identifier: GPL-2.0
/*
 * dtdemo：最小的平台驱动。
 * 绑定教程覆盖层里的 tutorial-gpio / tutorial-pwm / tutorial-fan 三个节点，
 * 把设备树里的属性读出来，打印到内核日志，并通过 sysfs 的 summary 文件展示。
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/gpio/consumer.h>
#include <linux/slab.h>

struct dtdemo_kind {
	const char *name;
	const char *phandle_prop;	/* 要统计条目数的 phandle 列表属性 */
	const char *cells_name;		/* 对应提供者里的 #xxx-cells */
};

static const struct dtdemo_kind kind_gpio = { "gpio", NULL, NULL };
static const struct dtdemo_kind kind_pwm  = { "pwm", "pwms", "#pwm-cells" };
static const struct dtdemo_kind kind_fan  = { "fan", "pwms", "#pwm-cells" };

struct dtdemo {
	const struct dtdemo_kind *kind;
	int nprops;
	int nphandles;
	int led_gpios;
	int button_gpios;
	u32 debounce_ms;
};

static ssize_t summary_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dtdemo *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "kind=%s props=%d phandles=%d led=%d button=%d debounce_ms=%u\n",
			  d->kind->name, d->nprops, d->nphandles,
			  d->led_gpios, d->button_gpios, d->debounce_ms);
}
static DEVICE_ATTR_RO(summary);

static struct attribute *dtdemo_attrs[] = {
	&dev_attr_summary.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dtdemo);

static int dtdemo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct property *prop;
	struct dtdemo *d;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	/* of_match_table 里每一项的 .data，就是这里拿到的指针 */
	d->kind = device_get_match_data(dev);
	if (!d->kind)
		return -ENODEV;

	dev_info(dev, "probe: node %pOF, kind %s\n", np, d->kind->name);

	for_each_property_of_node(np, prop) {
		dev_info(dev, "  property %-14s %d bytes\n", prop->name, prop->length);
		d->nprops++;
	}

	if (d->kind->phandle_prop) {
		d->nphandles = of_count_phandle_with_args(np, d->kind->phandle_prop,
							  d->kind->cells_name);
		if (d->nphandles < 0)
			return dev_err_probe(dev, d->nphandles, "bad %s\n", d->kind->phandle_prop);
	}

	/* gpiod_count 找的是 "<名字>-gpios" 属性；只数数量，不申请 GPIO */
	d->led_gpios = max(gpiod_count(dev, "led"), 0);
	d->button_gpios = max(gpiod_count(dev, "button"), 0);

	/* 可选属性：不存在就用默认值，不算错误 */
	d->debounce_ms = 10;
	device_property_read_u32(dev, "debounce-ms", &d->debounce_ms);

	platform_set_drvdata(pdev, d);
	return 0;
}

static void dtdemo_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "remove\n");
	/* devm_ 申请的内存会在这之后自动释放 */
}

static const struct of_device_id dtdemo_of_match[] = {
	{ .compatible = "tutorial,gpio-demo", .data = &kind_gpio },
	{ .compatible = "tutorial,pwm-demo",  .data = &kind_pwm },
	{ .compatible = "tutorial,fan-demo",  .data = &kind_fan },
	{ }
};
MODULE_DEVICE_TABLE(of, dtdemo_of_match);

static struct platform_driver dtdemo_driver = {
	.probe = dtdemo_probe,
	.remove_new = dtdemo_remove,
	.driver = {
		.name = "tutorial-dtdemo",
		.of_match_table = dtdemo_of_match,
		.dev_groups = dtdemo_groups,
	},
};
module_platform_driver(dtdemo_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("jetson-drivers ch02: device tree + platform driver demo");
