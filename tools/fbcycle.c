/* Cycle osd0 through solid opaque colors, re-driving the OSD canvas each frame.
   Amlogic VIU latches the OSD scan address/window on a pan/activate ioctl, not
   on writes -- Android's graphics backend issues that every frame.
   Usage: fbcycle [dev] [period_ms] [rounds] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
    int period = argc > 2 ? atoi(argv[2]) : 2000;
    int rounds = argc > 3 ? atoi(argv[3]) : 5;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) { perror("ioctl"); return 1; }
    unsigned char *p = mmap(0, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    unsigned cols[] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff, 0xff00ffff, 0xff000000};
    const char *nm[] = {"BLUE","GREEN","RED","WHITE","CYAN","BLACK"};
    printf("%s %ux%u ll=%u transp=%u/%u yoff=%u\n", dev, v.xres, v.yres,
           f.line_length, v.transp.offset, v.transp.length, v.yoffset);
    fflush(stdout);
    for (int r = 0; r < rounds; r++) {
        for (int c = 0; c < 6; c++) {
            unsigned argb = cols[c];
            for (unsigned y = 0; y < v.yres; y++) {
                unsigned *row = (unsigned *)(p + (size_t)y * f.line_length);
                for (unsigned x = 0; x < v.xres; x++) row[x] = argb;
            }
            msync(p, f.smem_len, MS_SYNC);
            v.yoffset = 0;
            v.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
            int r1 = ioctl(fd, FBIOPUT_VSCREENINFO, &v);
            int r2 = ioctl(fd, FBIOPAN_DISPLAY, &v);
            printf("%s put=%d pan=%d\n", nm[c], r1, r2); fflush(stdout);
            usleep(period * 1000);
        }
    }
    return 0;
}
