// SPDX-License-Identifier: GPL-2.0
/*
 * 第 0 章：第一个内核模块
 *
 * 演示：模块的加载和卸载、模块参数、printk 日志级别、
 * 内核定时器（运行在软中断上下文，不能睡眠）。
 */
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/timer.h>

static char *name = "Jetson";
module_param(name, charp, 0444);
MODULE_PARM_DESC(name, "打招呼的对象");

static int period_ms = 1000;
module_param(period_ms, int, 0644);
MODULE_PARM_DESC(period_ms, "定时器周期（毫秒），运行中可以通过 sysfs 修改");

static unsigned int ticks;
module_param(ticks, uint, 0444);
MODULE_PARM_DESC(ticks, "定时器已经触发的次数（只读）");

static struct timer_list hello_timer;

static void hello_timer_fn(struct timer_list *t)
{
	ticks++;
	pr_info("hello: tick %u, in_interrupt=%d, jiffies=%lu\n",
		ticks, in_interrupt() ? 1 : 0, jiffies);
	mod_timer(&hello_timer, jiffies + msecs_to_jiffies(period_ms));
}

static int __init hello_init(void)
{
	if (period_ms < 10) {
		pr_err("hello: period_ms=%d 太小，至少 10\n", period_ms);
		return -EINVAL;
	}

	pr_info("hello: Hello, %s! HZ=%d\n", name, HZ);
	timer_setup(&hello_timer, hello_timer_fn, 0);
	mod_timer(&hello_timer, jiffies + msecs_to_jiffies(period_ms));
	return 0;
}

static void __exit hello_exit(void)
{
	timer_delete_sync(&hello_timer);
	pr_info("hello: Goodbye, %s! ticks=%u\n", name, ticks);
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("jetson-drivers tutorial");
MODULE_DESCRIPTION("第 0 章：第一个内核模块");
