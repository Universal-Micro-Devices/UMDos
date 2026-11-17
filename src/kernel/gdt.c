/* gdt.c - 内核 GDT 与 TSS（P5）
 *
 * 为什么要在内核里重建 GDT：
 *   loader 的 GDT 中 0x08 是 32 位代码段，而 IDT 门使用选择子 0x08。
 *   在长模式下中断门指向 L=0 的段会进入兼容模式（32 位解码），
 *   64 位入口桩会跑错。这里把 0x08 改成 64 位代码段，并加入用户段与 TSS。
 */
#include "gdt.h"
#include "io.h"

typedef struct {
    u32 rsv0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 rsv1;
    u64 ist[7];
    u64 rsv2;
    u16 rsv3;
    u16 iomap_base;
} __attribute__((packed)) tss64_t;

static tss64_t tss;

/* GDT：null / kcode / kdata / ucode / udata / TSS(lo,hi) */
static struct {
    u64 null_;
    u64 kcode;
    u64 kdata;
    u64 ucode;
    u64 udata;
    u64 tss_lo;
    u64 tss_hi;
} __attribute__((packed)) gdtree;

static struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) gdtr;

void tss_set_rsp0(u64 rsp0) { tss.rsp0 = rsp0; }
u64 tss_rsp0(void) { return tss.rsp0; }
u16 tss_iomap_base(void) { return tss.iomap_base; }

static void reload_segments(void)
{
    __asm__ volatile(
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %0\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov %1, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        :
        : "i"(SEL_KCODE), "i"(SEL_KDATA)
        : "rax", "memory");
}

void gdt_init(void)
{
    gdtree.null_ = 0;
    gdtree.kcode = 0x00AF9A000000FFFFull;   /* 0x08: 64 位代码 DPL0 */
    gdtree.kdata = 0x00CF92000000FFFFull;   /* 0x10: 数据 DPL0 */
    gdtree.ucode = 0x00AFFA000000FFFFull;   /* 0x18: 64 位代码 DPL3 */
    gdtree.udata = 0x00CFF2000000FFFFull;   /* 0x20: 数据 DPL3 */

    for (u32 i = 0; i < sizeof(tss); i++) ((u8 *)&tss)[i] = 0;
    tss.iomap_base = (u16)sizeof(tss);
    tss.rsp0 = 0x90000;

    u64 base = (u64)(u64)&tss;
    u64 lo = (u64)(sizeof(tss) - 1) & 0xFFFFull;
    lo |= (base & 0xFFFFFFull) << 16;
    lo |= (u64)0x89 << 40;                  /* present, type=0x9 (64 位 TSS 可用) */
    lo |= ((base >> 24) & 0xFFull) << 56;
    gdtree.tss_lo = lo;
    gdtree.tss_hi = base >> 32;

    gdtr.limit = (u16)(sizeof(gdtree) - 1);
    gdtr.base = (u64)(u64)&gdtree;

    __asm__ volatile("lgdt %0" : : "m"(gdtr));
    reload_segments();
    __asm__ volatile("ltr %%ax" : : "a"(SEL_TSS));
    serial_puts("[kernel] gdt: rebuilt, tss loaded (user 0x18/0x20)\n");
}
