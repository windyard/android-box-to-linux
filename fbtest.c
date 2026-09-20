#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

/* Paint /dev/fb0 repeatedly so the scanout path can be read off the TV.
 * argv: fbtest [hold_s] [pattern] [fbdev]  (fbdev defaults to /dev/fb0)
 *   0 bands(B,G,R)  1 white  2 black  3 red  4 green  5 blue  6 checker
 * The panel reports red_off=16, i.e. ARGB8888, so 0x00RRGGBB maps directly. */
int main(int argc, char **argv)
{
	int fd, hold = 8, pat = 0;
	const char *dev = "/dev/fb0";
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	unsigned char *base;
	unsigned x, y;
	size_t sz;
	int t;

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
		perror("ioctl vsinfo");
		return 1;
	}
	printf("xres=%u yres=%u xv=%u yv=%u bpp=%u yoffset=%u red_off=%u green_off=%u blue_off=%u\n",
	       v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.bits_per_pixel,
	       v.yoffset, v.red.offset, v.green.offset, v.blue.offset);
	printf("smem_len=%u line_length=%u visual=%u pattern=%d\n",
	       f.smem_len, f.line_length, f.visual, pat);
	if (v.bits_per_pixel != 32) {
		printf("unsupported bpp\n");
		return 1;
	}

	sz = f.smem_len ? f.smem_len : (size_t)f.line_length * v.yres_virtual;
	base = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		    (off_t)v.yoffset * f.line_length);
	if (base == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	for (t = 0; t < hold * 5; t++) {
		for (y = 0; y < v.yres; y++) {
			unsigned int *row = (unsigned int *)(base + (size_t)y * f.line_length);
			for (x = 0; x < v.xres; x++) {
				unsigned int c;

				switch (pat) {
				case 1: c = 0x00ffffff; break;
				case 2: c = 0x00000000; break;
				case 3: c = 0x00ff0000; break;
				case 4: c = 0x0000ff00; break;
				case 5: c = 0x000000ff; break;
				case 6: c = (((x / 64) + (y / 64)) & 1) ? 0x00ffffff : 0x00000000; break;
				default: {
					unsigned i = (x * 3) / (v.xres ? v.xres : 1);
					c = i == 0 ? 0x000000ff : (i == 1 ? 0x0000ff00 : 0x00ff0000);
					break;
				}
				}
				row[x] = c;
			}
		}
		msync(base, sz, MS_SYNC);
		usleep(200000);
	}
	printf("done\n");
	return 0;
}
