/* syscall.h - 系统调用（P5） */
#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

#define SYS_exit   0
#define SYS_write  1
#define SYS_read   2
#define SYS_open   3
#define SYS_close  4
#define SYS_getpid 5
#define SYS_sleep  6
#define SYS_brk    7
#define SYS_fork   8
#define SYS_exec   9
#define SYS_getppid 10

void syscall_init(void);            /* 安装 int 0x80 门（DPL=3） */
void syscall_dispatch(u64 *regs);   /* regs 指向保存的通用寄存器区 */
u32  syscall_count(void);

#endif
