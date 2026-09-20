/* fbinfo: print a framebuffer's physical base + geometry only (no writes). */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : "/dev/fb0";
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror(dev); return 1; }
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) || ioctl(fd, FBIOGET_VSCREENINFO, &var)) {
        perror("ioctl"); return 1;
    }
    printf("%s: smem_start=0x%lx smem_len=%u line_len=%u | %ux%u virt=%ux%u bpp=%u | accel=%u type=%u visual=%u\n",
           dev, (unsigned long)fix.smem_start, fix.smem_len, fix.line_length,
           var.xres, var.yres, var.xres_virtual, var.yres_virtual,
           var.bits_per_pixel, fix.accel, fix.type, var.red.offset);
    close(fd);
    return 0;
}
