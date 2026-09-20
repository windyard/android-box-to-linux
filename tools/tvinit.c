/* tvinit - init for the R1200C's vendor 4.9 kernel.
 *
 * Brings up wired ethernet (the Amlogic PHY sometimes needs a bounce), logs to
 * the USB stick so boots are readable without a serial cable, optionally
 * switch_root's into the Alpine image on the stick, and otherwise leaves a root
 * shell on the network.  Runs as PID 1 and never exits. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/route.h>

#define MYIP     "192.0.2.126"
#define NETMASK  "255.255.255.0"
#define GW       "192.0.2.1"
#define VMIP     "192.0.2.220"
#define BINDPORT 5555
#define REVPORT  4444
#define WINDOW   900
#define STICK    "/mnt/usb"
#define ROOTMNT  "/mnt/root"
#define BB       "/bin/busybox"

extern char **environ;

static char pathenv[] = "PATH=/bin:/sbin:/usr/bin";
static char homeenv[] = "HOME=/";
static char *initenv[] = { pathenv, homeenv, NULL };

static int logfd = -1;

static void say(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    write(1, buf, n);
    if (logfd >= 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        char pre[32];
        int m = snprintf(pre, sizeof(pre), "[%5lu.%02lu] ",
                         (unsigned long)ts.tv_sec, ts.tv_nsec / 10000000);
        if (write(logfd, pre, m) == m)
            write(logfd, buf, n);
    }
}

static void reap(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

static void mount_basic(void)
{
    mkdir("/dev", 0755);
    mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
    mkdir("/proc", 0755);
    mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV, NULL);
    mkdir("/sys", 0755);
    mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV, NULL);
    mkdir("/tmp", 0777);
    mount("tmpfs", "/tmp", "tmpfs", 0, "mode=0777");
    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts", 0, NULL);
    mkdir("/mnt", 0755);
    mkdir(STICK, 0755);
    mkdir(ROOTMNT, 0755);
}

static int link_flags(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    if (fd < 0) return -1;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ - 1);
    int rc = ioctl(fd, SIOCGIFFLAGS, &ifr) < 0 ? -1 : ifr.ifr_flags;
    close(fd);
    return rc;
}

static int carrier(void)
{
    int fd = open("/sys/class/net/eth0/carrier", O_RDONLY);
    if (fd < 0) return -1;
    char c = '0';
    if (read(fd, &c, 1) < 1)
        c = '0';
    close(fd);
    return c == '1';
}

static int iface_set(const char *ifn, int up)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    if (fd < 0) return -1;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifn, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        close(fd);
        return -1;
    }
    if (up)
        ifr.ifr_flags |= IFF_UP;
    else
        ifr.ifr_flags &= ~IFF_UP;
    int rc = ioctl(fd, SIOCSIFFLAGS, &ifr);
    close(fd);
    return rc;
}

static int iface_addr(const char *ifn, const char *ip, const char *mask)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    if (fd < 0) return -1;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifn, IFNAMSIZ - 1);
    ((struct sockaddr_in *)&ifr.ifr_addr)->sin_family = AF_INET;
    inet_pton(AF_INET, ip, &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr);
    ioctl(fd, SIOCSIFADDR, &ifr);
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifn, IFNAMSIZ - 1);
    ((struct sockaddr_in *)&ifr.ifr_addr)->sin_family = AF_INET;
    inet_pton(AF_INET, mask, &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr);
    ioctl(fd, SIOCSIFNETMASK, &ifr);
    close(fd);
    return 0;
}

/* The dwmac/internal-PHY pairing on this board is unreliable on a warm
   reboot and sometimes only locks after several link cycles.  Keep bouncing
   until carrier rises; the IP/route are set regardless, so a late lock still
   gives connectivity.  Returns 1 if the link locked. */
