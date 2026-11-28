// SPDX-License-Identifier: GPL-2.0
/*
 * tusb：USB 主机侧驱动，驱动一个「Loopback」设备（VID:PID 1209:0001，厂商自定义接口，两个 bulk 端点）。
 *
 *   /dev/tusb0   write() 把数据发到 bulk OUT 端点（异步 URB 写法）
 *                read()  从 bulk IN 端点收数据（同步 usb_bulk_msg 写法）
 *   sysfs（在接口目录下）：ep_in、ep_out、max_packet、speed、tx_bytes、rx_bytes、rx_dropped
 *
 * 设备随时可能被拔掉，而用户程序可能还开着 /dev/tusb0。所以：
 *   - 驱动数据用 kref 计数，probe 持有一份，每次 open 再加一份，最后一个放手的人负责释放；
 *   - disconnect 时把 disconnected 置位，之后的 read/write 都返回 -ENODEV。
 */
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/uaccess.h>

#define TUSB_VID		0x1209
#define TUSB_PID		0x0001
/* 内核没开 CONFIG_USB_DYNAMIC_MINORS，USB 字符设备（主设备号 180）的次设备号要自己选一段，和 usb-skeleton 一样用 192 */
#define TUSB_MINOR_BASE		192
#define TUSB_BUF		4096
#define TUSB_TIMEOUT_MS		1000

struct tusb {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct kref kref;
	struct mutex io_mutex;		/* 串行化 I/O，并和 disconnect 互斥 */
	bool disconnected;

	u8 ep_in, ep_out;
	u16 mps, mps_out;		/* IN、OUT 端点的最大包长 */
	u8 *obuf, *ibuf;		/* kmalloc 的缓冲区才能做 DMA；栈上的数组不行 */
	struct urb *out_urb;
	struct completion out_done;
	int out_status;
	u64 tx_bytes, rx_bytes, rx_dropped;
};

static struct usb_driver tusb_driver;

static void tusb_delete(struct kref *kref)
{
	struct tusb *t = container_of(kref, struct tusb, kref);

	usb_free_urb(t->out_urb);
	kfree(t->obuf);
	kfree(t->ibuf);
	usb_put_intf(t->intf);
	usb_put_dev(t->udev);
	kfree(t);
}

static int tusb_open(struct inode *inode, struct file *file)
{
	struct usb_interface *intf = usb_find_interface(&tusb_driver, iminor(inode));
	struct tusb *t;

	if (!intf)
		return -ENODEV;
	t = usb_get_intfdata(intf);
	if (!t)
		return -ENODEV;
	kref_get(&t->kref);
	file->private_data = t;
	return 0;
}

static int tusb_release(struct inode *inode, struct file *file)
{
	struct tusb *t = file->private_data;

	kref_put(&t->kref, tusb_delete);
	return 0;
}

/* URB 完成回调：在中断上下文里执行，不能睡眠，只记下结果、唤醒等待者 */
static void tusb_out_complete(struct urb *urb)
{
	struct tusb *t = urb->context;

	t->out_status = urb->status;
	complete(&t->out_done);
}

static ssize_t tusb_write(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos)
{
	struct tusb *t = file->private_data;
	size_t len = min_t(size_t, count, TUSB_BUF);
	long left;
	int ret;

	if (!len)
		return 0;
	if (mutex_lock_interruptible(&t->io_mutex))
		return -ERESTARTSYS;
	if (t->disconnected) {
		ret = -ENODEV;
		goto out;
	}
	if (copy_from_user(t->obuf, ubuf, len)) {
		ret = -EFAULT;
		goto out;
	}

	/* 异步写法：填 URB → 提交 → 等完成回调。真实驱动里通常提交后就返回，同时挂着多个 URB 提高吞吐 */
	usb_fill_bulk_urb(t->out_urb, t->udev, usb_sndbulkpipe(t->udev, t->ep_out),
			  t->obuf, len, tusb_out_complete, t);
	/*
	 * bulk 传输靠「短包」（比最大包长短的包）表示结束。这个设备一次最多收 TUSB_BUF 字节：
	 * 长度正好是包长的整数倍、又没到 TUSB_BUF 时，没有短包，设备会一直等下去，
	 * 所以要在后面补一个零长度包（ZLP）。
	 */
	if (len % t->mps_out == 0 && len < TUSB_BUF)
		t->out_urb->transfer_flags |= URB_ZERO_PACKET;
	else
		t->out_urb->transfer_flags &= ~URB_ZERO_PACKET;
	reinit_completion(&t->out_done);
	ret = usb_submit_urb(t->out_urb, GFP_KERNEL);
	if (ret)
		goto out;
	left = wait_for_completion_interruptible_timeout(&t->out_done,
							 msecs_to_jiffies(TUSB_TIMEOUT_MS));
	if (left <= 0) {
		usb_kill_urb(t->out_urb);	/* 返回时回调一定已经执行完 */
		ret = left ? left : -ETIMEDOUT;
		goto out;
	}
	ret = t->out_status;
	if (!ret) {
		ret = t->out_urb->actual_length;
		t->tx_bytes += ret;
	}
out:
	mutex_unlock(&t->io_mutex);
	return ret;
}

