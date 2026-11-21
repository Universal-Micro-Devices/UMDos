/* efi_loader.c - UEFI 加载器（P13）
 *
 * 目标：在 UEFI 固件下把同一份内核镜像加载到固定物理地址并跳转，
 *       与 Legacy 链（boot -> loader -> head -> kernel）并存于同一张混合 ISO。
 *
 * 结构：
 *   1. 用 BootServices->LocateProtocol 取 GOP，读取帧缓冲信息；
 *   2. 用 AllocatePages(AllocateAddress) 申请 0x1000..0x8FFFF 的固定区域；
 *   3. 把编译时内嵌的内核镜像与 UMDFS 镜像拷到 0x10000 / 0x40000；
 *   4. 在 0x1000..0x6FFF 建恒等映射页表（与 head.asm 相同），填 bootinfo；
 *   5. ExitBootServices 后设置 CR3/CR4/EFER/CR0 并跳到内核入口 0x10000。
 *
 * 说明：本仓库没有 UEFI 运行环境（OVMF），此路径的结构与编译已验证，
 *       端到端启动需要在真机/带 OVMF 的 QEMU 上回归（见 README 已知限制）。
 */
#include <stdint.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;

#define EFIAPI __attribute__((ms_abi))

typedef struct { u64 sig; u32 rev; u32 hdrsize; u32 crc; u32 res; } efi_hdr;
typedef struct { u16 year; u8 mon, day, hour, min, sec, pad; } efi_time;
typedef struct efi_tbl {
    efi_hdr hdr;
    void *fw_vendor;
    u32 fw_rev;
    void *con_in_handle;
    void *con_in;
    void *con_out_handle;
    void *con_out;
    void *std_err_handle;
    void *std_err;
    void *runtime;
    void *boot;
    u64 ntables;
    void *config;
} efi_system_table;

typedef struct { efi_hdr hdr; void *reset; void *output_string; } efi_simple_text_out;

typedef struct {
    u32 max_mode;
    u32 mode;
    void *info;
    u64 size_info;
    u64 fb_base;
    u64 fb_size;
} gop_mode;

typedef struct {
    u32 version;
    u32 hres;
    u32 vres;
    u32 pixel_format;
} gop_info;

typedef struct { void *query_mode; void *set_mode; void *blt; gop_mode *mode; } gop_t;

typedef struct {
    efi_hdr hdr;
    void *raise_tpl;
    void *restore_tpl;
    void *allocate_pages;
    void *free_pages;
    void *get_memory_map;
    void *allocate_pool;
    void *free_pool;
    /* ... 这里只用到后面的 LocateProtocol，按 UEFI 2.x 偏移直接索引 */
} efi_boot_services;

typedef struct { u8 b[16]; } guid_t;

