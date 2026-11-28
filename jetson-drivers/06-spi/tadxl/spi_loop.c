// SPDX-License-Identifier: GPL-2.0
/*
 * spi_loop：用 spidev 在用户态做 SPI 回环测试。
 * 用法：./spi_loop /dev/spidev0.1 [速度Hz]
 * 用杜邦线把 MOSI（排针 19 脚）和 MISO（21 脚）短接，发出去的字节应该原样收回来。
 * 没接线时 MISO 悬空，收到的是 0x00 或 0xFF。
 * 返回 0 = 收发一致，1 = 不一致，2 = 出错。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

int main(int argc, char **argv)
{
	uint8_t tx[32], rx[32];
	uint32_t speed = argc > 2 ? strtoul(argv[2], NULL, 0) : 1000000;
	uint8_t mode = SPI_MODE_0, bits = 8;
	struct spi_ioc_transfer tr;
	int fd, i, bad = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: %s /dev/spidevB.C [speed_hz]\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror("open");
		return 2;
	}
	if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
	    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
	    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
		perror("ioctl setup");
		return 2;
	}

	for (i = 0; i < (int)sizeof(tx); i++)
		tx[i] = 0xa5 ^ (i * 7);
	memset(rx, 0, sizeof(rx));

	memset(&tr, 0, sizeof(tr));
	/*
	 * TODO：填好 tr，做一次全双工传输：发 tx 的同时收 rx。
	 *   struct spi_ioc_transfer 的定义在 /usr/include/linux/spi/spidev.h，
	 *   缓冲区地址字段是 __u64，要把指针强转成 unsigned long 再赋值。
	 *   要填：tx_buf、rx_buf、len、speed_hz、bits_per_word。
	 *   然后 ioctl(fd, SPI_IOC_MESSAGE(1), &tr)，返回值 < 0 时 perror 并返回 2。
	 */

	for (i = 0; i < (int)sizeof(tx); i++)
		if (rx[i] != tx[i])
			bad++;
	printf("sent %02x %02x %02x %02x ..., got %02x %02x %02x %02x ..., %d/%zu bytes differ\n",
	       tx[0], tx[1], tx[2], tx[3], rx[0], rx[1], rx[2], rx[3], bad, sizeof(tx));
	close(fd);
	return bad ? 1 : 0;
}
