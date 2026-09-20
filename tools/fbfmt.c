/* Drop the framebuffer's alpha channel (XRGB8888) so the Amlogic OSD stops
   honouring per-pixel alpha -- Xorg's fbdev driver writes 0x00RRGGBB, which is
   fully transparent while transp.length==8.  Usage: fbfmt [dev] [len] */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
    int len = argc > 2 ? atoi(argv[2]) : 0;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v)) { perror("get var"); return 1; }
    printf("before rgba=%u/%u,%u/%u,%u/%u transp=%u/%u %ux%u-%ux%u bpp=%u\n",
           v.red.offset, v.red.length, v.green.offset, v.green.length,
           v.blue.offset, v.blue.length, v.transp.offset, v.transp.length,
           v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.bits_per_pixel);
    v.transp.offset = 0;
    v.transp.length = len;
    v.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &v))
        perror("put var");
    ioctl(fd, FBIOGET_VSCREENINFO, &v);
    ioctl(fd, FBIOGET_FSCREENINFO, &f);
    printf("after  rgba=%u/%u,%u/%u,%u/%u transp=%u/%u %ux%u xv=%u yv=%u bpp=%u smem=0x%x ll=%u\n",
           v.red.offset, v.red.length, v.green.offset, v.green.length,
           v.blue.offset, v.blue.length, v.transp.offset, v.transp.length,
           v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.bits_per_pixel,
           (unsigned)f.smem_start, f.line_length);
    return 0;
}
