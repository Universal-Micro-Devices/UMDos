/* smp.c - SMP 启动（P15）
 *
 * 流程（多 CPU 时）：
 *   1. 从 MADT 取 LAPIC ID 列表（acpi_lapic_ids）；
 *   2. 把 AP 跳板拷到 0x8000，信息块写 0x8F00（cpu_index / ap_entry / stack / pml4）；
 *   3. 通过 LAPIC ICR 依次发 INIT、SIPI、SIPI；
 *   4. 轮询每个 AP 的 online 标志（AP 在 ap_entry 里置位并打印）。
 *
 * 单 CPU（模拟器/单核 QEMU）时跳过 AP 启动，只打印 BSP。
 */
#include "smp.h"
#include "percpu.h"
#include "acpi.h"
#include "io.h"
#include "mem.h"
#include "spinlock.h"
#include "smptramp.h"

#define LAPIC_BASE   0xFEE00000ull
#define LAPIC_SVR    0x0F0u
#define LAPIC_ICR_LO 0x300u
#define LAPIC_ICR_HI 0x310u
#define TRAMP_ADDR   0x8000u
#define INFO_ADDR    0x8F00u
#define MAX_AP       7

static u32 g_ap_started;

static volatile u32 *lapic(u32 off)
{
    return (volatile u32 *)(LAPIC_BASE + (u64)off);
}

static void lapic_write(u32 off, u32 v) { *lapic(off) = v; }

static void lapic_init_cpu(void)
{
    /* 开启 LAPIC（SVR bit8），清任务优先级 */
    lapic_write(LAPIC_SVR, 0x1FFu);
}

static void lapic_send(u32 apic_id, u32 icr)
{
    lapic_write(LAPIC_ICR_HI, apic_id << 24);
    lapic_write(LAPIC_ICR_LO, icr);
}

static void ap_entry(u32 cpu_index);

void ap_entry(u32 cpu_index)
{
    cpu_t *c = percpu_slot(cpu_index);
    if (c) {
        c->online = 1;
        c->apic_id = cpu_id();
        g_ap_started++;
        serial_puts("[kernel] smp: AP online idx=");
        char b[8];
        itou(cpu_index, b, 10, 0);
        serial_puts(b);
        serial_puts("\n");
    }
    for (;;)
        hlt();
}

int smp_init(void)
{
    u32 logical = cpu_logical_count();
    u32 ids[8];
    u32 n = acpi_lapic_ids(ids, 8);

    serial_puts("[kernel] smp: madt lapic=");
    {
        char b[16];
        itou(n, b, 10, 0);
        serial_puts(b);
        serial_puts(" logical=");
        itou(logical, b, 10, 0);
        serial_puts(b);
    }
    serial_puts("\n");

    if (logical <= 1 || n <= 1) {
        serial_puts("[kernel] smp: BSP only, AP bring-up skipped\n");
        return 0;
    }

    /* 1) 拷贝跳板并填信息块 */
    for (u32 i = 0; i < smp_tramp_len && i < 4096; i++)
        ((u8 *)(u64)(TRAMP_ADDR + i))[0] = smp_tramp_blob[i];
    u32 bsp = cpu_id();
    u32 slot = 1;
    for (u32 i = 0; i < n; i++) {
        if (ids[i] != bsp) continue;
        break;
    }
    for (u32 i = 0; i < n; i++) {
        if (ids[i] == bsp) continue;
        cpu_t *c = percpu_slot(slot);
        if (!c) break;
        /* 信息块 */
        *(volatile u32 *)(u64)(INFO_ADDR + 0) = slot;
        *(volatile u64 *)(u64)(INFO_ADDR + 8) = (u64)(u64)(void *)ap_entry;
        *(volatile u64 *)(u64)(INFO_ADDR + 12) = c->stack_top;
        *(volatile u64 *)(u64)(INFO_ADDR + 20) = 0x1000;   /* 内核 PML4 */
        c->apic_id = ids[i];

        lapic_init_cpu();
        lapic_send(ids[i], 0x0000C500u);          /* INIT */
        for (volatile int d = 0; d < 200000; d++)
            ;
        lapic_send(ids[i], 0x00004600u | (TRAMP_ADDR >> 12));   /* SIPI */
        for (volatile int d = 0; d < 200000; d++)
            ;
        lapic_send(ids[i], 0x00004600u | (TRAMP_ADDR >> 12));
        slot++;
    }

    /* 2) 等待 AP 报到（约 100ms 轮询） */
    for (volatile u32 spin = 0; spin < 20000000u; spin++) {
        u32 on = 0;
        for (int i = 0; i < MAX_CPUS; i++) {
            cpu_t *c = percpu_slot((u32)i);
            if (c && c->online) on++;
        }
        if (on >= n) break;
    }
    (void)g_ap_started;
    return 0;
}

u32 smp_started_aps(void) { return g_ap_started; }

int smp_selftest(void)
{
    spinlock_t lk;
    spin_init(&lk);
    volatile u32 v = 0;
    for (int i = 0; i < 1000; i++) {
        u64 f = irq_save();
        spin_lock(&lk);
        v++;
        spin_unlock(&lk);
        irq_restore(f);
    }
    int ok = (v == 1000);
    serial_puts(ok ? "[kernel] smp selftest: spinlock/bsp ok\n"
                   : "[kernel] smp selftest: FAILED\n");
    return ok ? 0 : -1;
}
