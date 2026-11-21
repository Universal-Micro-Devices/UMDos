/* percpu.c - 每 CPU 数据结构（P15） */
#include "percpu.h"
#include "io.h"
#include "mem.h"

static cpu_t g_cpus[MAX_CPUS];
static int g_ncpu;
static u32 g_bsp_apic;

u32 cpu_id(void)
{
    u32 eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return (ebx >> 24) & 0xFF;
}

u32 cpu_logical_count(void)
{
    u32 eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    u32 n = (ebx >> 16) & 0xFF;
    return n ? n : 1;
}

int percpu_init(void)
{
    for (int i = 0; i < MAX_CPUS; i++) {
        g_cpus[i].apic_id = 0xFFFFFFFFu;
        g_cpus[i].index = (u32)i;
        g_cpus[i].stack_top = 0;
        g_cpus[i].online = 0;
        g_cpus[i].ticks = 0;
    }
    g_bsp_apic = cpu_id();
    g_cpus[0].apic_id = g_bsp_apic;
    g_cpus[0].online = 1;
    u8 *stk = (u8 *)kmalloc(8192);
    g_cpus[0].stack_top = stk ? (u64)(u64)(stk + 8192) : 0x90000;
    g_ncpu = 1;
    serial_puts("[kernel] percpu: bsp apic_id=");
    char b[16];
    itou(g_bsp_apic, b, 10, 0);
    serial_puts(b);
    serial_puts(" logical=");
    itou(cpu_logical_count(), b, 10, 0);
    serial_puts(b);
    serial_puts("\n");
    return 0;
}

cpu_t *cpu_self(void)
{
    u32 id = cpu_id();
    for (int i = 0; i < g_ncpu; i++)
        if (g_cpus[i].online && g_cpus[i].apic_id == id) return &g_cpus[i];
    return &g_cpus[0];
}

cpu_t *cpu_by_apic(u32 apic_id)
{
    for (int i = 0; i < MAX_CPUS; i++)
        if (g_cpus[i].apic_id == apic_id) return &g_cpus[i];
    return 0;
}

cpu_t *percpu_slot(u32 index)
{
    return index < MAX_CPUS ? &g_cpus[index] : 0;
}

int percpu_count(void) { return g_ncpu; }

int percpu_selftest(void)
{
    u32 n = cpu_logical_count();
    cpu_t *me = cpu_self();
    int ok = (me != 0) && (me->online == 1) && (me->stack_top != 0) && (n >= 1);
    serial_puts(ok ? "[kernel] percpu selftest: bsp/stack/cpuid ok\n"
                   : "[kernel] percpu selftest: FAILED\n");
    return ok ? 0 : -1;
}
