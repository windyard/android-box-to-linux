/* mem_paint: fill a physical memory range via /dev/mem to test which canvas the
 * HDMI VPU is actually scanning.  Read-only-safe: it only writes pixels.
 * usage: mem_paint PHYS_HEX RRGGBB MEGABYTES  [bpp=32]
 *   e.g. mem_paint 0x3f800000 ff0000 8 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: mem_paint PHYS RRGGBB MB [bpp]\n"); return 2; }
    uintptr_t phys = (uintptr_t)strtoull(argv[1], 0, 16);
    unsigned long col = strtoul(argv[2], 0, 16);
    size_t mb = strtoul(argv[3], 0, 10);
    int bpp = (argc > 4) ? atoi(argv[4]) : 32;
    size_t len = mb * 1024 * 1024;
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t base = phys & ~(uintptr_t)(ps - 1);
    size_t off = phys - base;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    size_t maplen = off + len;
    volatile uint8_t *p = mmap(0, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)base);
    if (p == MAP_FAILED) { perror("mmap phys"); return 1; }
    uint8_t *q = (uint8_t *)p + off;
    unsigned r = (col >> 16) & 0xFF, g = (col >> 8) & 0xFF, b = col & 0xFF;
    if (bpp == 32) {
        uint32_t *d = (uint32_t *)q;
        uint32_t val = (0xFFu << 24) | (r << 16) | (g << 8) | b; /* ARGB */
        for (size_t i = 0; i < len / 4; i++) d[i] = val;
    } else {
        for (size_t i = 0; i < len; i++) q[i] = (uint8_t)col;
    }
    printf("phys 0x%lx base 0x%lx off 0x%lx len %zu painted 0x%06lx\n",
           (unsigned long)phys, (unsigned long)base, (unsigned long)off, len, col);
    munmap((void *)p, maplen);
    close(fd);
    return 0;
}
