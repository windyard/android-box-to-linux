/* Depth-agnostic solid fill: fbfill [dev] [rrggbb] [hold_s]
   16bpp -> RGB565, 32bpp -> per declared var bitfields, alpha forced to 0xff. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

static unsigned ch(unsigned v, struct fb_bitfield fb)
{
    return (v & ((1u << fb.length) - 1)) << fb.offset;
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
    unsigned rgb = argc > 2 ? (unsigned)strtoul(argv[2], 0, 16) : 0x00ff00u;
    int hold = argc > 3 ? atoi(argv[3]) : 10;
    unsigned r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) { perror("ioctl"); return 1; }
    unsigned pix;
    if (v.bits_per_pixel == 16) {
        unsigned rr = r >> 3, gg = g >> 2, bb = b >> 3;
        pix = (rr << 11) | (gg << 5) | bb;
    } else {
        pix = ch(r, v.red) | ch(g, v.green) | ch(b, v.blue);
        if (v.transp.length)
            pix |= (0xffu << v.transp.offset);
    }
    unsigned char *p = mmap(0, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    if (v.bits_per_pixel == 16) {
        unsigned short *q = (unsigned short *)p;
        for (unsigned y = 0; y < v.yres; y++) {
            unsigned short *row = q + (size_t)y * (f.line_length / 2);
            for (unsigned x = 0; x < v.xres; x++) row[x] = pix;
        }
    } else {
        unsigned *q = (unsigned *)p;
        for (unsigned y = 0; y < v.yres; y++) {
            unsigned *row = q + (size_t)y * (f.line_length / 4);
            for (unsigned x = 0; x < v.xres; x++) row[x] = pix;
        }
    }
    msync(p, f.smem_len, MS_SYNC);
    v.yoffset = 0;
    v.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    int r1 = ioctl(fd, FBIOPUT_VSCREENINFO, &v);
    int r2 = ioctl(fd, FBIOPAN_DISPLAY, &v);
    printf("%s %ux%u bpp=%u transp=%u/%u pix=0x%x put=%d pan=%d\n",
           dev, v.xres, v.yres, v.bits_per_pixel, v.transp.offset, v.transp.length, pix, r1, r2);
    fflush(stdout);
    sleep(hold);
    return 0;
}
