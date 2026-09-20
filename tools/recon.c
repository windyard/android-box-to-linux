#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

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

static void listdir(const char *d)
{
    DIR *dp = opendir(d);
    up("== %s %s", d, dp ? "OK" : "FAIL");
    if (!dp) return;
    struct dirent *e;
    int i = 0;
    while ((e = readdir(dp)) && i++ < 20) up("  %s", e->d_name);
    closedir(dp);
}

int main(int argc, char **argv)
{
    if (argc > 2) SFD = atoi(argv[2]);
    up("BINARY-RAN argv0=%s", argc > 0 ? argv[0] : "?");
    listdir("/tmp");
    listdir("/sbin");
    listdir("/system/bin");
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd >= 0) {
        char b[256]; int n;
        while ((n = read(fd, b, sizeof(b)-1)) > 0) { b[n]=0; char *p=strchr(b,'\n'); if(p)*p=0; up("m %s", b); }
        close(fd);
    }
    up("done-marker-reached");
    write(SFD, "done\n", 5);
    return 0;
}
