#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifndef SYS_reboot
#define SYS_reboot 142   /* ARM EABI */
#endif

/* Stable Linux reboot ABI constants (from uapi/linux/reboot.h). */
#define MAGIC1     0xfee1dead
#define MAGIC2     672274793
#define CMD_RESTART2 0xA1B2C3D4

/* Amlogic reboot-mode latch, same path as Android `reboot <mode>`:
   reboot(CMD_RESTART2, mode). u-boot get_rebootmode returns <mode>, and
   switch_bootmode then runs `update` -> fatloads aml_autoscript from USB.
     reboot_mode update           -> real: reboot + latch "update"
     reboot_mode update badmagic  -> dry run: forces EINVAL, does NOT reboot */
int main(int argc, char **argv) {
	const char *mode = (argc > 1) ? argv[1] : "update";
	int m2 = (argc > 2 && strcmp(argv[2], "badmagic") == 0) ? 0 : MAGIC2;
	printf("reboot(magic1=0x%x,magic2=0x%x,CMD_RESTART2,arg=\"%s\")\n",
	       MAGIC1, m2, mode);
	fflush(stdout);
	long r = syscall(SYS_reboot, MAGIC1, m2, CMD_RESTART2, (void *)mode);
	fprintf(stderr, "-> ret=%ld errno=%d (%s)  [returned => did NOT reboot]\n",
	        r, errno, strerror(errno));
	return errno ? errno : 1;
}
