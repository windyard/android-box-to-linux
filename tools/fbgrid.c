/* Paint a labelled tile grid so geometry vs. vertical fade can be told apart.
   fbgrid [dev] [cols] [rows] [hold_s]  -- 16bpp RGB565 or 32bpp with alpha=ff. */
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
    int cols = argc > 2 ? atoi(argv[2]) : 6;
    int rows = argc > 3 ? atoi(argv[3]) : 4;
    int hold = argc > 4 ? atoi(argv[4]) : 15;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) { perror("ioctl"); return 1; }
    unsigned char *p = mmap(0, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    /* saturated palette: R G B Y M C W, then repeat darkened */
    unsigned pal[] = {0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0xff00ff, 0x00ffff, 0xffffff};
    int npal = sizeof(pal) / sizeof(pal[0]);
    int bpp = v.bits_per_pixel;
    for (unsigned y = 0; y < v.yres; y++) {
        int ty = (int)(y * rows / v.yres);
        for (unsigned x = 0; x < v.xres; x++) {
            int tx = (int)(x * cols / v.xres);
            int idx = (ty * cols + tx) % npal;
            unsigned c = pal[idx];
            if (ty >= rows / 2)
                c = (c >> 1) | (c & 0x808080); /* lower half half-brightness */
            unsigned char *at = p + (size_t)y * f.line_length + (size_t)x * (bpp / 8);
            if (bpp == 16) {
                unsigned r = (c >> 19) & 0x1f, g = (c >> 10) & 0x3f, b = (c >> 1) & 0x1f;
                *(unsigned short *)at = (r << 11) | (g << 5) | b;
            } else {
                unsigned r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
                unsigned px = (r << v.red.offset) | (g << v.green.offset) | (b << v.blue.offset);
                if (v.transp.length)
                    px |= 0xffu << v.transp.offset;
                *(unsigned *)at = px;
            }
        }
    }
    msync(p, f.smem_len, MS_SYNC);
    v.yoffset = 0;
    v.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    int r1 = ioctl(fd, FBIOPUT_VSCREENINFO, &v);
    int r2 = ioctl(fd, FBIOPAN_DISPLAY, &v);
    printf("%s %ux%u bpp=%u grid=%dx%d (lower half half-brightness) put=%d pan=%d\n",
           dev, v.xres, v.yres, bpp, cols, rows, r1, r2);
    fflush(stdout);
    sleep(hold);
    return 0;
}
