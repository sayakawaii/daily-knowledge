// SPDX-License-Identifier: GPL-2.0
/*
 * tgpio_simboard：没有设备树时的「板级文件」写法。
 * 设备树出现之前，ARM 板子都是这样在 C 代码里描述硬件的（arch/arm/mach-xxx/board-xxx.c）。
 *
 * 做两件事：
 *   1. 登记一张 GPIO 查找表：设备 tutorial-gpio-sim 的 "led" 是 tutorial-sim 芯片第 0 根线，
 *      "button" 是第 1 根线（低电平有效）；
 *   2. 注册一个名叫 tutorial-gpio-sim 的平台设备，tgpio 驱动会按名字匹配上它。
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/machine.h>

static struct gpiod_lookup_table simboard_gpios = {
	.dev_id = "tutorial-gpio-sim",		/* 设备名 */
	.table = {
		/* 芯片标签、芯片内的线号、名字、标志 */
		GPIO_LOOKUP("tutorial-sim", 0, "led", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("tutorial-sim", 1, "button", GPIO_ACTIVE_LOW),
		{ }
	},
};

static struct platform_device *simboard_pdev;

static int __init simboard_init(void)
{
	/* 查找表要在设备 probe 之前登记 */
	gpiod_add_lookup_table(&simboard_gpios);

	simboard_pdev = platform_device_register_simple("tutorial-gpio-sim",
							PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(simboard_pdev)) {
		gpiod_remove_lookup_table(&simboard_gpios);
		return PTR_ERR(simboard_pdev);
	}
	return 0;
}

static void __exit simboard_exit(void)
{
	platform_device_unregister(simboard_pdev);
	gpiod_remove_lookup_table(&simboard_gpios);
}

module_init(simboard_init);
module_exit(simboard_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("jetson-drivers ch03: board file for gpio-sim");
