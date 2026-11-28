// SPDX-License-Identifier: GPL-2.0
/*
 * tserdev：serdev 驱动 —— 在内核里使用一个串口。
 * 绑定教程覆盖层里 ttyTHS2（uarte，serial@3140000）下的 tutorial-serdev 节点。
 * 绑定后 /dev/ttyTHS2 消失，串口归这个驱动独占。
 *
 * 用户接口：
 *   /dev/tutorial_serdev    write = 从串口发出去；read = 读串口收到的数据（阻塞，支持 O_NONBLOCK 和 poll）
 *   sysfs（/sys/bus/serial/devices/serial0-0/）：
 *     baudrate   读写，返回实际设置的波特率
 *     loopback   读写，1 = 打开 UART 内部回环（TX 在芯片内部直接接到 RX，不用接线）
 *     rx_bytes / tx_bytes / rx_dropped
 *
 * 实际产品里，serdev 用来写挂在串口上的芯片的驱动：蓝牙模块、GPS、串口屏、指纹模块……
 * 用户态不再直接操作 /dev/ttyXXX，而是用驱动提供的更高层接口。
 */
#include <linux/module.h>
#include <linux/serdev.h>
#include <linux/miscdevice.h>
#include <linux/kfifo.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/property.h>
#include <linux/termios.h>
#include <linux/of.h>
#include <linux/delay.h>

static bool loopback = true;
module_param(loopback, bool, 0444);
MODULE_PARM_DESC(loopback, "probe 时打开 UART 内部回环（默认打开，因为 ttyTHS2 没有引出来）");

#define TSERDEV_FIFO	4096
#define TSERDEV_WBUF	256

struct tserdev {
	struct serdev_device *serdev;
	struct miscdevice misc;
	DECLARE_KFIFO(rx, u8, TSERDEV_FIFO);
	wait_queue_head_t rx_wq;
	struct mutex read_lock;		/* 同一时间只有一个读者（kfifo 单读单写可以不加锁） */
	struct mutex write_lock;
	unsigned int baud;
	bool loop;
	atomic_t rx_bytes, tx_bytes, rx_dropped;
};

/*
 * 串口收到数据时由 tty 层调用（在工作队列里，进程上下文）。
 * 这里是唯一的写者，用户 read 是唯一的读者，kfifo 在单读单写时不需要锁。
 * 返回值是「我接收了多少字节」；返回少于 count 时 tty 层会稍后重发剩下的。
 * 这里选择全部接收、放不下的丢掉并计数，避免读者不读时把 tty 层堵住。
 */
static ssize_t tserdev_receive_buf(struct serdev_device *serdev, const u8 *data, size_t count)
{
	struct tserdev *t = serdev_device_get_drvdata(serdev);
	unsigned int n;

	n = kfifo_in(&t->rx, data, count);
	atomic_add(n, &t->rx_bytes);
	if (n < count)
		atomic_add(count - n, &t->rx_dropped);
	if (n)
		wake_up_interruptible(&t->rx_wq);
	return count;
}

static const struct serdev_device_ops tserdev_ops = {
	.receive_buf = tserdev_receive_buf,
	.write_wakeup = serdev_device_write_wakeup,	/* serdev_device_write() 等待发送完成要用 */
};

static struct tserdev *file_to_tserdev(struct file *f)
{
	/* misc 核心在 open 时把 file->private_data 设成 miscdevice 指针 */
	return container_of(f->private_data, struct tserdev, misc);
}

static ssize_t tserdev_read(struct file *f, char __user *buf, size_t len, loff_t *ppos)
{
	struct tserdev *t = file_to_tserdev(f);
	unsigned int copied;
	int ret;

	if (mutex_lock_interruptible(&t->read_lock))
		return -ERESTARTSYS;

	while (kfifo_is_empty(&t->rx)) {
		mutex_unlock(&t->read_lock);
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(t->rx_wq, !kfifo_is_empty(&t->rx)))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&t->read_lock))
			return -ERESTARTSYS;
	}
	ret = kfifo_to_user(&t->rx, buf, len, &copied);
	mutex_unlock(&t->read_lock);
	return ret ? ret : copied;
}

static ssize_t tserdev_write(struct file *f, const char __user *buf, size_t len, loff_t *ppos)
{
	struct tserdev *t = file_to_tserdev(f);
	u8 kbuf[TSERDEV_WBUF];
	ssize_t ret;

	len = min_t(size_t, len, sizeof(kbuf));
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;

	if (mutex_lock_interruptible(&t->write_lock))
		return -ERESTARTSYS;
	/* 阻塞直到全部交给 tty 层或超时；返回写出去的字节数 */
	ret = serdev_device_write(t->serdev, kbuf, len, msecs_to_jiffies(1000));
	if (ret > 0)
		atomic_add(ret, &t->tx_bytes);
	mutex_unlock(&t->write_lock);
	return ret;
}

static __poll_t tserdev_poll(struct file *f, poll_table *wait)
{
	struct tserdev *t = file_to_tserdev(f);

	poll_wait(f, &t->rx_wq, wait);
	return (kfifo_is_empty(&t->rx) ? 0 : EPOLLIN | EPOLLRDNORM) | EPOLLOUT | EPOLLWRNORM;
}

