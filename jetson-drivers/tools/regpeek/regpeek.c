// SPDX-License-Identifier: GPL-2.0
/*
 * regpeek：通过 /dev/mem 读外设寄存器（只读，不提供写功能）。
 * 用法：sudo ./regpeek <物理地址> [个数]
 *   sudo ./regpeek 0x32a0000       读风扇 PWM 控制器的第一个寄存器
 *
 * 能读的前提：CONFIG_DEVMEM=y，且没开 CONFIG_IO_STRICT_DEVMEM（本板就是这样）。
 * 只读设备寄存器（MMIO），不要读内存（RAM），STRICT_DEVMEM 会拒绝。
 * 只读正在工作（时钟已打开）的外设！读时钟关着的外设会触发总线超时：
 * dmesg 里出现一大段 CBB 错误（TIMEOUT_ERR），读到 0xffffffff，
 * 而且总线桥会把这个外设「封掉」，之后连内核驱动也访问不了，直到重启。见第 4 章。
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
	unsigned long addr, page, off;
	long psz = sysconf(_SC_PAGESIZE);
	int count = 1, fd, i;
	volatile uint32_t *p;
	void *map;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <phys addr> [count]\n", argv[0]);
		return 2;
	}
	addr = strtoul(argv[1], NULL, 0);
	if (argc > 2)
		count = atoi(argv[2]);
	if (addr & 3 || count < 1 || count > 64) {
		fprintf(stderr, "address must be 4-byte aligned, count 1..64\n");
		return 2;
	}

	page = addr & ~(psz - 1);
	off = addr - page;
	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		perror("open /dev/mem");
		return 1;
	}
	map = mmap(NULL, psz, PROT_READ, MAP_SHARED, fd, page);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	p = (volatile uint32_t *)((char *)map + off);
	for (i = 0; i < count && off + i * 4 < (unsigned long)psz; i++)
		printf("0x%08lx: 0x%08x\n", addr + i * 4, p[i]);

	munmap(map, psz);
	close(fd);
	return 0;
}
