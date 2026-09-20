/* Re-latch the VIU canvas to fb0's current memory WITHOUT painting:
   FBIOPUT_VSCREENINFO(NOW|FORCE, yoffset=0) + FBIOPAN_DISPLAY(yoffset=0).
   Needed after Xorg starts: X's own mode-set is non-FORCE, so the OSD scan
   keeps the stale frame (README §7.3 / §7.7.7). */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
    struct fb_var_screeninfo v;
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v)) { perror("get"); return 1; }
    v.yoffset = 0;
    v.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    int r1 = ioctl(fd, FBIOPUT_VSCREENINFO, &v);
    int r2 = ioctl(fd, FBIOPAN_DISPLAY, &v);
    printf("%s %ux%u bpp=%u put=%d pan=%d\n", dev, v.xres, v.yres, v.bits_per_pixel, r1, r2);
    return r1 || r2;
}
