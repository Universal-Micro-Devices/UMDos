/* mem.c - P4：E820 解析、物理页帧分配器、内核堆（kmalloc/kfree）、页表映射
 *
 * 现状：head 已经建立 0–4GB 的 2MB 大页恒等映射，所以任何物理页都可以直接访问；
 * 本文件在这之上提供“页帧分配 + 堆 + 用户窗口映射”。
 * 用户地址空间从 USER_BASE 开始，用 4KB 页单独建表（PML4 索引 256），
 * 与低端恒等映射互不影响，为后面的用户态（P5/P6）预留。
 */
#include "mem.h"
#include "io.h"
#include "spinlock.h"

#define FRAME_SIZE   4096u
#define MAX_FRAMES   (128u * 1024u * 1024u / FRAME_SIZE)   /* 最多管理 128MB */
#define HEAP_FRAMES  256u                                  /* 1MB 内核堆 */
#define PML4_BASE    0x1000u

typedef struct {
    u64 base;
    u64 len;
    u32 type;
    u32 attr;
} __attribute__((packed)) e820_entry;

static const bootinfo_t *BI;
static u8  frame_bm[MAX_FRAMES / 8];
static u32 frame_total;
static u32 frame_free;
static u64 heap_base;
static u64 heap_size;

static int bit_test(u32 f) { return (frame_bm[f >> 3] >> (f & 7)) & 1; }
static void bit_set(u32 f, int v)
{
    if (v) frame_bm[f >> 3] |= (u8)(1u << (f & 7));
    else frame_bm[f >> 3] &= (u8)~(1u << (f & 7));
}

static void reserve(u64 start, u64 end)
{
    if (end <= start) return;
    u32 a = (u32)(start / FRAME_SIZE);
    u32 b = (u32)((end + FRAME_SIZE - 1) / FRAME_SIZE);
    if (a >= frame_total) return;
    if (b > frame_total) b = frame_total;
    for (u32 f = a; f < b; f++) bit_set(f, 1);
}

static void mark_free_region(u64 start, u64 end)
{
    u32 a = (u32)((start + FRAME_SIZE - 1) / FRAME_SIZE);
    u32 b = (u32)(end / FRAME_SIZE);
    if (b > frame_total) b = frame_total;
    for (u32 f = a; f < b; f++) bit_set(f, 0);
}

void mem_init(const bootinfo_t *bi)
{
    BI = bi;
    frame_total = 0;
    frame_free = 0;
    heap_base = 0;
    heap_size = 0;

    for (u32 i = 0; i < sizeof(frame_bm); i++) frame_bm[i] = 0xFF;   /* 先全部占用 */

    if (bi->memmap_addr && bi->memmap_count) {
        const e820_entry *e = (const e820_entry *)(u64)bi->memmap_addr;
        for (u32 i = 0; i < bi->memmap_count && i < 32; i++) {
            u64 end = e[i].base + e[i].len;
            if (e[i].type != 1) continue;
            if (e[i].base >= 128ull * 1024 * 1024) continue;
            if (end > 128ull * 1024 * 1024) end = 128ull * 1024 * 1024;
            if (end <= e[i].base) continue;
            if (!frame_total) frame_total = (u32)(128ull * 1024 * 1024 / FRAME_SIZE);
            mark_free_region(e[i].base, end);
        }
    }
    if (!frame_total) {
        /* 没有 E820 时保守假设 0..16MB 可用 */
        frame_total = (u32)(16ull * 1024 * 1024 / FRAME_SIZE);
        for (u32 f = 0; f < frame_total; f++) bit_set(f, 0);
    }

    /* 预留：低 1MB、页表、bootinfo、16 位阶段映像、内核镜像+bss、UMDFS、栈 */
    reserve(0, 0x100000);
    reserve(0x100000, 0x30000 + 0x40000);      /* 内核窗口 0x10000..0x27FFF + FS 0x40000..0x5FFFF */
    reserve(0x5F000, 0x90000);                 /* FS 尾部 + 栈顶 */

    /* 从空页里划出一段连续物理内存作为内核堆 */
    u32 need = HEAP_FRAMES;
    u32 run = 0, start = 0;
    for (u32 f = 256; f < frame_total && heap_base == 0; f++) {
        if (!bit_test(f)) {
            if (!run) start = f;
            if (++run == need) {
                for (u32 k = start; k < start + need; k++) bit_set(k, 1);
                heap_base = (u64)start * FRAME_SIZE;
                heap_size = (u64)need * FRAME_SIZE;
            }
        } else {
            run = 0;
        }
    }

    frame_free = 0;
    for (u32 f = 0; f < frame_total; f++)
        if (!bit_test(f)) frame_free++;

    serial_puts("[kernel] mem: e820 entries=");
    char nb[24];
    itou(bi->memmap_count, nb, 10, 0);
    serial_puts(nb);
    serial_puts(" frames=");
    itou(frame_total, nb, 10, 0);
    serial_puts(nb);
    serial_puts(" free=");
    itou(frame_free, nb, 10, 0);
    serial_puts(nb);
    serial_puts(" heap=0x");
    itou(heap_base, nb, 16, 0);
    serial_puts(nb);
    serial_puts(" (");
    itou(heap_size / 1024, nb, 10, 0);
    serial_puts(nb);
    serial_puts(" KiB)\n");
}

