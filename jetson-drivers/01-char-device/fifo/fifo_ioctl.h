/* SPDX-License-Identifier: GPL-2.0 */
/* 驱动和用户程序共用的 ioctl 定义 */
#ifndef FIFO_IOCTL_H
#define FIFO_IOCTL_H

/* <linux/types.h> 和 <linux/ioctl.h> 内核和用户程序都能包含，__u64 这类类型两边一致 */
#include <linux/ioctl.h>
#include <linux/types.h>

struct fifo_stats {
	__u64 bytes_written;
	__u64 bytes_read;
	__u32 len;       /* 当前缓冲区里有多少字节 */
	__u32 capacity;  /* 缓冲区总容量 */
};

#define FIFO_IOC_MAGIC 'F'
#define FIFO_IOC_CLEAR     _IO(FIFO_IOC_MAGIC, 0)
#define FIFO_IOC_GET_LEN   _IOR(FIFO_IOC_MAGIC, 1, __u32)
#define FIFO_IOC_GET_STATS _IOR(FIFO_IOC_MAGIC, 2, struct fifo_stats)

#endif
