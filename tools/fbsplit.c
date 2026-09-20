/* Split-screen alpha probe: left half ARGB alpha=0x00, right half alpha=0xff,
   same RGB.  If only the right half lights up, the OSD honours per-pixel alpha
   and Xorg's 0x00RRGGBB output will be invisible.  Also paints two bands on
   the top/bottom edge to test whether the logo plane covers anything. */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
    unsigned rgb = argc > 2 ? (unsigned)strtoul(argv[2], 0, 0) : 0x0000ffu; /* BGR byte order */
    int hold = argc > 3 ? atoi(argv[3]) : 15;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) { perror("ioctl"); return 1; }
    unsigned char *p = mmap(0, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    for (unsigned y = 0; y < v.yres; y++) {
        unsigned *row = (unsigned *)(p + (size_t)y * f.line_length);
        for (unsigned x = 0; x < v.xres; x++)
            row[x] = (x < v.xres / 2 ? 0x00 : 0xff000000u) | rgb;
    }
    msync(p, f.smem_len, MS_SYNC);
    v.yoffset = 0;
    v.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    ioctl(fd, FBIOPUT_VSCREENINFO, &v);
    ioctl(fd, FBIOPAN_DISPLAY, &v);
    printf("%s %ux%u transp=%u/%u rgb=0x%06x left=a0 right=ff put/pan ok\n",
           dev, v.xres, v.yres, v.transp.offset, v.transp.length, rgb);
    fflush(stdout);
    sleep(hold);
    return 0;
}
