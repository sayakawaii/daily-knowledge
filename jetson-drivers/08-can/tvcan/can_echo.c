// SPDX-License-Identifier: GPL-2.0
/*
 * can_echo：SocketCAN 用户态程序。在 tx 网卡上发一帧，在 rx 网卡上等这一帧。
 * 用法：./can_echo <tx网卡> <rx网卡> <ID十六进制> [只收这个ID的过滤器]
 *   ./can_echo tvcan0 tvcan1 123           tvcan0 发 ID 0x123，tvcan1 收
 *   ./can_echo can0 can0 123               mttcan 内部回环
 *   ./can_echo tvcan0 tvcan1 456 123       rx 只收 0x123，所以收不到 0x456（返回 1）
 * 返回 0 = 1 秒内收到了内容一致的帧，1 = 没收到，2 = 出错。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/can.h>
#include <linux/can/raw.h>

/* 打开一个绑定到 ifname 的 CAN_RAW socket，失败返回 -1 */
static int can_open(const char *ifname)
{
	struct sockaddr_can addr = { .can_family = AF_CAN };
	struct ifreq ifr;
	int s;

	/*
	 * TODO 1：创建 CAN 原始 socket，绑定到 ifname，返回 socket。每一步失败都 perror、close（如果已经创建）并返回 -1。
	 *   1. socket(协议族 PF_CAN, 类型 SOCK_RAW, 协议 CAN_RAW)
	 *   2. 把网卡名拷进 ifr.ifr_name，ioctl(s, SIOCGIFINDEX, &ifr) 得到网卡编号 ifr.ifr_ifindex
	 *   3. addr.can_ifindex = 网卡编号，bind(s, (struct sockaddr *)&addr, sizeof(addr))
	 * 这和 TCP/UDP socket 是同一套 API，只是地址结构换成了 sockaddr_can。
	 */
	(void)addr; (void)ifr; (void)s;
	return -1;
}

int main(int argc, char **argv)
{
	struct can_frame tx = { 0 }, rx;
	int ts, rs, i;

	if (argc < 4) {
		fprintf(stderr, "usage: %s <tx if> <rx if> <id hex> [filter id hex]\n", argv[0]);
		return 2;
	}
	rs = can_open(argv[2]);
	ts = can_open(argv[1]);
	if (rs < 0 || ts < 0)
		return 2;

	if (argc > 4) {
		/* 接收过滤：(收到的ID & mask) == (can_id & mask) 才收 */
		/*
		 * TODO 2：在 rs 上设置接收过滤器，只收 ID 等于 argv[4]（十六进制）的标准帧。
		 *   struct can_filter 有 can_id 和 can_mask 两个字段：(收到的ID & can_mask) == (can_id & can_mask) 才收。
		 *   标准帧 ID 是 11 位，掩码用 CAN_SFF_MASK。
		 *   setsockopt(rs, SOL_CAN_RAW, CAN_RAW_FILTER, &f, sizeof(f))，失败 perror 并返回 2。
		 */
	}

	tx.can_id = strtoul(argv[3], NULL, 16) & CAN_SFF_MASK;
	tx.len = 8;
	for (i = 0; i < 8; i++)
		tx.data[i] = 0x10 + i;
	if (write(ts, &tx, sizeof(tx)) != sizeof(tx)) {
		perror("write");
		return 2;
	}

	for (;;) {
		struct pollfd p = { .fd = rs, .events = POLLIN };

		if (poll(&p, 1, 1000) <= 0) {
			printf("no frame received\n");
			return 1;
		}
		if (read(rs, &rx, sizeof(rx)) != sizeof(rx))
			return 2;
		if (rx.can_id == tx.can_id && rx.len == tx.len && !memcmp(rx.data, tx.data, 8)) {
			printf("received %03X [%d] %02X %02X ... %02X\n", rx.can_id, rx.len,
			       rx.data[0], rx.data[1], rx.data[7]);
			return 0;
		}
	}
}
