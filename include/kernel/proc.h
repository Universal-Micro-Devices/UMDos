/* proc.h - 进程抽象与 /proc（P10） */
#ifndef PROC_H
#define PROC_H

#include "types.h"
#include "vfs.h"

#define PROC_MAX 8

#define PROC_FREE    0
#define PROC_READY   1
#define PROC_RUNNING 2
#define PROC_EXITED  3

typedef struct {
    int  used;
    u32  pid;
    u32  ppid;
    char name[16];
    int  state;
    int  exit_code;
    vfs_fdset_t *fds;
} proc_t;

void    proc_init(void);
proc_t *proc_current(void);
void    proc_set_current(proc_t *p);
proc_t *proc_find(u32 pid);
proc_t *proc_at(int index);
int     proc_count(void);
int     proc_fork(void);            /* 返回子进程 pid，失败 -1 */
int     proc_spawn(const char *name);
u32     proc_format_tasks(char *buf, u32 cap);
u32     proc_format_meminfo(char *buf, u32 cap);
u32     proc_format_version(char *buf, u32 cap);
u32     proc_format_mounts(char *buf, u32 cap);
int     proc_selftest(void);

#endif