static const struct file_operations tserdev_fops = {
	.owner = THIS_MODULE,
	.read = tserdev_read,
	.write = tserdev_write,
	.poll = tserdev_poll,
	.llseek = noop_llseek,
};

static int tserdev_set_loop(struct tserdev *t, bool on)
{
	int ret = on ? serdev_device_set_tiocm(t->serdev, TIOCM_LOOP, 0)
		     : serdev_device_set_tiocm(t->serdev, 0, TIOCM_LOOP);

	if (!ret)
		t->loop = on;
	return ret;
}

static ssize_t baudrate_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tserdev *t = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", t->baud);
}

static ssize_t baudrate_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct tserdev *t = dev_get_drvdata(dev);
	unsigned int baud;
	int ret;

	ret = kstrtouint(buf, 0, &baud);
	if (ret)
		return ret;
	if (baud < 300 || baud > 4000000)
		return -EINVAL;
	mutex_lock(&t->write_lock);
	serdev_device_wait_until_sent(t->serdev, msecs_to_jiffies(500));
	/* 返回值是硬件实际能做到的波特率，不一定和要求的完全一样 */
	t->baud = serdev_device_set_baudrate(t->serdev, baud);
	mutex_unlock(&t->write_lock);
	return count;
}
static DEVICE_ATTR_RW(baudrate);

static ssize_t loopback_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tserdev *t = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", t->loop);
}

static ssize_t loopback_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct tserdev *t = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;
	ret = tserdev_set_loop(t, on);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(loopback);

#define TSERDEV_COUNTER(name)							\
static ssize_t name##_show(struct device *dev, struct device_attribute *attr,	\
			   char *buf)						\
{										\
	struct tserdev *t = dev_get_drvdata(dev);				\
	return sysfs_emit(buf, "%d\n", atomic_read(&t->name));			\
}										\
static DEVICE_ATTR_RO(name)

TSERDEV_COUNTER(rx_bytes);
TSERDEV_COUNTER(tx_bytes);
TSERDEV_COUNTER(rx_dropped);

static struct attribute *tserdev_attrs[] = {
	&dev_attr_baudrate.attr,
	&dev_attr_loopback.attr,
	&dev_attr_rx_bytes.attr,
	&dev_attr_tx_bytes.attr,
	&dev_attr_rx_dropped.attr,
	NULL,
};
ATTRIBUTE_GROUPS(tserdev);

static void tserdev_misc_deregister(void *data)
{
	misc_deregister(data);
}

static int tserdev_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct tserdev *t;
	u32 speed = 115200;
	int ret;

	t = devm_kzalloc(dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->serdev = serdev;
	INIT_KFIFO(t->rx);
	init_waitqueue_head(&t->rx_wq);
	mutex_init(&t->read_lock);
	mutex_init(&t->write_lock);
	serdev_device_set_drvdata(serdev, t);
	serdev_device_set_client_ops(serdev, &tserdev_ops);

	/* 打开串口（相当于用户态 open("/dev/ttyTHS2")），devm 版本在 remove 时自动关闭 */
	ret = devm_serdev_device_open(dev, serdev);
	if (ret)
		return dev_err_probe(dev, ret, "open uart\n");

	device_property_read_u32(dev, "current-speed", &speed);
	t->baud = serdev_device_set_baudrate(serdev, speed);
	serdev_device_set_flow_control(serdev, false);
	ret = serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);
	if (ret)
		return ret;

	if (loopback) {
		ret = tserdev_set_loop(t, true);
		if (ret)
			return dev_err_probe(dev, ret, "enable loopback\n");
	}

	/*
	 * 从打开串口到切到内部回环之间，悬空的 RX 引脚可能收到噪声字节。等一下再清掉。
	 * kfifo_reset_out 只动读指针，是读者一侧的操作，和 receive_buf 并发也安全。
	 */
	msleep(20);
	kfifo_reset_out(&t->rx);
	atomic_set(&t->rx_bytes, 0);
	atomic_set(&t->rx_dropped, 0);

	t->misc.minor = MISC_DYNAMIC_MINOR;
	t->misc.name = "tutorial_serdev";
	t->misc.fops = &tserdev_fops;
	t->misc.mode = 0666;
	t->misc.parent = dev;
	ret = misc_register(&t->misc);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, tserdev_misc_deregister, &t->misc);
	if (ret)
		return ret;

	dev_info(dev, "uart ready: %u baud, loopback %s, /dev/%s\n",
		 t->baud, t->loop ? "on" : "off", t->misc.name);
	return 0;
}

static const struct of_device_id tserdev_of_match[] = {
	{ .compatible = "tutorial,serdev-demo" },
	{ }
};
MODULE_DEVICE_TABLE(of, tserdev_of_match);

static struct serdev_device_driver tserdev_driver = {
	.probe = tserdev_probe,
	.driver = {
		.name = "tutorial-serdev",
		.of_match_table = tserdev_of_match,
		.dev_groups = tserdev_groups,
	},
};
module_serdev_device_driver(tserdev_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("jetson-drivers ch07: serdev driver with char device interface");
