#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

static void note(void)
{
	printf("   >>> interesting; gpio486 now:\n");
	fflush(stdout);
	system("grep -a 'gpio-486\\|gpio-498' /sys/kernel/debug/gpio 2>&1 | sed 's/^/        /'");
	printf("   >>> dmesg tail:\n");
	fflush(stdout);
	system("dmesg | tail -4 | sed 's/^/        /'");
}

int main(void)
{
	static const unsigned char types[] = { 'w', 'W', 'a', 'm', 'v', 0x00 };
	static const int dirs[] = { 0, 1, 2, 3 };
	static const unsigned sizes[] = { 0, 4, 8, 16 };
	static const int nrs[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
	int ti, di, si, ni;
	int fd = open("/dev/wifi_power", O_RDWR);
	int handled = 0;

	if (fd < 0) { perror("open /dev/wifi_power"); return 1; }
	printf("opened /dev/wifi_power fd=%d\n", fd);
	for (ti = 0; ti < (int)(sizeof(types)/sizeof(types[0])); ti++) {
		for (di = 0; di < 4; di++) {
			for (si = 0; si < (int)(sizeof(sizes)/sizeof(sizes[0])); si++) {
				for (ni = 0; ni < (int)(sizeof(nrs)/sizeof(nrs[0])); ni++) {
					unsigned cmd = ((unsigned)dirs[di] << 30) |
						(sizes[si] << 16) | ((unsigned)types[ti] << 8) | nrs[ni];
					unsigned char buf[32];
					int r;
					int e;
					memset(buf, 0, sizeof(buf));
					buf[0] = 1;
					errno = 0;
					r = ioctl(fd, cmd, buf);
					e = errno;
					if (r == 0) {
						printf("OK   cmd=%#010x type=%#02x dir=%d size=%u nr=%d -> 0\n",
							cmd, types[ti], dirs[di], sizes[si], nrs[ni]);
						handled++; note();
					} else if (e != ENOTTY) {
						printf("ERR  cmd=%#010x type=%#02x dir=%d size=%u nr=%d -> %d errno=%d (%s)\n",
							cmd, types[ti], dirs[di], sizes[si], nrs[ni], r, e, strerror(e));
						if (e != EINVAL && e != EFAULT) { handled++; note(); }
					}
				}
			}
		}
	}
	printf("done, %d non-ENOTTY results\n", handled);
	close(fd);
	return 0;
}
