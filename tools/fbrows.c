/* Horizontal colour bands, top to bottom, to map the visible vertical extent.
   fbrows [dev] [n] [hold_s] */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
    int n = argc > 2 ? atoi(argv[2]) : 8;
    int hold = argc > 3 ? atoi(argv[3]) : 20;
    unsigned pal[] = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                      0xff00ff, 0xff0000, 0x0000ff, 0x804000};
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) { perror("ioctl"); return 1; }
    unsigned char *p = mmap(0, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    for (unsigned y = 0; y < v.yres; y++) {
        unsigned *row = (unsigned *)(p + (size_t)y * f.line_length);
        int i = (int)(y * (unsigned)n / v.yres);
        unsigned c = pal[i % 8];
        unsigned px = 0xff000000u | ((c >> 16) << v.red.offset) |
                      ((c >> 8 & 0xff) << v.green.offset) | ((c & 0xff) << v.blue.offset);
        for (unsigned x = 0; x < v.xres; x++) row[x] = px;
    }
    msync(p, f.smem_len, MS_SYNC);
    v.yoffset = 0;
    v.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    ioctl(fd, FBIOPUT_VSCREENINFO, &v);
    ioctl(fd, FBIOPAN_DISPLAY, &v);
    printf("%s %ux%u rows=%d order top->bottom: W Y C G M R B brown\n", dev, v.xres, v.yres, n);
    fflush(stdout);
    sleep(hold);
    return 0;
}
