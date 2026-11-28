// SPDX-License-Identifier: GPL-2.0
/*
 * tgpio：一个 LED 加一个按键。
 *   - 按键用中断检测，延时工作队列做软件去抖；
 *   - 每次按下计数加 1，默认同时翻转 LED；
 *   - 通过 sysfs 暴露 led / button / presses / edges / debounce_ms，presses 变化时 sysfs_notify。
 *
 * 两种绑定方式：
 *   1. 设备树：教程覆盖层里的 tutorial-gpio 节点（29 脚 LED、31 脚按键）；
 *   2. 板级文件：tgpio_simboard.ko 注册的 tutorial-gpio-sim 设备，GPIO 来自 gpio-sim 模拟器。
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/atomic.h>

static bool toggle_on_press = true;
module_param(toggle_on_press, bool, 0644);
MODULE_PARM_DESC(toggle_on_press, "每次按下翻转 LED（跳线测试时要关掉，否则 LED 和按键会互相触发）");

struct tgpio {
	struct device *dev;
	struct gpio_desc *led;
	struct gpio_desc *button;
	int irq;
	struct delayed_work debounce_work;
	struct mutex lock;		/* 保护下面三个字段 */
	int stable;			/* 去抖后的按键状态：1 = 按下（逻辑值，已经考虑了 ACTIVE_LOW） */
	int led_on;
	unsigned long presses;
	atomic_t edges;			/* 原始中断次数，能看出抖动有多严重 */
	u32 debounce_ms;
};

/*
 * 硬中断上下文：不能睡眠，要尽快返回。
 * 只记一次边沿，然后把去抖工作往后推 debounce_ms。抖动期间不停有边沿，工作就一直被往后推，
 * 直到电平稳定 debounce_ms 之后才真正执行一次。
 */
static irqreturn_t tgpio_irq(int irq, void *data)
{
	struct tgpio *t = data;

	atomic_inc(&t->edges);
	mod_delayed_work(system_wq, &t->debounce_work, msecs_to_jiffies(t->debounce_ms));
	return IRQ_HANDLED;
}

/* 进程上下文（工作队列线程）：可以睡眠，可以用 _cansleep 接口和 mutex */
static void tgpio_debounce_fn(struct work_struct *work)
{
	struct tgpio *t = container_of(to_delayed_work(work), struct tgpio, debounce_work);
	bool notify = false;
	int val;

	val = gpiod_get_value_cansleep(t->button);
	if (val < 0)
		return;

	mutex_lock(&t->lock);
	if (val != t->stable) {
		t->stable = val;
		if (val) {
			t->presses++;
			notify = true;
			if (toggle_on_press) {
				t->led_on = !t->led_on;
				gpiod_set_value_cansleep(t->led, t->led_on);
			}
		}
	}
	mutex_unlock(&t->lock);

	if (notify)
		sysfs_notify(&t->dev->kobj, NULL, "presses");
}

static ssize_t led_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tgpio *t = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", t->led_on);
}

static ssize_t led_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct tgpio *t = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	mutex_lock(&t->lock);
	t->led_on = on;
	gpiod_set_value_cansleep(t->led, on);
	mutex_unlock(&t->lock);
	return count;
}
static DEVICE_ATTR_RW(led);

static ssize_t button_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tgpio *t = dev_get_drvdata(dev);

	/* 现读引脚，不经过去抖 */
	return sysfs_emit(buf, "%d\n", gpiod_get_value_cansleep(t->button));
}
static DEVICE_ATTR_RO(button);

static ssize_t presses_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tgpio *t = dev_get_drvdata(dev);
	unsigned long n;

	mutex_lock(&t->lock);
	n = t->presses;
	mutex_unlock(&t->lock);
	return sysfs_emit(buf, "%lu\n", n);
}
static DEVICE_ATTR_RO(presses);

static ssize_t edges_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tgpio *t = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", atomic_read(&t->edges));
}
static DEVICE_ATTR_RO(edges);

static ssize_t debounce_ms_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tgpio *t = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(t->debounce_ms));
}

static ssize_t debounce_ms_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct tgpio *t = dev_get_drvdata(dev);
	u32 v;
	int ret;

	ret = kstrtou32(buf, 0, &v);
	if (ret)
		return ret;
	if (v < 1 || v > 1000)
		return -EINVAL;
	WRITE_ONCE(t->debounce_ms, v);
	return count;
}
static DEVICE_ATTR_RW(debounce_ms);

static struct attribute *tgpio_attrs[] = {
	&dev_attr_led.attr,
	&dev_attr_button.attr,
	&dev_attr_presses.attr,
	&dev_attr_edges.attr,
	&dev_attr_debounce_ms.attr,
	NULL,
};
ATTRIBUTE_GROUPS(tgpio);

static void tgpio_cancel_work(void *data)
{
	struct tgpio *t = data;

	cancel_delayed_work_sync(&t->debounce_work);
}

static int tgpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tgpio *t;
	int ret;

	t = devm_kzalloc(dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->dev = dev;
	mutex_init(&t->lock);
	INIT_DELAYED_WORK(&t->debounce_work, tgpio_debounce_fn);

	t->debounce_ms = 20;
	device_property_read_u32(dev, "debounce-ms", &t->debounce_ms);

	/* 名字 "led" 对应设备树里的 led-gpios，或板级文件 lookup 表里的 "led" */
	t->led = devm_gpiod_get(dev, "led", GPIOD_OUT_LOW);
	if (IS_ERR(t->led))
		return dev_err_probe(dev, PTR_ERR(t->led), "get led gpio\n");

	t->button = devm_gpiod_get(dev, "button", GPIOD_IN);
	if (IS_ERR(t->button))
		return dev_err_probe(dev, PTR_ERR(t->button), "get button gpio\n");

	t->irq = gpiod_to_irq(t->button);
	if (t->irq < 0)
		return dev_err_probe(dev, t->irq, "button has no irq\n");

	ret = gpiod_get_value_cansleep(t->button);
	t->stable = ret > 0;

	platform_set_drvdata(pdev, t);

	/*
	 * 释放顺序和注册顺序相反：先 free_irq（不会再有新的 mod_delayed_work），
	 * 再取消工作。所以取消工作的回调要比 devm_request_irq 先注册。
	 */
	ret = devm_add_action_or_reset(dev, tgpio_cancel_work, t);
	if (ret)
		return ret;

	ret = devm_request_irq(dev, t->irq, tgpio_irq,
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			       dev_name(dev), t);
	if (ret)
		return dev_err_probe(dev, ret, "request irq %d\n", t->irq);

	dev_info(dev, "led + button ready, irq %d, debounce %u ms\n", t->irq, t->debounce_ms);
	return 0;
}

static const struct of_device_id tgpio_of_match[] = {
	{ .compatible = "tutorial,gpio-demo" },
	{ }
};
MODULE_DEVICE_TABLE(of, tgpio_of_match);

/* 没有设备树节点的设备按名字匹配 */
static const struct platform_device_id tgpio_ids[] = {
	{ .name = "tutorial-gpio-sim" },
	{ }
};
MODULE_DEVICE_TABLE(platform, tgpio_ids);

static struct platform_driver tgpio_driver = {
	.probe = tgpio_probe,
	.id_table = tgpio_ids,
	.driver = {
		.name = "tutorial-gpio",
		.of_match_table = tgpio_of_match,
		.dev_groups = tgpio_groups,
	},
};
module_platform_driver(tgpio_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("jetson-drivers ch03: LED + debounced button");
