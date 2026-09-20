#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

int main(int argc, char **argv)
{
	unsigned long cmd = strtoul(argv[1], NULL, 0);
	int val = argc > 2 ? atoi(argv[2]) : 1;
	unsigned char buf[32];
	int fd, r;

	memset(buf, 0, sizeof(buf));
	buf[0] = (unsigned char)val;
	fd = open("/dev/wifi_power", O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	errno = 0;
	r = ioctl(fd, cmd, buf);
	printf("cmd=%#lx arg=%d -> %d errno=%d (%s)\n", cmd, val, r, errno, strerror(errno));
	fflush(stdout);
	usleep(300000);
	close(fd);
	return 0;
}
