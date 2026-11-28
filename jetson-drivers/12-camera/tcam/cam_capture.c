// cam_capture：最小的 V4L2 mmap 流式采集程序，不依赖任何库。
//   ./cam_capture <设备> <帧数> [输出文件]
// 输出：每帧一行「FRAME 帧号 时间戳(us) 字节数」，最后一行
//   SUMMARY frames=采到的帧数 dropped=丢帧数 fps=按时间戳算的帧率
// 给了输出文件时，把所有帧原样拼起来写进去（用 ffplay 或 YUView 可以看，见 README）。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define NBUF 4

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

int main(int argc, char **argv)
{
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_requestbuffers req = { 0 };
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	void *mem[NBUF];
	size_t len[NBUF];
	FILE *out = NULL;
	int fd, n, got = 0;
	long long first_ts = 0, last_ts = 0;
	unsigned int first_seq = 0, last_seq = 0;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <device> <frames> [outfile]\n", argv[0]);
		return 1;
	}
	n = atoi(argv[2]);
	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	if (argc > 3 && !(out = fopen(argv[3], "wb"))) {
		perror(argv[3]);
		return 1;
	}

	// 1. 当前格式（用 v4l2-ctl --set-fmt-video 预先设好，这里只读出来）
	if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
		perror("VIDIOC_G_FMT");
		return 1;
	}
	fprintf(stderr, "format %.4s %ux%u, %u bytes/frame\n", (char *)&fmt.fmt.pix.pixelformat,
		fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.sizeimage);

	// 2. 让驱动分配 NBUF 个缓冲区，再逐个映射到用户空间
	req.count = NBUF;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
		perror("VIDIOC_REQBUFS");
		return 1;
	}
	for (unsigned i = 0; i < req.count; i++) {
		struct v4l2_buffer b = { .type = req.type, .memory = req.memory, .index = i };

		if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
			perror("VIDIOC_QUERYBUF");
			return 1;
		}
		len[i] = b.length;
		mem[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
		if (mem[i] == MAP_FAILED) {
			perror("mmap");
			return 1;
		}
		// 3. 把空缓冲区交给驱动（入队），驱动会往里填帧
		if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
			perror("VIDIOC_QBUF");
			return 1;
		}
	}

	// 4. 开始采集
	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		perror("VIDIOC_STREAMON");
		return 1;
	}

	while (got < n) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
		long long ts;

		if (poll(&p, 1, 2000) <= 0) {
			fprintf(stderr, "timeout waiting for a frame\n");
			break;
		}
		// TODO 1：取一帧。
		//   (1) VIDIOC_DQBUF：从驱动那里取出一个填好的缓冲区（出队）。b.index 告诉你是哪一个，b.bytesused 是有效字节数；
		//       失败就 perror 然后 break。
		//   (2) 有输出文件时，把 mem[b.index] 开头的 b.bytesused 字节 fwrite 进去。
		break;

		ts = b.timestamp.tv_sec * 1000000LL + b.timestamp.tv_usec;
		if (!got) {
			first_ts = ts;
			first_seq = b.sequence;
		}
		last_ts = ts;
		last_seq = b.sequence;
		printf("FRAME %u %lld %u\n", b.sequence, ts, b.bytesused);
		got++;

		// TODO 2：VIDIOC_QBUF，把这个缓冲区再交给驱动，失败就 perror 然后 break。
		//   注意要放在用完 b 的时间戳和帧号之后：QBUF 会改写 b。忘了 QBUF 会怎样？（练习 1）
	}

	xioctl(fd, VIDIOC_STREAMOFF, &type);
	for (unsigned i = 0; i < req.count; i++)
		munmap(mem[i], len[i]);
	if (out)
		fclose(out);
	close(fd);

	printf("SUMMARY frames=%d dropped=%u fps=%.2f\n", got,
	       got ? last_seq - first_seq + 1 - got : 0,
	       got > 1 ? (got - 1) * 1e6 / (last_ts - first_ts) : 0.0);
	return got == n ? 0 : 1;
}
