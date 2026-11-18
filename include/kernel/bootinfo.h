/* bootinfo.h - FirstMain 与内核之间的约定结构（位于物理地址 0x7000）
 *
 * 前 32 字节沿用 VBE 信息块布局，供 16 位阶段填写；后面是扩展字段。
 * 所有字段都是 32 位小端。
 */
#ifndef BOOTINFO_H
#define BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_ADDR   0x7000u
#define BOOTINFO_MAGIC  0x31444d55u      /* "UMD1" */

typedef struct {
    uint32_t fb_base_lo;        /* +0  帧缓冲物理地址低 32 位 */
    uint32_t fb_base_hi;        /* +4  高 32 位（本系统恒为 0） */
    uint32_t fb_bytes;          /* +8  帧缓冲字节数 */
    uint32_t reserved0;         /* +12 */
    uint32_t fb_width;          /* +16 */
    uint32_t fb_height;         /* +20 */
    uint32_t fb_stride;         /* +24 每行像素数 */
    uint32_t fb_bpp;            /* +28 位深 */

    uint32_t magic;             /* +32 BOOTINFO_MAGIC */
    uint32_t kernel_lba;        /* +36 内核镜像起始 LBA */
    uint32_t kernel_sectors;    /* +40 */
    uint32_t fs_lba;            /* +44 文件系统镜像起始 LBA */
    uint32_t fs_sectors;        /* +48 */
    uint32_t part_count;        /* +52 读到的分区数（0 表示无分区表） */
    uint32_t part_lba[4];       /* +56 每个分区的起始 LBA */
    uint32_t part_size[4];      /* +72 每个分区的扇区数 */
    uint32_t creds_addr;        /* +88 凭据块物理地址（0x0700） */
    uint32_t splash_seconds;    /* +92 启动画面停留秒数 */
    uint32_t flags;             /* +96 bit0 = 跳过停留（调试用） */
    uint32_t memmap_addr;       /* +100 E820 内存图地址（0 表示没有） */
    uint32_t memmap_count;      /* +104 条目数 */
    uint32_t debug_idt_base;    /* +108 IDT 基址（调试约定，供测试工具投递中断） */
    uint32_t env_flags;         /* +112 bit0 = 没有 GDT/IDT 模型（模拟器），跳过 ring3 */
} bootinfo_t;

#endif
