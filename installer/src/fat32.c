/* fat32.c - 安装引导用的最小 FAT32 格式化与文件写入
 *
 * 只实现安装需要的部分：格式化一个分区、创建 CONFIG 目录、写入 AUTH 文件，
 * 以及把文件读回来做校验。卷参数固定为 512B/簇、32 个保留扇区、2 个 FAT。
 */
#include "fat32.h"
#include "ata.h"
#include "io.h"

#define SEC_PER_CLUSTER 1
#define RESERVED        32
#define NUM_FATS        2
#define ROOT_CLUSTER    2

static const ata_dev *dev;
static u32 part_start, part_sectors, fat_size, clusters;
static u32 next_cluster;

static u8 secbuf[512];

static u32 cluster_lba(u32 c)
{
    /* 数据区在 保留扇区 + 两个 FAT 之后 */
    return part_start + RESERVED + NUM_FATS * fat_size + (c - 2) * SEC_PER_CLUSTER;
}

static int rd_sec(u32 lba, void *buf) { return ata_read_dev(dev, lba, 1, buf); }
static int wr_sec(u32 lba, const void *buf) { return ata_write_dev(dev, lba, 1, buf); }

static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static u32 get32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* ---------------- FAT 表 ---------------- */
static int fat_write(u32 f0, u32 value)
{
    u32 entry = f0 * 4;
    u32 sec_off = entry / 512;
    u32 inside = entry % 512;
    for (int f = 0; f < NUM_FATS; f++) {
        u32 lba = part_start + RESERVED + (u32)f * fat_size + sec_off;
        if (rd_sec(lba, secbuf) != 0) return -1;
        put32(secbuf + inside, value & 0x0FFFFFFF);
        if (wr_sec(lba, secbuf) != 0) return -2;
    }
    return 0;
}

static u32 fat_read(u32 f0)
{
    u32 entry = f0 * 4;
    u32 lba = part_start + RESERVED + entry / 512;
    if (rd_sec(lba, secbuf) != 0) return 0x0FFFFFFF;
    return get32(secbuf + (entry % 512)) & 0x0FFFFFFF;
}

/* ---------------- 格式化 ---------------- */
int fat32_format(const ata_dev *d, u32 start, u32 sectors, char *err)
{
    dev = d;
    part_start = start;
    part_sectors = sectors;
    if (sectors < 2048) { strcpy_s(err, 64, "partition too small"); return -1; }

    u32 data_sectors = sectors - RESERVED;
    clusters = (sectors - RESERVED) / SEC_PER_CLUSTER;
    for (int iter = 0; iter < 6; iter++) {
        fat_size = ((clusters + 2) * 4 + 511) / 512;
        if (fat_size < 1) fat_size = 1;
        u32 avail = sectors - RESERVED - NUM_FATS * fat_size;
        u32 c2 = avail / SEC_PER_CLUSTER;
        if (c2 == clusters) break;
        clusters = c2;
    }
    u32 need = RESERVED + NUM_FATS * fat_size;
    if (need >= sectors) { strcpy_s(err, 64, "partition too small for FAT32"); return -2; }

    /* 引导扇区 */
    for (int i = 0; i < 512; i++) secbuf[i] = 0;
    secbuf[0] = 0xEB; secbuf[1] = 0x58; secbuf[2] = 0x90;
    const char *oem = "UMDOS   ";
    for (int i = 0; i < 8; i++) secbuf[3 + i] = (u8)oem[i];
    put16(secbuf + 11, 512);
    secbuf[13] = SEC_PER_CLUSTER;
    put16(secbuf + 14, RESERVED);
    secbuf[16] = NUM_FATS;
    put16(secbuf + 17, 0);
    put16(secbuf + 19, 0);
    secbuf[21] = 0xF8;
    put16(secbuf + 22, 0);
    put16(secbuf + 24, 63);
    put16(secbuf + 26, 255);
    put32(secbuf + 28, start);
    put32(secbuf + 32, sectors);
    put32(secbuf + 36, fat_size);
    put16(secbuf + 40, 0);
    put16(secbuf + 42, 0);
    put32(secbuf + 44, ROOT_CLUSTER);
    put16(secbuf + 48, 1);
    put16(secbuf + 50, 6);
    secbuf[64] = 0x80;
    secbuf[66] = 0x29;
    put32(secbuf + 67, 0x554D4430u);
    const char *lab = "UMDOS      ";
    for (int i = 0; i < 11; i++) secbuf[71 + i] = (u8)lab[i];
    const char *fst = "FAT32   ";
    for (int i = 0; i < 8; i++) secbuf[82 + i] = (u8)fst[i];
    secbuf[510] = 0x55; secbuf[511] = 0xAA;

    if (wr_sec(part_start, secbuf) != 0) { strcpy_s(err, 64, "write boot sector failed"); return -3; }
    if (wr_sec(part_start + 6, secbuf) != 0) { strcpy_s(err, 64, "write backup boot failed"); return -3; }

    /* FSInfo */
    for (int i = 0; i < 512; i++) secbuf[i] = 0;
    put32(secbuf + 0, 0x41615252u);
    put32(secbuf + 484, 0x61417272u);
    put32(secbuf + 488, clusters - 8);
    put32(secbuf + 492, 8);
    put32(secbuf + 508, 0xAA550000u);
    wr_sec(part_start + 1, secbuf);
    wr_sec(part_start + 7, secbuf);

    /* 两个 FAT 先整体清零（保证干净），再写前几个保留项 */
    for (int i = 0; i < 512; i++) secbuf[i] = 0;
    for (int f = 0; f < NUM_FATS; f++) {
        u32 base = part_start + RESERVED + (u32)f * fat_size;
        for (u32 s = 0; s < fat_size; s++)
            if (wr_sec(base + s, secbuf) != 0) { strcpy_s(err, 64, "clear FAT failed"); return -4; }
    }
    fat_write(0, 0x0FFFFFF8);
    fat_write(1, 0x0FFFFFFF);
    fat_write(ROOT_CLUSTER, 0x0FFFFFFF);
    next_cluster = ROOT_CLUSTER + 1;

    /* 根目录簇清零 */
    for (int i = 0; i < 512; i++) secbuf[i] = 0;
    if (wr_sec(cluster_lba(ROOT_CLUSTER), secbuf) != 0) { strcpy_s(err, 64, "clear root failed"); return -5; }

    return 0;
}

