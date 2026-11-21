/* prog.c - 用户态演示程序（P6） */
#include "ulib.h"

static int fmt_ulong(char *buf, u64 v)
{
    char tmp[24];
    int n = 0, k = 0;
    if (!v) tmp[n++] = '0';
    while (v) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n) buf[k++] = tmp[--n];
    return k;
}

int main(void)
{
    const char *m1 = "ring3: hello from user mode\n";
    write(1, m1, strlen(m1));

    char buf[64];
    int n = 0;
    const char *pre = "ring3: getpid()=";
    while (*pre) buf[n++] = *pre++;
    n += fmt_ulong(buf + n, (u64)getpid());
    buf[n++] = '\n';
    write(1, buf, (u64)n);

    char *p = (char *)malloc(32);
    if (p) {
        const char *ok = "ring3: malloc ok\n";
        u64 l = strlen(ok);
        for (u64 i = 0; i < l; i++) p[i] = ok[i];
        write(1, p, l);
        free(p);
    }

    const char *bye = "ring3: exiting\n";
    write(1, bye, strlen(bye));
    return 0;
}
