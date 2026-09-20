#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

/* Amlogic's VIU latches the OSD canvas on a pan/activate ioctl rather than on
 * writes, which is what Android's graphics backend issues every frame. Paint,
 * then keep issuing FBIOPUT_VSCREENINFO(FORCE) + FBIOPAN_DISPLAY so osd0 is
 * actually re-driven. argv: fbpan [hold_s] [pattern] [fbdev] */
int main(int argc, char **argv)
{
	int fd, hold = 10, pat = 0, i, screens = 1, rc_put = -1, rc_pan = -1;
	const char *dev = "/dev/fb0";
	struct fb_var_screeninfo v, v0;
	struct fb_fix_screeninfo f;
	unsigned char *base;
	unsigned x, y;
	size_t sz;

	if (argc > 1)
		hold = atoi(argv[1]);
	if (argc > 2)
		pat = atoi(argv[2]);
	if (argc > 3)
		dev = argv[3];

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror(dev);
		return 1;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) {
		perror("ioctl");
		return 1;
	}
	v0 = v;
	printf("%s xres=%u yres=%u xv=%u yv=%u bpp=%u line_length=%u smem_len=%u\n",
	       dev, v.xres, v.yres, v.xres_virtual, v.yres_virtual,
	       v.bits_per_pixel, f.line_length, f.smem_len);
	if (v.yres_virtual < 2 * v.yres) {
		printf("requesting double-height virtual buffer\n");
		v.yres_virtual = 2 * v.yres;
		v.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
		if (ioctl(fd, FBIOPUT_VSCREENINFO, &v))
			perror("put vscreeninfo (grow)");
		ioctl(fd, FBIOGET_VSCREENINFO, &v);
		ioctl(fd, FBIOGET_FSCREENINFO, &f);
		printf("now yv=%u smem_len=%u line_length=%u\n",
		       v.yres_virtual, f.smem_len, f.line_length);
	}

	screens = (v.yres_virtual >= 2 * v.yres) ? 2 : 1;
	sz = (size_t)f.line_length * v.yres * screens;
	if (f.smem_len && sz > f.smem_len) {
		printf("clamping map to smem_len=%u\n", f.smem_len);
		sz = f.smem_len;
		screens = 1;
	}
	base = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (base == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	for (i = 0; i < hold * 5; i++) {
		unsigned off = (screens == 2 && (i & 1)) ? v.yres : 0;

		for (y = 0; y < v.yres; y++) {
			unsigned int *row = (unsigned int *)(base + (size_t)(off + y) * f.line_length);
			for (x = 0; x < v.xres; x++) {
				unsigned int c;

				switch (pat) {
				case 1: c = 0x00ffffff; break;
				case 3: c = 0x00ff0000; break;
				case 4: c = 0x0000ff00; break;
				case 5: c = 0x000000ff; break;
				case 6: c = (((x / 64) + (y / 64)) & 1) ? 0x00ffffff : 0x00000000; break;
				default: {
					unsigned s = (x * 3) / (v.xres ? v.xres : 1);
					c = s == 0 ? 0x000000ff : (s == 1 ? 0x0000ff00 : 0x00ff0000);
					break;
				}
				}
				row[x] = c;
			}
		}
		v.yoffset = off;
		v.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
		rc_put = ioctl(fd, FBIOPUT_VSCREENINFO, &v);
		rc_pan = ioctl(fd, FBIOPAN_DISPLAY, &v);
		if (i == 0)
			printf("first frame: put=%d pan=%d yoffset=%u\n", rc_put, rc_pan, off);
		usleep(200000);
	}
	v = v0;
	printf("exiting after %d frames (last put=%d pan=%d)\n", i, rc_put, rc_pan);
	return 0;
}
