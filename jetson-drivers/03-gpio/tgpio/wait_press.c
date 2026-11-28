// SPDX-License-Identifier: GPL-2.0
/*
 * wait_press：不轮询，等 presses 文件变化。
 * 用法：./wait_press <presses 文件> <要等几次> <超时毫秒>
 * 每次变化打印一行 "press N"；等够了返回 0，超时返回 1。
 *
 * sysfs 文件的 poll 规则（和普通文件、设备文件不一样）：
 *   1. 先完整读一次，内核才会开始「记录」这个文件有没有变化；
 *   2. poll 要等 POLLPRI | POLLERR，不是 POLLIN（sysfs 文件永远可读，等 POLLIN 会立刻返回）；
 *   3. 醒来后必须 lseek 回开头再 read，否则读到的是 EOF，下一次 poll 也会立刻返回。
 * 驱动那边调用 sysfs_notify() 时，等在这个文件上的 poll 被唤醒。
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

static long read_value(int fd)
{
	char buf[32];
	ssize_t n;

	/* TODO 1：回到文件开头（规则 3）。失败返回 -1。 */

	n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	return strtol(buf, NULL, 10);
}

int main(int argc, char **argv)
{
	int want, timeout_ms, got = 0;
	long last, now;
	int fd;

	if (argc != 4) {
		fprintf(stderr, "usage: %s <presses file> <count> <timeout ms>\n", argv[0]);
		return 2;
	}
	want = atoi(argv[2]);
	timeout_ms = atoi(argv[3]);

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 2;
	}
	last = read_value(fd);

	while (got < want) {
		/*
		 * TODO 2：填好 pfd，调用 poll 等 timeout_ms 毫秒，返回值存到 r。
		 *   要等的事件见文件开头的规则 2。
		 */
		int r = 0;

		if (r < 0) {
			perror("poll");
			return 2;
		}
		if (r == 0) {
			fprintf(stderr, "timeout after %d presses\n", got);
			return 1;
		}
		now = read_value(fd);
		if (now != last) {
			printf("press %ld\n", now);
			fflush(stdout);
			got += now - last;
			last = now;
		}
	}
	close(fd);
	return 0;
}