static int net_up(void)
{
    int i, locked = 0;
    for (i = 0; i < 6 && !locked; i++) {
        iface_set("eth0", 0);
        usleep(700000);
        iface_set("eth0", 1);
        iface_addr("eth0", MYIP, NETMASK);
        for (int s = 0; s < 40 && !carrier(); s++)
            usleep(250000);
        locked = carrier();
        say("tvinit: link cycle %d carrier=%d\n", i + 1, locked);
    }
    say("tvinit: eth0 carrier=%d flags=0x%x\n", carrier(), link_flags());
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct rtentry rt;
    if (fd >= 0) {
        memset(&rt, 0, sizeof(rt));
        rt.rt_flags = RTF_UP | RTF_GATEWAY;
        ((struct sockaddr_in *)&rt.rt_dst)->sin_family = AF_INET;
        ((struct sockaddr_in *)&rt.rt_gateway)->sin_family = AF_INET;
        inet_pton(AF_INET, GW, &((struct sockaddr_in *)&rt.rt_gateway)->sin_addr);
        ioctl(fd, SIOCADDRT, &rt);
        close(fd);
    }
    return locked;
}

static int stick_mount(void)
{
    for (int i = 0; i < 12; i++) {
        if (access("/dev/sda1", F_OK) == 0)
            break;
        usleep(500000);
    }
    for (int i = 0; i < 5; i++) {
        if (mount("/dev/sda1", STICK, "vfat", MS_NOATIME, NULL) == 0)
            return 0;
        say("tvinit: stick mount try %d failed errno=%d\n", i, errno);
        umount(STICK);
        sleep(1);
    }
    say("tvinit: stick mount failed errno=%d\n", errno);
    return -1;
}

static void clear_misc(void)
{
    /* misc (mmcblk0p7) holds the reboot-mode token U-Boot reads; zeroing it
       gives a normal Android boot afterwards. */
    char z[512];
    memset(z, 0, sizeof(z));
    for (int i = 0; i < 10; i++) {
        int fd = open("/dev/mmcblk0p7", O_WRONLY);
        if (fd >= 0) {
            for (int j = 0; j < 8; j++)
                if (write(fd, z, sizeof(z)) < 0)
                    break;
            fsync(fd);
            close(fd);
            sync();
            return;
        }
        usleep(300000);
    }
    say("tvinit: misc node not found\n");
}

static void shell_on(int fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        ioctl(fd, TIOCSCTTY, 0);
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2) close(fd);
        char *av[] = { (char *)BB, (char *)"sh", NULL };
        execve(BB, av, initenv);
        _exit(127);
    }
}

static int connect_timeout(const char *ip, int port, int ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr(ip);
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    fd_set wf;
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    FD_ZERO(&wf);
    FD_SET(fd, &wf);
    if (select(fd + 1, NULL, &wf, NULL, &tv) <= 0) {
        close(fd);
        return -1;
    }
    int err = 0;
    socklen_t el = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) {
        close(fd);
        errno = err;
        return -1;
    }
    fcntl(fd, F_SETFL, fl);
    return fd;
}

static void reverse(void)
{
    int fd = connect_timeout(VMIP, REVPORT, 3000);
    if (fd < 0) {
        say("tvinit: reverse to %s:%d failed errno=%d\n", VMIP, REVPORT, errno);
        return;
    }
    say("tvinit: reverse shell connected\n");
    shell_on(fd);
    close(fd);
}

static void bind_shell(void)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(BINDPORT);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        say("tvinit: bind %d failed errno=%d\n", BINDPORT, errno);
        close(ls);
        return;
    }
    listen(ls, 4);
    say("tvinit: bind shell on :%d for %ds\n", BINDPORT, WINDOW);
    for (int t = 0; t < WINDOW; t++) {
        fd_set rf;
        struct timeval tv = { 1, 0 };
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        int r = select(ls + 1, &rf, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(ls, &rf)) {
            int fd = accept(ls, NULL, NULL);
            if (fd >= 0) {
                say("tvinit: shell client connected\n");
                shell_on(fd);
                close(fd);
            }
        }
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
    }
    close(ls);
    say("tvinit: bind window closed\n");
}

