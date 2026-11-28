// SPDX-License-Identifier: GPL-2.0
/*
 * uart_loop：用户态串口编程（termios），用 UART 内部回环自测，不需要接线。
 * 用法：sudo ./uart_loop /dev/ttyTHS1 115200 [ext]
 *   默认打开 UART 内部回环（TIOCM_LOOP）；加 ext 参数则不开回环，需要用杜邦线把 TX（8 脚）接到 RX（10 脚）。
 * 发 64 字节，收回来比较。返回 0 = 一致，1 = 不一致或超时，2 = 出错。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>

/* glibc 的头文件没有定义它（内核 include/uapi/asm-generic/termios.h 里有），和 <termios.h> 一起包含会冲突，自己定义 */
#ifndef TIOCM_LOOP
#define TIOCM_LOOP 0x8000
#endif

static speed_t to_speed(long baud)
{
	switch (baud) {
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 921600: return B921600;
	case 1000000: return B1000000;
	case 2000000: return B2000000;
	case 3000000: return B3000000;
	default: return 0;
	}
}

int main(int argc, char **argv)
{
	unsigned char tx[64], rx[64];
	int ext = argc > 3 && !strcmp(argv[3], "ext");
	struct termios tio;
	size_t got = 0;
	speed_t spd;
	int fd, i, bits = TIOCM_LOOP, bad = 0;

	if (argc < 3 || !(spd = to_speed(atol(argv[2])))) {
		fprintf(stderr, "usage: %s /dev/ttyXXX <baud> [ext]\n", argv[0]);
		return 2;
	}
	/* O_NOCTTY：不要让这个串口变成本进程的控制终端 */
	fd = open(argv[1], O_RDWR | O_NOCTTY);
	if (fd < 0) {
		perror("open");
		return 2;
	}

	/*
	 * TODO 1：把串口设成原始模式、指定波特率、8N1、无流控。
	 *   1. tcgetattr 读出当前设置到 tio（失败 perror 并返回 2）
	 *   2. cfmakeraw：关掉行缓冲、回显、换行转换、特殊字符处理
	 *   3. cfsetispeed / cfsetospeed 设成 spd
	 *   4. c_cflag 加上 CLOCAL | CREAD，去掉 CRTSCTS（硬件流控）、CSTOPB（2 停止位）、PARENB（校验）
	 *   5. c_cc[VMIN] = 0、c_cc[VTIME] = 0：read 不等待，后面用 poll 等数据
	 *   6. tcsetattr(fd, TCSANOW, &tio) 生效（失败 perror 并返回 2）
	 *   7. tcflush(fd, TCIOFLUSH) 清掉残留数据
	 */

	/*
	 * TODO 2：不是 ext 模式时，打开 UART 内部回环：ioctl(fd, TIOCMBIS, &bits)，bits 已经是 TIOCM_LOOP。
	 *   TIOCMBIS = 置位调制解调器控制位，TIOCMBIC = 清除。失败 perror 并返回 2。
	 */

	for (i = 0; i < (int)sizeof(tx); i++)
		tx[i] = 'A' + i % 26;
	if (write(fd, tx, sizeof(tx)) != sizeof(tx)) {
		perror("write");
		return 2;
	}

	while (got < sizeof(rx)) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		ssize_t n;

		if (poll(&p, 1, 500) <= 0)
			break;
		n = read(fd, rx + got, sizeof(rx) - got);
		if (n <= 0)
			break;
		got += n;
	}

	if (!ext)
		ioctl(fd, TIOCMBIC, &bits);
	close(fd);

	for (i = 0; i < (int)got; i++)
		if (rx[i] != tx[i])
			bad++;
	printf("%s %s baud: sent %zu, received %zu, %d differ\n",
	       argv[1], argv[2], sizeof(tx), got, bad);
	return (got == sizeof(tx) && !bad) ? 0 : 1;
}
