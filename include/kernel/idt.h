/* idt.h - 中断描述符表与异常处理（P3） */
#ifndef IDT_H
#define IDT_H

#include "types.h"

void idt_init(void);
void isr_dispatch(u64 vec, u64 err, u64 rip, u64 cs, u64 rflags, u64 *regs);
u32  panic_count(void);

#endif
