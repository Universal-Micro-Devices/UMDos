/* gdt.h - 内核自建 GDT / TSS（P5）
 *
 * loader 的 GDT 里 0x08 是 32 位代码段，而 IDT 门用的是 0x08；
 * 在长模式下中断门指向 L=0 的段会进入兼容模式，所以 P5 在内核里
 * 重新建一份 GDT：0x08 改成 64 位代码段，并加入用户段与 TSS。
 */
#ifndef GDT_H
#define GDT_H

#include "types.h"

#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x18
#define SEL_UDATA 0x20
#define SEL_TSS   0x28

void gdt_init(void);
void tss_set_rsp0(u64 rsp0);
u64  tss_rsp0(void);
u16  tss_iomap_base(void);

#endif