u32 mem_frames_total(void) { return frame_total; }
u32 mem_frames_free(void) { return frame_free; }
u64 mem_heap_base(void) { return heap_base; }
u64 mem_heap_size(void) { return heap_size; }

u32 mem_total_kb(void)
{
    u64 best = 0;
    if (BI && BI->memmap_addr) {
        const e820_entry *e = (const e820_entry *)(u64)BI->memmap_addr;
        for (u32 i = 0; i < BI->memmap_count && i < 32; i++)
            if (e[i].type == 1 && e[i].base + e[i].len > best)
                best = e[i].base + e[i].len;
    }
    return (u32)(best / 1024);
}

u32 mem_usable_kb(void) { return frame_total * FRAME_SIZE / 1024; }

void *pmm_alloc(void)
{
    for (u32 f = 0; f < frame_total; f++) {
        if (!bit_test(f)) {
            bit_set(f, 1);
            frame_free--;
            u64 pa = (u64)f * FRAME_SIZE;
            u8 *p = (u8 *)pa;
            for (u32 i = 0; i < FRAME_SIZE; i++) p[i] = 0;
            return (void *)pa;
        }
    }
    return 0;
}

void pmm_free(void *p)
{
    u64 pa = (u64)p;
    if (pa >= (u64)frame_total * FRAME_SIZE) return;
    u32 f = (u32)(pa / FRAME_SIZE);
    if (bit_test(f)) {
        bit_set(f, 0);
        frame_free++;
    }
}

/* ---------------- 内核堆：首次适配 + 空闲链表 ---------------- */
typedef struct blk {
    u64 size;
    struct blk *next;
} blk_t;

static blk_t *free_list;
static u64 heap_used;

static spinlock_t heap_lock;

static void *kmalloc_raw(u64 n)
{
    if (!heap_base || !n) return 0;
    n = (n + 15) & ~15ull;
    blk_t **pp = &free_list;
    for (blk_t *b = free_list; b; pp = &b->next, b = b->next) {
        if (b->size >= n) {
            *pp = b->next;
            heap_used += b->size;
            return (void *)((u8 *)b + sizeof(blk_t));
        }
    }
    /* 还没切过的堆：直接向后分配 */
    static u64 top;
    if (!top) top = heap_base;
    if (top + sizeof(blk_t) + n > heap_base + heap_size)
        return 0;
    blk_t *b = (blk_t *)top;
    b->size = n;
    b->next = 0;
    top += sizeof(blk_t) + n;
    heap_used += n;
    return (void *)((u8 *)b + sizeof(blk_t));
}

static void kfree_raw(void *p)
{
    if (!p) return;
    blk_t *b = (blk_t *)((u8 *)p - sizeof(blk_t));
    b->next = free_list;
    free_list = b;
    if (heap_used >= b->size) heap_used -= b->size;
}

void *kmalloc(u64 n)
{
    u64 f = irq_save();
    spin_lock(&heap_lock);
    void *p = kmalloc_raw(n);
    spin_unlock(&heap_lock);
    irq_restore(f);
    return p;
}

void kfree(void *p)
{
    u64 f = irq_save();
    spin_lock(&heap_lock);
    kfree_raw(p);
    spin_unlock(&heap_lock);
    irq_restore(f);
}

/* ---------------- 用户窗口页表（PML4 索引 256） ---------------- */
static u64 *table_at(u64 pa) { return (u64 *)pa; }

int vmm_map_page(u64 va, u64 pa, u64 flags)
{
    u64 *pml4 = table_at(PML4_BASE);
    u32 i4 = (u32)((va >> 39) & 0x1FF);
    u32 i3 = (u32)((va >> 30) & 0x1FF);
    u32 i2 = (u32)((va >> 21) & 0x1FF);
    u32 i1 = (u32)((va >> 12) & 0x1FF);

    if (!(pml4[i4] & 1)) {
        void *t = pmm_alloc();
        if (!t) return -1;
        pml4[i4] = (u64)t | 3;
    }
    u64 *pdpt = table_at(pml4[i4] & ~0xFFFull);
    if (!(pdpt[i3] & 1)) {
        void *t = pmm_alloc();
        if (!t) return -1;
        pdpt[i3] = (u64)t | 3;
    }
    u64 *pd = table_at(pdpt[i3] & ~0xFFFull);
    if (!(pd[i2] & 1)) {
        void *t = pmm_alloc();
        if (!t) return -1;
        pd[i2] = (u64)t | 3;
    } else if (pd[i2] & 0x80) {
        return -2;                       /* 已是 2MB 大页，本实现不拆分 */
    }
    u64 *pt = table_at(pd[i2] & ~0xFFFull);
    pt[i1] = (pa & ~0xFFFull) | (flags & 0xFFF) | 1;
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    return 0;
}