static ssize_t tusb_read(struct file *file, char __user *ubuf, size_t count, loff_t *ppos)
{
	struct tusb *t = file->private_data;
	int actual = 0, ret;
	size_t n;

	if (!count)
		return 0;
	if (mutex_lock_interruptible(&t->io_mutex))
		return -ERESTARTSYS;
	if (t->disconnected) {
		ret = -ENODEV;
		goto out;
	}
	/*
	 * 同步写法：usb_bulk_msg 内部就是「填 URB、提交、等完成」。
	 * 总是按整个缓冲区去收：主机给的长度比设备发来的包小，会得到 -EOVERFLOW。
	 */
	ret = usb_bulk_msg(t->udev, usb_rcvbulkpipe(t->udev, t->ep_in), t->ibuf, TUSB_BUF,
			   &actual, TUSB_TIMEOUT_MS);
	if (ret)
		goto out;
	n = min_t(size_t, actual, count);
	if (copy_to_user(ubuf, t->ibuf, n)) {
		ret = -EFAULT;
		goto out;
	}
	t->rx_bytes += actual;
	t->rx_dropped += actual - n;
	ret = n;
out:
	mutex_unlock(&t->io_mutex);
	return ret;
}

static const struct file_operations tusb_fops = {
	.owner = THIS_MODULE,
	.open = tusb_open,
	.release = tusb_release,
	.read = tusb_read,
	.write = tusb_write,
	.llseek = noop_llseek,
};

static char *tusb_devnode(const struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0666;	/* 教程方便起见，普通用户也能读写 */
	return NULL;
}

static struct usb_class_driver tusb_class = {
	.name = "tusb%d",
	.fops = &tusb_fops,
	.devnode = tusb_devnode,
	.minor_base = TUSB_MINOR_BASE,
};

#define TUSB_ATTR_RO(_name, _fmt, _expr)						\
static ssize_t _name##_show(struct device *dev, struct device_attribute *attr, char *buf)	\
{											\
	struct tusb *t = usb_get_intfdata(to_usb_interface(dev));			\
	return sysfs_emit(buf, _fmt "\n", _expr);					\
}											\
static DEVICE_ATTR_RO(_name)

TUSB_ATTR_RO(ep_in, "0x%02x", t->ep_in);
TUSB_ATTR_RO(ep_out, "0x%02x", t->ep_out);
TUSB_ATTR_RO(max_packet, "%u", t->mps);
TUSB_ATTR_RO(speed, "%s", usb_speed_string(t->udev->speed));
TUSB_ATTR_RO(tx_bytes, "%llu", t->tx_bytes);
TUSB_ATTR_RO(rx_bytes, "%llu", t->rx_bytes);
TUSB_ATTR_RO(rx_dropped, "%llu", t->rx_dropped);

static struct attribute *tusb_attrs[] = {
	&dev_attr_ep_in.attr, &dev_attr_ep_out.attr, &dev_attr_max_packet.attr,
	&dev_attr_speed.attr, &dev_attr_tx_bytes.attr, &dev_attr_rx_bytes.attr,
	&dev_attr_rx_dropped.attr, NULL
};
ATTRIBUTE_GROUPS(tusb);

static int tusb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_endpoint_descriptor *in, *out;
	struct tusb *t;
	int ret;

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	kref_init(&t->kref);
	mutex_init(&t->io_mutex);
	init_completion(&t->out_done);
	t->udev = usb_get_dev(interface_to_usbdev(intf));
	t->intf = usb_get_intf(intf);

	/* 在当前备用设置（altsetting）的端点里，找第一个 bulk IN 和第一个 bulk OUT */
	ret = usb_find_common_endpoints(intf->cur_altsetting, &in, &out, NULL, NULL);
	if (ret) {
		dev_err(&intf->dev, "no bulk in/out endpoints\n");
		goto err;
	}
	t->ep_in = in->bEndpointAddress;
	t->ep_out = out->bEndpointAddress;
	t->mps = usb_endpoint_maxp(in);
	t->mps_out = usb_endpoint_maxp(out);

	ret = -ENOMEM;
	t->obuf = kmalloc(TUSB_BUF, GFP_KERNEL);
	t->ibuf = kmalloc(TUSB_BUF, GFP_KERNEL);
	t->out_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!t->obuf || !t->ibuf || !t->out_urb)
		goto err;

	usb_set_intfdata(intf, t);
	ret = usb_register_dev(intf, &tusb_class);
	if (ret) {
		dev_err(&intf->dev, "no minor available\n");
		usb_set_intfdata(intf, NULL);
		goto err;
	}
	dev_info(&intf->dev, "tusb%d: bulk in 0x%02x out 0x%02x, max packet %u, %s\n",
		 intf->minor - TUSB_MINOR_BASE, t->ep_in, t->ep_out, t->mps,
		 usb_speed_string(t->udev->speed));
	return 0;
err:
	kref_put(&t->kref, tusb_delete);
	return ret;
}

static void tusb_disconnect(struct usb_interface *intf)
{
	struct tusb *t = usb_get_intfdata(intf);
	int minor = intf->minor;

	/* 先删 /dev 节点，不再有新的 open；USB 核心在调用这里之前已经终止了这个接口上所有的 URB */
	usb_deregister_dev(intf, &tusb_class);
	mutex_lock(&t->io_mutex);
	t->disconnected = true;
	mutex_unlock(&t->io_mutex);
	usb_set_intfdata(intf, NULL);
	kref_put(&t->kref, tusb_delete);
	dev_info(&intf->dev, "tusb%d disconnected\n", minor - TUSB_MINOR_BASE);
}

static const struct usb_device_id tusb_ids[] = {
	{ USB_DEVICE_INTERFACE_CLASS(TUSB_VID, TUSB_PID, USB_CLASS_VENDOR_SPEC) },
	{ }
};
MODULE_DEVICE_TABLE(usb, tusb_ids);

static struct usb_driver tusb_driver = {
	.name = "tutorial-usb",
	.probe = tusb_probe,
	.disconnect = tusb_disconnect,
	.id_table = tusb_ids,
	.dev_groups = tusb_groups,
};
module_usb_driver(tusb_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("jetson-drivers tutorial: USB bulk loopback host driver");
