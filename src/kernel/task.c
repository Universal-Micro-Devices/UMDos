/* task.c - 内核线程与轮转调度（P5） */
#include "task.h"
#include "io.h"
#include "mem.h"
#include "gdt.h"
#include "idt.h"

extern void switch_ctx(u64 *old_rsp, u64 new_rsp);

static task_t tasks[TASK_MAX];
static int cur = -1;
static u32 switch_count;
static u32 tick_count;

task_t *task_current(void) { return cur >= 0 ? &tasks[cur] : 0; }
u32 sched_switches(void) { return switch_count; }

u32 sched_tasks(void)
{
    u32 n = 0;
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_FREE) n++;
    return n;
}

static void task_build_stack(task_t *t, void (*entry)(void))
{
    u64 *sp = (u64 *)(t->kstack_top);
    *--sp = (u64)(u64)entry;      /* switch_ctx 最后 ret 到这里 */
    *--sp = 0;                    /* rbx */
    *--sp = 0;                    /* rbp */
    *--sp = 0;                    /* r12 */
    *--sp = 0;                    /* r13 */
    *--sp = 0;                    /* r14 */
    *--sp = 0;                    /* r15 */
    t->rsp = (u64)(u64)sp;
}

int task_create(const char *name, void (*entry)(void))
{
    int slot = -1;
    for (int i = 1; i < TASK_MAX; i++)
        if (tasks[i].state == TASK_FREE) { slot = i; break; }
    if (slot < 0) return -1;

    task_t *t = &tasks[slot];
    for (u32 k = 0; k < sizeof(*t); k++) ((u8 *)t)[k] = 0;
    t->id = (u32)slot;
    t->state = TASK_READY;
    t->is_user = 0;
    strcpy_s(t->name, sizeof(t->name), name);

    u8 *stk = (u8 *)kmalloc(TASK_STACK);
    if (!stk) return -1;
    t->kstack_top = (u64)(u64)(stk + TASK_STACK);
    task_build_stack(t, entry);
    serial_puts("[kernel] task created: ");
    serial_puts(name);
    serial_puts("\n");
    return slot;
}

void sched_init(void)
{
    for (int i = 0; i < TASK_MAX; i++) {
        tasks[i].state = TASK_FREE;
        tasks[i].id = (u32)i;
    }
    /* 0 号是当前主线程（就是现在这条执行流） */
    cur = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].kstack_top = 0x90000;
    tasks[0].rsp = 0;
    strcpy_s(tasks[0].name, sizeof(tasks[0].name), "main");
    tss_set_rsp0(tasks[0].kstack_top);
    serial_puts("[kernel] sched: main task registered\n");
}

static int next_ready(int from)
{
    for (int k = 1; k <= TASK_MAX; k++) {
        int i = (from + k) % TASK_MAX;
        if (i == from) continue;
        if (tasks[i].state == TASK_READY) return i;
    }
    return -1;
}

void sched_yield(void)
{
    if (cur < 0) return;
    u64 flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    cli();

    int prev = cur;
    int next = next_ready(prev);
    if (next >= 0) {
        serial_puts("[sched] ");
        serial_puts(tasks[prev].name);
        serial_puts(" -> ");
        serial_puts(tasks[next].name);
        serial_puts("\n");
        if (tasks[prev].state == TASK_RUNNING) tasks[prev].state = TASK_READY;
        tasks[next].state = TASK_RUNNING;
        tasks[prev].switches++;
        cur = next;
        switch_count++;
        tss_set_rsp0(tasks[next].kstack_top);
        switch_ctx(&tasks[prev].rsp, tasks[next].rsp);
    }
    __asm__ volatile("push %0; popfq" : : "r"(flags));
}

void sched_tick(void)
{
    tick_count++;
    if ((tick_count % TASK_QUANTUM) == 0)
        sched_yield();
}

void task_exit(int code)
{
    task_t *t = task_current();
    if (t) {
        t->exit_code = code;
        t->state = TASK_DEAD;
        serial_puts("[kernel] task exit: ");
        serial_puts(t->name);
        serial_puts("\n");
    }
    for (;;) {
        sched_yield();
        /* 没有可运行任务时也绝不能再回到死任务 */
        __asm__ volatile("hlt");
    }
}
