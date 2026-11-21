/* libc.c - 用户态最小 libc（P6）
 * 只实现规格要求的最小集合：_start / write / exit / malloc / free / open / read / close
 * 另外提供 getpid / sleep / brk 方便演示。
 */
#include "ulib.h"

long syscall3(long num, long a, long b, long c)
{
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return ret;
}

long write(int fd, const void *buf, u64 len)
{
    return syscall3(SYS_write, fd, (long)buf, (long)len);
}

void exit(int code)
{
    syscall3(SYS_exit, code, 0, 0);
    for (;;) {
    }
}

int getpid(void) { return (int)syscall3(SYS_getpid, 0, 0, 0); }
int sleep_ticks(int n) { return (int)syscall3(SYS_sleep, n, 0, 0); }
int brk(void *addr) { return (int)syscall3(SYS_brk, (long)addr, 0, 0); }

int open(const char *path, int flags) { return (int)syscall3(SYS_open, (long)path, flags, 0); }
int read(int fd, void *buf, u64 len) { return (int)syscall3(SYS_read, fd, (long)buf, (long)len); }
int close(int fd) { return (int)syscall3(SYS_close, fd, 0, 0); }

/* 简单竞技场分配器：演示够用，free 暂不回收（README 已注明） */
static char arena[16384];
static u64 arena_used;

void *malloc(u64 n)
{
    n = (n + 15) & ~15ul;
    if (arena_used + n > sizeof(arena)) return 0;
    void *p = &arena[arena_used];
    arena_used += n;
    return p;
}

void free(void *p) { (void)p; }

void *memset(void *d, int c, u64 n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

u64 strlen(const char *s)
{
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

/* ELF 入口 */
int main(void);

void _start(void) __attribute__((section(".text._start")));
void _start(void)
{
    exit(main());
}
