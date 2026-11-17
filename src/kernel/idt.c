/* idt.c - 64 位 IDT、异常分发与 panic 屏幕（P3） */
#include "idt.h"
#include "gfx.h"
#include "io.h"
#include "irq.h"
#include "syscall.h"
#include "bootinfo.h"

typedef struct {
    u16 off_lo;
    u16 sel;
    u8  ist;
    u8  type;
    u16 off_mid;
    u32 off_hi;
    u32 zero;
} __attribute__((packed)) idt_entry;

typedef struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) idt_ptr;

static idt_entry idt[256] __attribute__((aligned(16)));
static idt_ptr idtp;
static u32 g_panics;

extern u64 isr_stub_table[48];       /* isr.asm */
extern u8 isr_0x80[];               /* isr.asm：系统调用门 */

static void idt_set(int v, u64 handler, u8 type)
{
    idt[v].off_lo = (u16)(handler & 0xFFFF);
    idt[v].sel = 0x08;
    idt[v].ist = 0;
    idt[v].type = type;
    idt[v].off_mid = (u16)((handler >> 16) & 0xFFFF);
    idt[v].off_hi = (u32)(handler >> 32);
    idt[v].zero = 0;
}

void idt_init(void)
{
    for (int i = 0; i < 256; i++)
        idt_set(i, 0, 0);
    for (int i = 0; i < 48; i++)
        idt_set(i, isr_stub_table[i], 0x8E);     /* present, DPL=0, 中断门 */
    idt_set(0x80, (u64)(u64)isr_0x80, 0xEE);     /* int 0x80: DPL=3, 中断门 */
    idtp.limit = (u16)(sizeof(idt_entry) * 256 - 1);
    idtp.base = (u64)(u64)idt;
    __asm__ volatile("lidt %0" : : "m"(idtp));
    /* 调试约定：把 IDT 基址写到 bootinfo +108，测试工具据此模拟中断投递
     *（Unicorn 不跟踪 IDTR，无法从 CR/IDTR 反查） */
    *(volatile u32 *)(BOOTINFO_ADDR + 108) = (u32)(u64)idt;
    serial_puts("[kernel] idt ready (48 vectors)\n");
}

u32 panic_count(void) { return g_panics; }

static const char *exc_name(u64 v)
{
    static const char *names[32] = {
        "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
        "#DF", "CSEG", "#TS", "#NP", "#SS", "#GP", "#PF", "RSVD",
        "#MF", "#AC", "#MC", "#XM", "#VE", "#CP", "RSV22", "RSV23",
        "RSV24", "RSV25", "RSV26", "RSV27", "RSV28", "RSV29", "RSV30", "RSV31",
    };
    return v < 32 ? names[v] : "?";
}

static void panic_screen(u64 vec, u64 err, u64 rip, u64 cs, u64 rflags)
{
    u64 cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    g_panics++;
    cli();

    /* 红色 panic 屏 */
    u32 w = gfx_width(), h = gfx_height();
    gfx_clear(0x00150000);
    gfx_rect(0, 0, (int)w, 60, 0x00A00000u);
    gfx_text_center((int)w / 2, 14, "UMDos PANIC", C_WHITE, 3);

    char buf[96];
    char *p = buf;
    const char *a = "exception ";
    while (*a) *p++ = *a++;
    p = itou(vec, p, 10, 0);
    *p++ = ' ';
    const char *nm = exc_name(vec);
    while (*nm) *p++ = *nm++;
    *p = 0;
    gfx_text(40, 100, buf, C_WHITE, 2);

    p = buf;
    const char *b = "error=";
    while (*b) *p++ = *b++;
    p = itou(err, p, 16, 1);
    *p = 0;
    gfx_text(40, 140, buf, 0x00FFC0C0u, 2);

    p = buf;
    const char *c = "cs=0x";
    while (*c) *p++ = *c++;
    p = itou(cs, p, 16, 1);
    *p = 0;
    gfx_text(40, 180, buf, 0x00FFC0C0u, 2);

    p = buf;
    const char *d = "rip=0x";
    while (*d) *p++ = *d++;
    p = itou(rip, p, 16, 0);
    *p = 0;
    gfx_text(40, 220, buf, 0x00FFC0C0u, 2);

    p = buf;
    const char *e = "cr2=0x";
    while (*e) *p++ = *e++;
    p = itou(cr2, p, 16, 0);
    *p = 0;
    gfx_text(40, 260, buf, 0x00FFC0C0u, 2);

    p = buf;
    const char *f = "rflags=0x";
    while (*f) *p++ = *f++;
    p = itou(rflags, p, 16, 0);
    *p = 0;
    gfx_text(40, 300, buf, 0x00FFC0C0u, 2);

    gfx_text(40, 380, "system halted (no reboot). reset the machine manually.", C_WHITE, 2);
    gfx_text(40, 420, "serial log has the same information.", 0x00C0C0C0u, 1);

    /* 串口日志 */
    serial_puts("[kernel] PANIC exc=");
    serial_puts(exc_name(vec));
    serial_puts(" vec=");
    char nb[24];
    itou(vec, nb, 10, 0);
    serial_puts(nb);
    serial_puts(" err=0x");
    itou(err, nb, 16, 0);
    serial_puts(nb);
    serial_puts(" rip=0x");
    itou(rip, nb, 16, 0);
    serial_puts(nb);
    serial_puts(" cr2=0x");
    itou(cr2, nb, 16, 0);
    serial_puts(nb);
    serial_puts("\n");

    for (;;)
        hlt();
}

void isr_dispatch(u64 vec, u64 err, u64 rip, u64 cs, u64 rflags, u64 *regs)
{
    if (vec == 0x80) {
        syscall_dispatch(regs);
        return;
    }
    if (vec < 32) {
        static int shown;
        if (shown < 8) {
            shown++;
            serial_puts("[kernel] exc vec=");
            char b[16];
            itou(vec, b, 10, 0);
            serial_puts(b);
            serial_puts(" rip=0x");
            itou(rip, b, 16, 0);
            serial_puts(b);
            serial_puts(" cs=0x");
            itou(cs, b, 16, 0);
            serial_puts(b);
            serial_puts("\n");
        }
        panic_screen(vec, err, rip, cs, rflags);
        return;
    }
    irq_dispatch((u32)(vec - 32));
}
