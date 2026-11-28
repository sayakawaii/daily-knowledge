// usb_loop：通过 /dev/tusb0 做回环测试，再测吞吐。
//   ./usb_loop [/dev/tusb0] [吞吐测试的轮数，默认 2000]
// 输出每种长度 OK / BAD，最后一行是吞吐（MB/s）。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#define MAXLEN 4096

// 写 len 字节，再读回来，读到的放进 in，返回读到的字节数；出错返回 -1
static int loop_once(int fd, const unsigned char *out, unsigned char *in, int len)
{
	// TODO：先 write(fd, out, len)，写的字节数不等于 len 就返回 -1；
	//   再 read(fd, in, MAXLEN)，返回读到的字节数（出错时 read 自己返回 -1）。
	//   为什么读的时候给 MAXLEN 而不是 len？看驱动 tusb_read 的注释。
	(void)fd; (void)out; (void)in; (void)len;
	return -1;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/tusb0";
	int rounds = argc > 2 ? atoi(argv[2]) : 2000;
	// 1 字节、不满一个包、正好一个包（512）、一个包多一点、一整页
	int sizes[] = { 1, 100, 511, 512, 513, 2048, 4096 };
	unsigned char out[MAXLEN], in[MAXLEN];
	struct timespec a, b;
	int fd, bad = 0;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror(dev);
		return 1;
	}
	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		int len = sizes[i], n;

		for (int k = 0; k < len; k++)
			out[k] = (unsigned char)(k * 7 + len);
		memset(in, 0, sizeof(in));
		n = loop_once(fd, out, in, len);
		if (n == len && !memcmp(out, in, len)) {
			printf("OK %d\n", len);
		} else {
			printf("BAD %d (got %d)\n", len, n);
			bad++;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &a);
	for (int r = 0; r < rounds && !bad; r++)
		if (loop_once(fd, out, in, MAXLEN) != MAXLEN)
			bad++;
	clock_gettime(CLOCK_MONOTONIC, &b);
	double sec = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
	if (!bad)
		printf("THROUGHPUT %.1f MB/s\n", 2.0 * rounds * MAXLEN / sec / 1e6);
	close(fd);
	return bad ? 1 : 0;
}
