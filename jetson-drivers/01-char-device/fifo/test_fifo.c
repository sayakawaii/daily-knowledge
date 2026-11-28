/*
 * 第 1 章测试：用系统调用操作 /dev/tutorial_fifo
 * 编译：make    运行：sudo insmod fifo.ko && ./test_fifo
 * 把每个 TODO 补上，9 项全部 PASS 就算完成。
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fifo_ioctl.h"

#define DEV "/dev/tutorial_fifo"

static int pass, fail;

static void check(const char *name, int ok)
{
	printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
	ok ? pass++ : fail++;
}

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(void)
{
	char buf[8192];
	unsigned int len = 12345;
	struct fifo_stats st = {0};
	ssize_t n = -1;
	int fd, nfd = -1;

	fd = open(DEV, O_RDWR);
	if (fd < 0) {
		perror("open " DEV "（模块加载了吗？）");
		return 1;
	}
	ioctl(fd, FIFO_IOC_CLEAR);

	/* 1. 写进去，再读出来 */
	/* TODO 1：用 write() 往 fd 写入字符串 "hello" 的 5 个字节，返回值存到 n。
	 *   原型：ssize_t write(int fd, const void *buf, size_t count);
	 *   内核里对应的是 fifo.c 的 fifo_write()。 */

	check("write 写入 5 个字节", n == 5);

	/* 2. ioctl 查询长度 */
	/* TODO 2：用 ioctl() 发送 FIFO_IOC_GET_LEN 命令，结果写到 len。
	 *   原型：int ioctl(int fd, unsigned long request, ...);
	 *   第三个参数传 len 的地址。驱动里用 put_user() 把值写回这个地址。 */

	check("FIFO_IOC_GET_LEN 返回 5", len == 5);

	memset(buf, 0, sizeof(buf));
	n = -1;
	/* TODO 3：用 read() 从 fd 读，最多读 sizeof(buf) 字节，返回值存到 n。
	 *   read 返回的是实际读到的字节数，可能比你要的少。 */

	check("read 读回 \"hello\"", n == 5 && memcmp(buf, "hello", 5) == 0);

	/* 3. 非阻塞读：缓冲区空的时候应该立刻返回 -1，errno = EAGAIN */
	/* TODO 4：再打开一次 DEV，这次加上 O_NONBLOCK 标志，存到 nfd。
	 *   标志用 | 组合：O_RDWR | ...
	 *   驱动里 file->f_flags & O_NONBLOCK 判断的就是这个。 */

	n = read(nfd, buf, sizeof(buf));
	check("非阻塞读空缓冲区：返回 -1 且 errno == EAGAIN", nfd >= 0 && n == -1 && errno == EAGAIN);

	/* 4. 阻塞读：子进程 200 ms 后写入，父进程的 read 应该睡到那时才返回 */
	pid_t pid = fork();
	if (pid == 0) {
		int cfd = open(DEV, O_WRONLY);

		usleep(200 * 1000);
		if (write(cfd, "wake", 4) != 4)
			_exit(1);
		_exit(0);
	}
	double t0 = now_ms();
	n = read(fd, buf, sizeof(buf));
	double dt = now_ms() - t0;
	waitpid(pid, NULL, 0);
	check("阻塞读等到了子进程写入的 \"wake\"", n == 4 && memcmp(buf, "wake", 4) == 0);
	printf("       （read 睡了 %.0f ms）\n", dt);
	check("阻塞读确实睡了大约 200 ms（150~400）", dt > 150 && dt < 400);

	/* 5. poll：空的时候等 100 ms 超时；有数据时立刻返回 POLLIN */
	int r1 = -1, r2 = -1;
	struct pollfd pfd = {0};
	/* TODO 5：
	 *   (1) 设置 pfd：.fd = fd，.events = POLLIN（表示「我关心可读」）
	 *   (2) r1 = poll(&pfd, 1, 100);   缓冲区空，应该等满 100 ms 后返回 0（超时）
	 *   (3) 往 fd 写 1 个字节
	 *   (4) r2 = poll(&pfd, 1, 100);   有数据了，应该立刻返回 1，pfd.revents 里有 POLLIN
	 *   驱动里对应 fifo_poll()：它不睡眠，只是告诉内核「现在能不能读写」和「该在哪个等待队列上等」。 */

	check("poll：空时超时返回 0，有数据后返回 1 且带 POLLIN",
	      r1 == 0 && r2 == 1 && (pfd.revents & POLLIN));
	ioctl(fd, FIFO_IOC_CLEAR);

	/* 6. 写满：非阻塞写 8192 字节，只能写进 4096，再写返回 EAGAIN */
	memset(buf, 'A', sizeof(buf));
	ssize_t w1 = write(nfd, buf, 8192);
	ssize_t w2 = write(nfd, buf, 1);
	int e2 = errno;
	check("写满：第一次只写进 4096 字节，第二次返回 EAGAIN", w1 == 4096 && w2 == -1 && e2 == EAGAIN);

	/* 7. 统计信息 */
	/* TODO 6：用 ioctl() 发送 FIFO_IOC_GET_STATS，结果写到结构体 st。
	 *   和 TODO 2 一样，只是这次传的是结构体的地址；驱动里用 copy_to_user() 拷贝整个结构体。 */

	printf("       （stats: written=%llu read=%llu len=%u capacity=%u）\n",
	       (unsigned long long)st.bytes_written, (unsigned long long)st.bytes_read, st.len, st.capacity);
	check("FIFO_IOC_GET_STATS：len 4096，capacity 4096", st.len == 4096 && st.capacity == 4096);

	ioctl(fd, FIFO_IOC_CLEAR);
	if (nfd >= 0)
		close(nfd);
	close(fd);
	printf("\n通过 %d 项，失败 %d 项\n", pass, fail);
	if (!fail)
		printf("全部通过！\n");
	return fail ? 1 : 0;
}
