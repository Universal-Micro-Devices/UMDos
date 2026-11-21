/* percpu.h - 每 CPU 数据（P15） */
#ifndef PERCPU_H
#define PERCPU_H

#include "types.h"

#define MAX_CPUS 8

typedef struct {
    u32 apic_id;
    u32 index;
    u64 stack_top;
    volatile u32 online;
    volatile u64 ticks;
} cpu_t;

int    percpu_init(void);
cpu_t *cpu_self(void);
cpu_t *cpu_by_apic(u32 apic_id);
cpu_t *percpu_slot(u32 index);
int    percpu_count(void);
u32    cpu_id(void);                 /* 当前 CPU 的 APIC ID（CPUID） */
u32    cpu_logical_count(void);      /* CPUID 报告的逻辑处理器数 */
int    percpu_selftest(void);

#endif