/* Mount the Alpine rootfs from the internal eMMC cache partition (mmcblk0p3).
   This is the primary root now that Linux is flashed on-board; no USB stick
   required.  Returns 0 on success. */
static int mount_emmc_root(void)
{
    /* Primary root is the big "data" partition (mmcblk0p21, ~3.6 GB) we moved
       Alpine onto; "cache" (mmcblk0p3, 1.1 GB) is kept as a fallback so a
       broken data root still boots.  The Amlogic kernel exposes each partition
       both as mmcblk0pN and under its DT name, and a recovery/boot handoff can
       win the race against the partition scan, so wait briefly for a node. */
    for (int i = 0; i < 10; i++) {
        if (access("/dev/data", F_OK) == 0 || access("/dev/cache", F_OK) == 0)
            break;
        usleep(300000);
    }
    const char *cands[] = { "/dev/data", "/dev/mmcblk0p21",
                            "/dev/cache", "/dev/mmcblk0p3", NULL };
    for (int i = 0; cands[i]; i++) {
        if (mount(cands[i], ROOTMNT, "ext4", 0, NULL) == 0) {
            say("tvinit: mounted root from %s (internal eMMC)\n", cands[i]);
            return 0;
        }
        say("tvinit: %s ext4 mount failed errno=%d\n", cands[i], errno);
    }
    return -1;
}

/* Fallback root: loop-mount the Alpine image on the USB stick.  Used only if
   the on-board rootfs is unusable, so the stick can rescue a broken eMMC. */
static int mount_usb_root(void)
{
    if (access(STICK "/rootfs.img", F_OK) != 0 && stick_mount() != 0) {
        say("tvinit: no stick for rootfs\n");
        return -1;
    }
    if (access(STICK "/rootfs.img", F_OK) != 0) {
        say("tvinit: rootfs.img missing\n");
        return -1;
    }
    if (system(BB " losetup /dev/loop0 " STICK "/rootfs.img") != 0) {
        say("tvinit: losetup failed\n");
        return -1;
    }
    if (mount("/dev/loop0", ROOTMNT, "ext4", 0, NULL) != 0) {
        say("tvinit: usb rootfs mount failed errno=%d\n", errno);
        return -1;
    }
    say("tvinit: mounted root from USB stick loop (fallback)\n");
    return 0;
}

/* Finish the handoff: ROOTMNT already holds a mounted rootfs.  busybox 1.22
   switch_root does not relocate /dev, and the Alpine image has an empty /dev,
   so PID1 init couldn't open /dev/console and would panic the kernel.  Mount a
   fresh devtmpfs (kernel auto-populates console/ttyS0/tty1/...) plus proc/sys
   into the new root, then exec switch_root.  Never returns on success. */
static void do_handoff(void)
{
    mkdir(ROOTMNT "/dev", 0755);
    if (mount("devtmpfs", ROOTMNT "/dev", "devtmpfs", 0, NULL) != 0)
        say("tvinit: devtmpfs into root failed errno=%d\n", errno);
    mkdir(ROOTMNT "/dev/pts", 0755);
    mount("devpts", ROOTMNT "/dev/pts", "devpts", 0, "gid=5,mode=620");
    mkdir(ROOTMNT "/proc", 0555);
    mount("proc", ROOTMNT "/proc", "proc", 0, NULL);
    mkdir(ROOTMNT "/sys", 0555);
    mount("sysfs", ROOTMNT "/sys", "sysfs", 0, NULL);
    say("tvinit: switch_root into " ROOTMNT "\n");
    sync();
    if (logfd >= 0) {
        fcntl(logfd, F_DUPFD_CLOEXEC, 3);
        close(logfd);
        logfd = -1;
    }
    char *av[] = { BB, "switch_root", "-c", "/dev/ttyS0", ROOTMNT, "/sbin/init", NULL };
    execve(BB, av, initenv);
    say("tvinit: exec switch_root failed errno=%d\n", errno);
}

