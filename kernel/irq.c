/* irq.c - PIC 重映射、PIT、IRQ0/1/12/14 处理（P3）
 *
 * 说明：本版本的内核事件循环仍然保留轮询回退（模拟器没有 PIT/PIC 硬件模型），
 * 但中断路径是真实可用的：键盘/鼠标 IRQ 把数据推进环形缓冲，PIT 提供 tick。
 */
#include "irq.h"
#include "io.h"
#include "input.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

static u32 g_ticks;
static u32 g_irq_count[16];
static u32 g_ata_irq;

void pic_remap(void)
{
    outb(PIC1_CMD, 0x11);       /* ICW1: 边沿触发 + 需要 ICW4 */
    outb(PIC2_CMD, 0x11);
    outb(PIC1_DATA, 0x20);      /* ICW2: 主片向量基址 0x20 */
    outb(PIC2_DATA, 0x28);      /* ICW2: 从片向量基址 0x28 */
    outb(PIC1_DATA, 0x04);      /* ICW3: 从片接在 IRQ2 */
    outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, 0x01);      /* ICW4: 8086 模式 */
    outb(PIC2_DATA, 0x01);

    /* 只打开：IRQ0(PIT)、IRQ1(键盘)、IRQ2(级联)、IRQ12(鼠标)、IRQ14(ATA) */
    u8 m1 = (u8)~(1 << 0 | 1 << 1 | 1 << 2 | 1 << 4);
    u8 m2 = (u8)~(1 << 4 | 1 << 6);
    outb(PIC1_DATA, m1);
    outb(PIC2_DATA, m2);
}

void pit_init_hz(u32 hz)
{
    if (hz == 0) hz = 100;
    u32 div = 1193182u / hz;
    if (div > 0xFFFF) div = 0xFFFF;
    outb(0x43, 0x36);                       /* 通道0, lo/hi, 方波 */
    outb(0x40, (u8)(div & 0xFF));
    outb(0x40, (u8)((div >> 8) & 0xFF));
}

void irq_init(void)
{
    pic_remap();
    pit_init_hz(100);
    serial_puts("[kernel] irq: pic remapped 0x20/0x28, pit 100Hz\n");
}

u32 pit_ticks(void) { return g_ticks; }
u32 irq_counts(u32 irq) { return irq < 16 ? g_irq_count[irq] : 0; }
u32 ata_irq_count(void) { return g_ata_irq; }

void irq_dispatch(u32 irq)
{
    if (irq < 16) g_irq_count[irq]++;

    switch (irq) {
    case 0:                                  /* PIT */
        g_ticks++;
        break;
    case 1: {                                /* 键盘 */
        u8 sc = inb(0x60);
        if (g_irq_count[1] <= 8) {           /* 只记录前 8 个，便于定位又不刷屏 */
            serial_puts("[kernel] irq1 sc=0x");
            char b[4];
            itou(sc, b, 16, 2);
            serial_puts(b);
            serial_puts("\n");
        }
        kbd_irq_push(sc);
        break;
    }
    case 12: {                               /* PS/2 鼠标 */
        u8 b = inb(0x60);
        mouse_irq_push(b);
        break;
    }
    case 14:                                 /* ATA */
        (void)inb(0x1F7);
        g_ata_irq++;
        break;
    default:
        break;
    }

    if (irq >= 8)
        outb(PIC2_CMD, 0x20);                /* 从片 EOI */
    outb(PIC1_CMD, 0x20);                    /* 主片 EOI */
}

/* 自检：不依赖真实硬件，直接走处理逻辑 */
int irq_selftest(void)
{
    u32 t0 = g_ticks;
    irq_dispatch(0);
    int tick_ok = (g_ticks == t0 + 1);

    /* 键盘：推一个 'A' 的扫描码，确认能从输入层取到事件 */
    input_selftest_expected();               /* 清空输入层缓冲 */
    kbd_irq_push(0x1E);                      /* 扫描码集 1 的 'A' */
    input_event ev;
    int kbd_ok = 0;
    for (int i = 0; i < 8; i++) {
        if (input_poll(&ev)) {
            if (ev.type == EV_KEY && ev.key == 'a')
                kbd_ok = 1;
            break;
        }
    }
    input_selftest_reset();
    return (tick_ok && kbd_ok) ? 0 : -1;
}