/* ---------------- 目录项 ----------------
 * FAT32 目录项布局：name[11] attr(11) ... 簇号高 16 位在 20，低 16 位在 26
 */
#define DIR_DATE ((u16)(((2024 - 1980) << 9) | (1 << 5) | 1))

static void fill_entry(u8 *e, const char *name83, u8 attr, u32 cluster, u32 size)
{
    for (int i = 0; i < 32; i++) e[i] = 0;
    for (int i = 0; i < 11; i++) e[i] = (u8)name83[i];
    e[11] = attr;
    put16(e + 14, 0);                                /* 创建时间 00:00 */
    put16(e + 16, DIR_DATE);                         /* 创建日期 */
    put16(e + 18, DIR_DATE);                         /* 访问日期 */
    put16(e + 20, (u16)(cluster >> 16));             /* 簇号高 16 位 */
    put16(e + 22, 0);                                /* 修改时间 */
    put16(e + 24, DIR_DATE);                         /* 修改日期 */
    put16(e + 26, (u16)(cluster & 0xFFFF));          /* 簇号低 16 位 */
    put32(e + 28, size);
}

static u32 entry_cluster(const u8 *e)
{
    u32 hi = (u32)e[20] | ((u32)e[21] << 8);
    u32 lo = (u32)e[26] | ((u32)e[27] << 8);
    return (hi << 16) | lo;
}

/* 在目录簇里追加一个目录项 */
static int dir_add(u32 dir_cluster, const u8 entry[32])
{
    u32 lba = cluster_lba(dir_cluster);
    for (int i = 0; i < (int)SEC_PER_CLUSTER; i++) {
        if (rd_sec(lba + (u32)i, secbuf) != 0) return -1;
        for (int k = 0; k < 16; k++) {
            u8 *e = secbuf + k * 32;
            if (e[0] == 0x00 || e[0] == 0xE5) {
                for (int j = 0; j < 32; j++) e[j] = entry[j];
                return wr_sec(lba + (u32)i, secbuf);
            }
        }
    }
    return -2;                                            /* 目录满了（dev.1 足够用） */
}

/* 创建 CONFIG 目录，返回簇号 */
int fat32_create_dir(const ata_dev *d, u32 start, u32 parent, const char *name83, u32 cluster, char *err)
{
    (void)d; (void)start;
    u8 e[32];
    next_cluster = cluster + 1;
    fill_entry(e, name83, 0x10, cluster, 0);
    if (dir_add(parent, e) != 0) { strcpy_s(err, 64, "add dir entry failed"); return -1; }

    /* 目录自身的 . 和 .. */
    for (int i = 0; i < 512; i++) secbuf[i] = 0;
    u8 dot[32];
    fill_entry(dot, ".          ", 0x10, cluster, 0);
    for (int i = 0; i < 32; i++) secbuf[i] = dot[i];
    fill_entry(dot, "..         ", 0x10, parent == ROOT_CLUSTER ? 0 : parent, 0);
    for (int i = 0; i < 32; i++) secbuf[32 + i] = dot[i];
    if (wr_sec(cluster_lba(cluster), secbuf) != 0) { strcpy_s(err, 64, "write dir cluster failed"); return -2; }

    fat_write(cluster, 0x0FFFFFFF);
    return (int)cluster;
}

