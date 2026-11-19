/* mmrd BASE LEN: dump 32-bit words of physical MMIO, aligned reads only */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	unsigned long base = strtoul(argv[1], 0, 0), len = strtoul(argv[2], 0, 0);
	unsigned long pbase = base & ~0xfffUL, off = base - pbase;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("open"); return 1; }
	volatile uint32_t *m = mmap(0, len + off, PROT_READ, MAP_SHARED, fd, pbase);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	for (unsigned long i = 0; i < len; i += 4) {
		uint32_t v = m[(off + i) / 4];
		if (v) printf("%08lx %08x\n", base + i, v);
	}
	return 0;
}
