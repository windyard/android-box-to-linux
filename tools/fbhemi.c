/* Half-screen discriminator: fill TOP or BOTTOM half with tiles, leave the
   other half black.  If the reported fade follows the content rather than the
   screen position, the ramp is in our data, not in the display path.
   fbhemi [dev] [top|bottom] [hold_s] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
    int top = (argc > 2 && strcmp(argv[2], "top") == 0) ? 1 : 0;
    int hold = argc > 3 ? atoi(argv[3]) : 18;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) { perror("ioctl"); return 1; }
    unsigned char *p = mmap(0, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    unsigned pal[] = {0xff0000, 0x00ff00, 0x0000ff, 0xffff00};
    unsigned yy0 = top ? 0 : v.yres / 2;
    unsigned yy1 = top ? v.yres / 2 : v.yres;
    for (unsigned y = 0; y < v.yres; y++) {
        unsigned *row = (unsigned *)(p + (size_t)y * f.line_length);
        int on = (y >= yy0 && y < yy1);
        for (unsigned x = 0; x < v.xres; x++) {
            if (!on) { row[x] = 0xff000000u; continue; }
            int tx = (int)(x * 4 / v.xres);
            int ty = (int)((y - yy0) * 2 / (yy1 - yy0));
            unsigned c = pal[(ty * 4 + tx) & 3];
            row[x] = 0xff000000u | c;
        }
    }
    msync(p, f.smem_len, MS_SYNC);
    v.yoffset = 0;
    v.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    int r1 = ioctl(fd, FBIOPUT_VSCREENINFO, &v);
    int r2 = ioctl(fd, FBIOPAN_DISPLAY, &v);
    printf("%s %ux%u filled %s half with R/G/B/Y tiles put=%d pan=%d\n",
           dev, v.xres, v.yres, top ? "top" : "bottom", r1, r2);
    fflush(stdout);
    sleep(hold);
    return 0;
}