int vmm_unmap_page(u64 va)
{
    u64 *pml4 = table_at(PML4_BASE);
    u32 i4 = (u32)((va >> 39) & 0x1FF);
    u32 i3 = (u32)((va >> 30) & 0x1FF);
    u32 i2 = (u32)((va >> 21) & 0x1FF);
    u32 i1 = (u32)((va >> 12) & 0x1FF);
    if (!(pml4[i4] & 1)) return -1;
    u64 *pdpt = table_at(pml4[i4] & ~0xFFFull);
    if (!(pdpt[i3] & 1)) return -1;
    u64 *pd = table_at(pdpt[i3] & ~0xFFFull);
    if (!(pd[i2] & 1) || (pd[i2] & 0x80)) return -1;
    u64 *pt = table_at(pd[i2] & ~0xFFFull);
    pt[i1] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    return 0;
}

u64 vmm_pte_flags(u64 va)
{
    u64 *pml4 = table_at(PML4_BASE);
    u32 i4 = (u32)((va >> 39) & 0x1FF);
    if (!(pml4[i4] & 1)) return 0;
    u64 *pdpt = table_at(pml4[i4] & ~0xFFFull);
    u32 i3 = (u32)((va >> 30) & 0x1FF);
    if (!(pdpt[i3] & 1)) return 0;
    u64 *pd = table_at(pdpt[i3] & ~0xFFFull);
    u32 i2 = (u32)((va >> 21) & 0x1FF);
    if (!(pd[i2] & 1)) return 0;
    if (pd[i2] & 0x80) return pd[i2];
    u64 *pt = table_at(pd[i2] & ~0xFFFull);
    u32 i1 = (u32)((va >> 12) & 0x1FF);
    return pt[i1];
}

int vmm_is_mapped(u64 va)
{
    u64 *pml4 = table_at(PML4_BASE);
    u32 i4 = (u32)((va >> 39) & 0x1FF);
    if (!(pml4[i4] & 1)) return 0;
    u64 *pdpt = table_at(pml4[i4] & ~0xFFFull);
    u32 i3 = (u32)((va >> 30) & 0x1FF);
    if (!(pdpt[i3] & 1)) return 0;
    u64 *pd = table_at(pdpt[i3] & ~0xFFFull);
    u32 i2 = (u32)((va >> 21) & 0x1FF);
    if (!(pd[i2] & 1)) return 0;
    if (pd[i2] & 0x80) return 1;
    u64 *pt = table_at(pd[i2] & ~0xFFFull);
    u32 i1 = (u32)((va >> 12) & 0x1FF);
    return (pt[i1] & 1) != 0;
}

/* ---------------- 自检 ---------------- */
int mem_selftest(void)
{
    int ok = 1;

    u8 *a = (u8 *)kmalloc(64);
    u8 *b = (u8 *)kmalloc(1000);
    if (!a || !b) return -1;
    for (int i = 0; i < 64; i++) a[i] = (u8)i;
    for (int i = 0; i < 1000; i++) b[i] = (u8)(i * 7);
    for (int i = 0; i < 64; i++) if (a[i] != (u8)i) ok = 0;
    for (int i = 0; i < 1000; i++) if (b[i] != (u8)(i * 7)) ok = 0;
    if (a == b) ok = 0;
    kfree(a);
    kfree(b);

    /* 用户窗口映射：USER_BASE -> 新页，写读后解映射 */
    void *pa = pmm_alloc();
    if (!pa) return -2;
    if (vmm_map_page(USER_BASE, (u64)pa, 0x07) != 0) ok = 0;
    volatile u64 *u = (volatile u64 *)USER_BASE;
    *u = 0x1122334455667788ull;
    if (*u != 0x1122334455667788ull) ok = 0;
    if (vmm_unmap_page(USER_BASE) != 0) ok = 0;
    pmm_free(pa);

    serial_puts(ok ? "[kernel] mem selftest: kmalloc/vmm ok\n"
                   : "[kernel] mem selftest: FAILED\n");
    return ok ? 0 : -3;
}
