/* fbflip: force meson-fb to retarget the OSD canvas by doing REAL page flips.
 * Sets yres_virtual = 2*yres, paints page0 magenta and page1 cyan, then pans
 * between them so the driver updates the scan canvas on each vsync. */
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
    const char *dev = (argc > 1) ? argv[1] : "/dev/fb0";
    int iters = (argc > 2) ? atoi(argv[2]) : 8;
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    struct fb_var_screeninfo var, orig;
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &orig) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
        perror("get"); return 1;
    }
    var = orig;
    var.xres = orig.xres; var.yres = orig.yres;
    var.xres_virtual = orig.xres;
    var.yres_virtual = (unsigned)orig.yres * 2;
    var.bits_per_pixel = 32;
    var.activate = FB_ACTIVATE_NOW;
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &var)) perror("FBIOPUT(2page)");
    ioctl(fd, FBIOGET_VSCREENINFO, &var);
    printf("now %ux%u virt=%ux%u ll=%u\n", var.xres, var.yres,
           var.xres_virtual, var.yres_virtual, fix.line_length);

    size_t page = (size_t)fix.line_length * var.yres;
    size_t maplen = (size_t)fix.line_length * var.yres_virtual;
    unsigned char *p = mmap(0, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    /* page0 magenta (B=ff,R=ff), page1 cyan (B=ff,G=ff) */
    for (size_t i = 0; i + 3 < page; i += 4) { p[i]=0xFF; p[i+1]=0x00; p[i+2]=0xFF; p[i+3]=0xFF; }
    for (size_t i = 0; i + 3 < page; i += 4) { unsigned char *c=p+page+i; c[0]=0xFF; c[1]=0xFF; c[2]=0x00; c[3]=0xFF; }

    for (int k = 0; k < iters; k++) {
        unsigned off = (k & 1) ? var.yres : 0;
        var.yoffset = off; var.xoffset = 0; var.activate = FB_ACTIVATE_NOW;
        if (ioctl(fd, FBIOPAN_DISPLAY, &var)) { perror("FBIOPAN"); break; }
        printf("pan yoffset=%u (%s)\n", off, (k&1)?"cyan":"magenta");
        fflush(stdout);
        usleep(500000);
    }
    munmap(p, maplen); close(fd);
    return 0;
}