static int read_cnt(const char *p)
{
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    char b[16] = { 0 };
    int n = read(fd, b, 15);
    close(fd);
    return n > 0 ? atoi(b) : 0;
}

static void write_cnt(const char *p, int v)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char b[16];
    int n = snprintf(b, sizeof(b), "%d\n", v);
    write(fd, b, n);
    fsync(fd);
    close(fd);
}

/* Ask U-Boot to boot the recovery partition again on the next reset, then
   hard-reboot.  A fresh boot re-initialises the flaky PHY; this is how the
   working boots came up, so we simply retry it a bounded number of times. */
static void reboot_recovery(void)
{
    int fd = open("/proc/sys/kernel/sysrq", O_WRONLY);
    if (fd >= 0) { write(fd, "1", 1); close(fd); }
    for (int t = 0; t < 5; t++) {
        fd = open("/dev/mmcblk0p7", O_WRONLY);
        if (fd >= 0) {
            char m[64];
            memset(m, 0, sizeof(m));
            strcpy(m, "boot-recovery");
            write(fd, m, sizeof(m));
            fsync(fd);
            close(fd);
            break;
        }
        usleep(300000);
    }
    sync();
    fd = open("/proc/sysrq-trigger", O_WRONLY);
    if (fd >= 0) { write(fd, "b", 1); close(fd); }
    sync();
    for (;;)
        pause();
}

int main(void)
{
    environ = initenv;
    signal(SIGCHLD, reap);
    signal(SIGPIPE, SIG_IGN);
    int cfd = open("/dev/console", O_RDWR);
    if (cfd >= 0) {
        dup2(cfd, 0);
        dup2(cfd, 1);
        dup2(cfd, 2);
        if (cfd > 2) close(cfd);
    }
    say("\n\ntvinit: R1200C ramfs linux up\n");
    mount_basic();
    clear_misc();
    system(BB " dmesg -n 1");

    /* Primary boot: Linux flashed on the internal eMMC (cache partition).  Try
       it first, without ever touching the USB stick, so a stick-free boot goes
       straight to eMMC.  Alpine brings up eth0 itself and a fresh boot re-inits
       the flaky PHY, so we don't net_up() before the handoff. */
    if (mount_emmc_root() == 0)
        do_handoff();

    /* eMMC root unusable.  Only now probe the stick: mount it for logging and
       as a rescue rootfs, so a broken on-board image is recoverable.  A
       hand-written misc boot-recovery is not honored by this U-Boot, so never
       self-reboot here -- that would drop to Android, whose restore service
       then overwrites our recovery. */
    int sticks = (stick_mount() == 0);
    if (sticks) {
        logfd = open(STICK "/tvinit.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        say("tvinit: stick mounted, logging to tvinit.log\n");
    }
    if (mount_usb_root() == 0)
        do_handoff();
    say("tvinit: no usable rootfs (eMMC or USB), staying in ramfs\n");

    /* Nothing booted.  Bring up the ramfs network stack so the box stays
     * reachable for diagnosis/repair, and self-heal a dead PHY via the stick. */
    int locked = net_up();
    say("tvinit: eth0=%s locked=%d\n", MYIP, locked);
    if (locked) {
        if (sticks)
            unlink(STICK "/phyfail");
    } else if (sticks) {
        int n = read_cnt(STICK "/phyfail");
        if (n < 12) {
            write_cnt(STICK "/phyfail", n + 1);
            sync();
            say("tvinit: no carrier, retry boot %d/12\n", n + 1);
            reboot_recovery();
        }
        say("tvinit: PHY retries exhausted, staying local\n");
    }
    reverse();
    bind_shell();
    say("tvinit: window done, idling\n");
    for (;;)
        pause();
    return 0;
}