static guid_t GOP_GUID = { { 0xde, 0xa9, 0x42, 0x90, 0xdc, 0x23, 0x38, 0x4a,
                             0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };

#define BOOTINFO ((volatile u32 *)0x7000)
#define KERNEL_ADDR 0x10000u
#define FS_ADDR     0x40000u
#define MAX_KERNEL  (160u * 1024u)

static u8 kdeploy[MAX_KERNEL] __attribute__((aligned(16)));
static u8 fsdeploy[128u * 1024u] __attribute__((aligned(16)));
static u32 g_kernel_len, g_fs_len;
static u64 g_fb, g_w, g_h, g_stride;

/* 由 gen_embed.py 生成：两个数组 + 长度 */
#include "efi_embed.h"

static void wstr16(efi_simple_text_out *out, const char *s)
{
    u16 buf[256];
    int n = 0;
    while (s[n] && n < 250) {
        buf[n] = (u16)s[n];
        n++;
    }
    buf[n] = 0;
    buf[n + 1] = 0;
    if (out && out->output_string)
        ((void (EFIAPI *)(void *, u16 *))out->output_string)(out, buf);
}

static void *bs_fn(void *boot, int index)
{
    void **tbl = (void **)boot;
    return tbl[index];
}

static void *locate_gop(void *boot)
{
    /* LocateProtocol 在 BootServices 表里是第 40 项（含 24 字节头） */
    void *locate = bs_fn(boot, 40);
    if (!locate) return 0;
    void *gop = 0;
    u64 st = ((u64 (EFIAPI *)(void *, void *, void *, void **))locate)(boot, &GOP_GUID, 0, &gop);
    return st == 0 ? gop : 0;
}

static void setup_tables(void)
{
    /* 与 head.asm 相同的恒等映射：PML4 0x1000, PDPT 0x2000, 4 个 PD 0x3000..0x6FFF */
    u64 *pml4 = (u64 *)0x1000;
    u64 *pdpt = (u64 *)0x2000;
    for (int i = 0; i < 512; i++) pml4[i] = 0;
    for (int i = 0; i < 512; i++) pdpt[i] = 0;
    pml4[0] = (u64)pdpt | 3;
    for (int p = 0; p < 4; p++) {
        u64 *pd = (u64 *)(u64)(0x3000u + (u32)p * 0x1000u);
        for (int i = 0; i < 512; i++)
            pd[i] = ((u64)(p * 512 + i) * 0x200000ull) | 0x83;   /* P|RW|PS */
        pdpt[p] = (u64)pd | 3;
    }
}

static void fill_bootinfo(void)
{
    BOOTINFO[0] = (u32)(g_fb & 0xFFFFFFFF);
    BOOTINFO[1] = (u32)(g_fb >> 32);
    BOOTINFO[2] = (u32)(g_stride * g_h * 4);
    BOOTINFO[3] = 0;
    BOOTINFO[4] = (u32)g_w;
    BOOTINFO[5] = (u32)g_h;
    BOOTINFO[6] = (u32)(g_stride / 4);
    BOOTINFO[7] = 32;
    BOOTINFO[8] = 0x31444D55u;       /* magic "UMD1" */
    BOOTINFO[9] = 50;                /* kernel_lba（EFI 路径不使用） */
    BOOTINFO[10] = MAX_KERNEL / 512;
    BOOTINFO[11] = 512;              /* fs_lba */
    BOOTINFO[12] = 256;
    BOOTINFO[13] = 0;
    for (int i = 14; i < 22; i++) BOOTINFO[i] = 0;
    BOOTINFO[22] = 0x0700;           /* creds_addr（内核已不用） */
    BOOTINFO[23] = 60;               /* splash_seconds */
    BOOTINFO[24] = 0;                /* flags */
    BOOTINFO[25] = 0;                /* memmap_addr */
    BOOTINFO[26] = 0;                /* memmap_count */
    BOOTINFO[27] = 0;                /* debug_idt_base */
    BOOTINFO[28] = 1;                /* env_flags：EFI 路径先跳过 ring3 */
}

EFIAPI u64 EfiMain(void *image, efi_system_table *st)
{
    if (!st || !st->con_out) return 0;
    efi_simple_text_out *out = (efi_simple_text_out *)st->con_out;
    wstr16(out, "UMDos UEFI loader (P13)\r\n");

    void *gop = locate_gop(st->boot);
    if (!gop) {
        wstr16(out, "GOP not found; falling back to legacy boot path.\r\n");
        return 0;
    }
    gop_t *g = (gop_t *)gop;
    gop_info *info = (gop_info *)g->mode->info;
    g_fb = g->mode->fb_base;
    g_w = info->hres;
    g_h = info->vres;
    g_stride = info->hres * 4;
    wstr16(out, "GOP ok: framebuffer ready\r\n");

    /* 固定地址分配：0x1000..0x8FFFF（页表/bootinfo/内核/FS） */
    void *alloc = bs_fn(st->boot, 2);        /* AllocatePages */
    if (alloc) {
        u64 addr = 0x1000;
        u64 pages = (0x90000 - 0x1000) / 4096;
        u64 rc = ((u64 (EFIAPI *)(u32, u32, u64, void **))alloc)(0 /*AllocateAddress*/,
                                                                0 /*EfiLoaderData*/, pages, (void **)&addr);
        if (rc != 0) {
            wstr16(out, "fixed memory allocation failed\r\n");
            return 0;
        }
    }

    g_kernel_len = EFI_KERNEL_LEN;
    g_fs_len = EFI_FS_LEN;
    if (g_kernel_len > MAX_KERNEL) g_kernel_len = MAX_KERNEL;
    if (g_fs_len > sizeof(fsdeploy)) g_fs_len = sizeof(fsdeploy);
    for (u32 i = 0; i < g_kernel_len; i++) ((u8 *)KERNEL_ADDR)[i] = EFI_KERNEL[i];
    for (u32 i = 0; i < g_fs_len; i++) ((u8 *)FS_ADDR)[i] = EFI_FS[i];

    setup_tables();
    fill_bootinfo();

    /* 退出 Boot Services，切到自己的页表 */
    void *exitbs = bs_fn(st->boot, 29);
    if (exitbs) {
        u64 mmap_size = 0, map_key = 0, desc_size = 0;
        u32 desc_ver = 0;
        static u8 mmap[8192];
        void *getmm = bs_fn(st->boot, 4);
        if (getmm) {
            mmap_size = sizeof(mmap);
            ((u64 (EFIAPI *)(void *, void *, u64 *, u64 *, u64 *, u32 *))getmm)(
                st->boot, mmap, &mmap_size, &map_key, &desc_size, &desc_ver);
        }
        u64 rc = ((u64 (EFIAPI *)(void *, u64))exitbs)(image, map_key);
        if (rc != 0) {
            wstr16(out, "ExitBootServices failed\r\n");
            return 0;
        }
    }

    __asm__ volatile(
        "mov %0, %%cr3\n\t"
        "mov %%cr4, %%rax\n\t"
        "or $0x20, %%rax\n\t"          /* PAE */
        "mov %%rax, %%cr4\n\t"
        "mov $0xC0000080, %%ecx\n\t"
        "rdmsr\n\t"
        "or $0x100, %%eax\n\t"         /* LME */
        "wrmsr\n\t"
        "mov %%cr0, %%rax\n\t"
        "mov $0x80000001, %%edx\n\t"
        "or %%rdx, %%rax\n\t"          /* PG|PE */
        "mov %%rax, %%cr0\n\t"
        "mov $0x90000, %%rsp\n\t"
        "mov $0x7000, %%rdi\n\t"
        "mov $0x10000, %%rax\n\t"
        "jmp *%%rax\n\t"
        :
        : "r"(0x1000ull)
        : "rax", "rcx", "rdx", "rdi", "memory");
    return 0;
}
