/* task.h - 内核线程、轮转调度与用户进程（P5） */
#ifndef TASK_H
#define TASK_H

#include "types.h"

#define TASK_MAX      8
#define TASK_STACK    8192
#define TASK_QUANTUM  5          /* tick 数 */

enum { TASK_FREE = 0, TASK_READY, TASK_RUNNING, TASK_DEAD };

typedef struct {
    u64 rsp;                     /* 保存的内核栈指针 */
    u64 kstack_top;              /* 内核栈顶（TSS.rsp0 用） */
    u32 id;
    char name[12];
    int state;
    u64 switches;
    int exit_code;
    int is_user;
} task_t;

void  sched_init(void);
void  sched_yield(void);
void  sched_tick(void);          /* IRQ0 调用 */
task_t *task_current(void);
int   task_create(const char *name, void (*entry)(void));
void  task_exit(int code) __attribute__((noreturn));
u32   sched_switches(void);
u32   sched_tasks(void);

/* 用户态（P6） */
int   user_run(const void *elf, u32 len, u32 *entry_rip, u32 *user_rsp);

#endif
