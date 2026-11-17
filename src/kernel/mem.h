/* mem.h - 物理内存、页帧分配、内核堆与页表辅助（P4） */
#ifndef MEM_H
#define MEM_H

#include "types.h"
#include "bootinfo.h"

#define USER_BASE 0x0000008000000000ull     /* 用户地址空间预留起点（恒等映射之外） */

void mem_init(const bootinfo_t *bi);
u32  mem_total_kb(void);
u32  mem_usable_kb(void);
u32  mem_frames_total(void);
u32  mem_frames_free(void);
u64  mem_heap_base(void);
u64  mem_heap_size(void);

void *pmm_alloc(void);              /* 分配 4KB 物理页，返回物理地址并清零 */
void  pmm_free(void *p);

void *kmalloc(u64 n);
void  kfree(void *p);

int   vmm_map_page(u64 va, u64 pa, u64 flags);
int   vmm_unmap_page(u64 va);
int   vmm_is_mapped(u64 va);
u64   vmm_pte_flags(u64 va);
int   mem_selftest(void);

#endif
