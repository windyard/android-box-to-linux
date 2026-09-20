#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

static char last[256];
static char gpio[256];

static void snap(void)
{
	FILE *f;
	last[0] = gpio[0] = 0;
	f = popen("dmesg | tail -1", "r");
	if (f) { if (!fgets(last, sizeof(last), f)) last[0] = 0; pclose(f); last[strcspn(last, "\n")] = 0; }
	f = popen("grep -a 'gpio-486' /sys/kernel/debug/gpio", "r");
	if (f) { if (!fgets(gpio, sizeof(gpio), f)) gpio[0] = 0; pclose(f); gpio[strcspn(gpio, "\n")] = 0; }
}

int main(void)
{
	static const unsigned char types[] = { 'w', 'W', 'a', 'm', 'v' };
	static const int dirs[] = { 0, 1, 2, 3 };
	static const unsigned sizes[] = { 0, 4, 8, 16 };
	static const int nrs[] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
	int ti, di, si, ni;
	int fd = open("/dev/wifi_power", O_RDWR);

	if (fd < 0) { perror("open"); return 1; }
	snap();
	printf("entry: %s | %s\n", last, gpio);
	for (ti = 0; ti < (int)(sizeof(types)/sizeof(*types)); ti++)
	for (di = 0; di < 4; di++)
	for (si = 0; si < (int)(sizeof(sizes)/sizeof(*sizes)); si++)
	for (ni = 0; ni < (int)(sizeof(nrs)/sizeof(*nrs)); ni++) {
		unsigned cmd = ((unsigned)dirs[di] << 30) | (sizes[si] << 16) |
			((unsigned)types[ti] << 8) | nrs[ni];
		unsigned char buf[32];
		int r, e;
		memset(buf, 0, sizeof(buf));
		buf[0] = 1;
		errno = 0;
		r = ioctl(fd, cmd, buf);
		e = errno;
		if (r == 0 || (r < 0 && e != ENOTTY && e != EINVAL)) {
			usleep(120000);
			snap();
			if (strstr(last, "default")) continue;
			printf("cmd=%#010x ret=%d err=%d | %s | %s\n", cmd, r, e, last, gpio);
			fflush(stdout);
		}
	}
	printf("=== end state ===\n");
	snap();
	printf("%s\n%s\n", last, gpio);
	close(fd);
	return 0;
}
