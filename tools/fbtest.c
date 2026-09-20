/* fbtest: fill an Amlogic OSD framebuffer with a solid color AND flip it to the
 * active scan source (pan/activate), to prove HDMI scanout past the boot logo.
 * usage: fbtest [device] [RRGGBB]   e.g. fbtest /dev/fb0 00FF00 */
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
    const char *dev = (argc > 1) ? argv[1] : "/dev/fb0";
    unsigned long col = (argc > 2) ? strtoul(argv[2], 0, 16) : 0xFF0000UL;
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror(dev); return 1; }
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
        perror("getinfo"); return 1;
    }
    fprintf(stderr, "%s: %ux%u virt=%ux%u bpp=%u ll=%u smem=%lu\n",
            dev, var.xres, var.yres, var.xres_virtual, var.yres_virtual,
            var.bits_per_pixel, fix.line_length, (unsigned long)fix.smem_len);
    unsigned r = (col >> 16) & 0xFF, g = (col >> 8) & 0xFF, b = col & 0xFF;
    size_t len = (size_t)fix.smem_len;
    if (!len) len = (size_t)fix.line_length * var.yres_virtual;
    unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    for (size_t i = 0; i + 3 < len; i += 4) {
        p[i] = b; p[i + 1] = g; p[i + 2] = r; p[i + 3] = 0xFF;
    }
    msync(p, len, MS_SYNC);

    /* Flip: point the console at yoffset 0 and activate -> driver switches the
     * OSD scan source from the boot-logo plane to this fb. */
    var.xoffset = 0;
    var.yoffset = 0;
    var.vmode &= ~FB_VMODE_SMOOTH_XPAN;
    var.activate = FB_ACTIVATE_NOW;
    if (ioctl(fd, FBIOPAN_DISPLAY, &var))
        perror("FBIOPAN_DISPLAY");
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &var))
        perror("FBIOPUT_VSCREENINFO");
    /* re-fill after any mode reset */
    for (size_t i = 0; i + 3 < len; i += 4) {
        p[i] = b; p[i + 1] = g; p[i + 2] = r; p[i + 3] = 0xFF;
    }
    munmap(p, len);
    close(fd);
    printf("filled %s 0x%06lx + panned/activated\n", dev, col);
    return 0;
}