int fat32_write_file(const ata_dev *d, u32 start, u32 dir_cluster, const char *name83,
                     const void *data, u32 len, char *err)
{
    (void)d; (void)start;
    u32 nclusters = (len + SEC_PER_CLUSTER * 512 - 1) / (SEC_PER_CLUSTER * 512);
    if (nclusters == 0) nclusters = 1;
    u32 first = next_cluster;                             /* 顺序分配 */
    if (first + nclusters + 2 >= clusters) { strcpy_s(err, 64, "no space"); return -1; }
    next_cluster = first + nclusters;

    const u8 *p = (const u8 *)data;
    for (u32 i = 0; i < nclusters; i++) {
        for (int s = 0; s < SEC_PER_CLUSTER; s++) {
            for (int k = 0; k < 512; k++) secbuf[k] = 0;
            u32 off = i * SEC_PER_CLUSTER * 512 + (u32)s * 512;
            for (int k = 0; k < 512 && off + (u32)k < len; k++)
                secbuf[k] = p[off + k];
            if (wr_sec(cluster_lba(first + i) + (u32)s, secbuf) != 0) {
                strcpy_s(err, 64, "write data failed");
                return -2;
            }
        }
        fat_write(first + i, (i == nclusters - 1) ? 0x0FFFFFFF : (first + i + 1));
    }

    u8 e[32];
    fill_entry(e, name83, 0x20, first, len);
    if (dir_add(dir_cluster, e) != 0) { strcpy_s(err, 64, "add file entry failed"); return -3; }
    return 0;
}

int fat32_read_file(const ata_dev *d, u32 start, const char *name83,
                    void *buf, u32 cap, u32 *out_len)
{
    dev = d;
    part_start = start;
    u32 data_sectors = (part_sectors ? part_sectors : 0);
    (void)data_sectors;

    /* 读根目录，找 CONFIG 目录 */
    static const char CONFIG83[12] = "CONFIG     ";
    u32 lba = cluster_lba(ROOT_CLUSTER);
    if (rd_sec(lba, secbuf) != 0) return -1;
    u32 cluster = 0, size = 0;
    for (int k = 0; k < 16; k++) {
        u8 *e = secbuf + k * 32;
        if (e[0] == 0x00) break;
        int match = 1;
        for (int i = 0; i < 11; i++)
            if (e[i] != (u8)CONFIG83[i]) { match = 0; break; }
        if (match && (e[11] & 0x10)) {
            cluster = entry_cluster(e);
            break;
        }
    }
    if (!cluster) return -2;

    /* 进 CONFIG 目录找 AUTH */
    lba = cluster_lba(cluster);
    if (rd_sec(lba, secbuf) != 0) return -3;
    u32 fcluster = 0;
    for (int k = 0; k < 16; k++) {
        u8 *e = secbuf + k * 32;
        if (e[0] == 0x00) break;
        if (e[0] == '.') continue;
        int match = 1;
        for (int i = 0; i < 11; i++)
            if (e[i] != (u8)name83[i]) { match = 0; break; }
        if (match && !(e[11] & 0x10)) {
            fcluster = entry_cluster(e);
            size = get32(e + 28);
            break;
        }
    }
    if (!fcluster) return -4;

    u32 n = size < cap ? size : cap;
    u32 done = 0;
    u32 c = fcluster;
    while (done < n) {
        if (rd_sec(cluster_lba(c), secbuf) != 0) return -5;
        for (int k = 0; k < 512 && done < n; k++)
            ((u8 *)buf)[done++] = secbuf[k];
        c = fat_read(c);
        if (c >= 0x0FFFFFF8) break;
    }
    if (out_len) *out_len = n;
    return 0;
}

int fat32_part_info(u32 *start, u32 *sectors, u32 *nclusters, u32 *fatsz)
{
    if (start) *start = part_start;
    if (sectors) *sectors = part_sectors;
    if (nclusters) *nclusters = clusters;
    if (fatsz) *fatsz = fat_size;
    return 0;
}
