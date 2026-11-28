// SPDX-License-Identifier: GPL-2.0
/*
 * 第 1 章：字符设备 /dev/tutorial_fifo
 *
 * 一个 4 KB 的先进先出缓冲区：写进去的数据按顺序读出来。
 * 演示：misc 设备、read/write、阻塞与非阻塞、poll、ioctl、互斥锁、等待队列。
 */
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "fifo_ioctl.h"

#define FIFO_SIZE 4096

static DEFINE_KFIFO(fifo, char, FIFO_SIZE);
static DEFINE_MUTEX(fifo_lock);
static DECLARE_WAIT_QUEUE_HEAD(read_wq);
static DECLARE_WAIT_QUEUE_HEAD(write_wq);
static u64 bytes_written, bytes_read;

static ssize_t fifo_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	unsigned int copied;
	int ret;

	if (mutex_lock_interruptible(&fifo_lock))
		return -ERESTARTSYS;

	while (kfifo_is_empty(&fifo)) {
		mutex_unlock(&fifo_lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		/* 睡眠，直到有数据或者被信号（比如 Ctrl+C）打断 */
		if (wait_event_interruptible(read_wq, !kfifo_is_empty(&fifo)))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&fifo_lock))
			return -ERESTARTSYS;
	}

	ret = kfifo_to_user(&fifo, buf, count, &copied);
	if (!ret)
		bytes_read += copied;
	mutex_unlock(&fifo_lock);

	if (ret)
		return ret;
	wake_up_interruptible(&write_wq);
	return copied;
}

static ssize_t fifo_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	unsigned int copied;
	int ret;

	if (mutex_lock_interruptible(&fifo_lock))
		return -ERESTARTSYS;

	while (kfifo_is_full(&fifo)) {
		mutex_unlock(&fifo_lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(write_wq, !kfifo_is_full(&fifo)))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&fifo_lock))
			return -ERESTARTSYS;
	}

	ret = kfifo_from_user(&fifo, buf, count, &copied);
	if (!ret)
		bytes_written += copied;
	mutex_unlock(&fifo_lock);

	if (ret)
		return ret;
	wake_up_interruptible(&read_wq);
	return copied;
}

static __poll_t fifo_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = 0;

	poll_wait(file, &read_wq, wait);
	poll_wait(file, &write_wq, wait);

	mutex_lock(&fifo_lock);
	if (!kfifo_is_empty(&fifo))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (!kfifo_is_full(&fifo))
		mask |= EPOLLOUT | EPOLLWRNORM;
	mutex_unlock(&fifo_lock);
	return mask;
}

static long fifo_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	struct fifo_stats st;
	u32 len;

	switch (cmd) {
	case FIFO_IOC_CLEAR:
		mutex_lock(&fifo_lock);
		kfifo_reset(&fifo);
		mutex_unlock(&fifo_lock);
		wake_up_interruptible(&write_wq);
		return 0;

	case FIFO_IOC_GET_LEN:
		mutex_lock(&fifo_lock);
		len = kfifo_len(&fifo);
		mutex_unlock(&fifo_lock);
		return put_user(len, (u32 __user *)uarg);

	case FIFO_IOC_GET_STATS:
		mutex_lock(&fifo_lock);
		st.bytes_written = bytes_written;
		st.bytes_read = bytes_read;
		st.len = kfifo_len(&fifo);
		st.capacity = kfifo_size(&fifo);
		mutex_unlock(&fifo_lock);
		return copy_to_user(uarg, &st, sizeof(st)) ? -EFAULT : 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations fifo_fops = {
	.owner          = THIS_MODULE,
	.read           = fifo_read,
	.write          = fifo_write,
	.poll           = fifo_poll,
	.unlocked_ioctl = fifo_ioctl,
	.llseek         = noop_llseek,
};

static struct miscdevice fifo_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "tutorial_fifo",
	.fops  = &fifo_fops,
	.mode  = 0666,
};

static int __init fifo_init(void)
{
	int ret = misc_register(&fifo_misc);

	if (ret)
		return ret;
	pr_info("tutorial_fifo: /dev/%s ready, capacity %u bytes\n",
		fifo_misc.name, kfifo_size(&fifo));
	return 0;
}

static void __exit fifo_exit(void)
{
	misc_deregister(&fifo_misc);
}

module_init(fifo_init);
module_exit(fifo_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("第 1 章：字符设备 FIFO");
