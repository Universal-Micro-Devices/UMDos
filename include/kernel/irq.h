/* irq.h - PIC/PIT 与硬件中断分发（P3） */
#ifndef IRQ_H
#define IRQ_H

#include "types.h"

void irq_init(void);
void pit_init_hz(u32 hz);
void pic_remap(void);            /* 重映射 PIC + PIT 100Hz + 打开需要的 IRQ */
void irq_dispatch(u32 irq);
u32  pit_ticks(void);
u32  irq_counts(u32 irq);
u32  ata_irq_count(void);
int  irq_selftest(void);        /* 直接调用处理逻辑做自检，返回 0 表示通过 */

#endif
