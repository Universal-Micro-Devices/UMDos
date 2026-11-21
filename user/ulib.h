/* ulib.h - 最小 libc 头（用户态） */
#ifndef ULIB_H
#define ULIB_H

typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;

#define SYS_exit 0
#define SYS_write 1
#define SYS_read 2
#define SYS_open 3
#define SYS_close 4
#define SYS_getpid 5
#define SYS_sleep 6
#define SYS_brk 7

long syscall3(long num, long a, long b, long c);
long write(int fd, const void *buf, u64 len);
void exit(int code) __attribute__((noreturn));
int  getpid(void);
int  sleep_ticks(int n);
int  brk(void *addr);
int  open(const char *path, int flags);
int  read(int fd, void *buf, u64 len);
int  close(int fd);

void *malloc(u64 n);
void  free(void *p);
void *memset(void *d, int c, u64 n);
u64   strlen(const char *s);

#endif
