/* fat32.h - 安装引导用的最小 FAT32 支持 */
#ifndef INST_FAT32_H
#define INST_FAT32_H

#include "types.h"
#include "ata.h"

int fat32_format(const ata_dev *d, u32 start, u32 sectors, char *err);
int fat32_create_dir(const ata_dev *d, u32 start, u32 parent, const char *name83,
                     u32 cluster, char *err);
int fat32_write_file(const ata_dev *d, u32 start, u32 dir_cluster, const char *name83,
                     const void *data, u32 len, char *err);
int fat32_read_file(const ata_dev *d, u32 start, const char *name83,
                    void *buf, u32 cap, u32 *out_len);
int fat32_part_info(u32 *start, u32 *sectors, u32 *nclusters, u32 *fatsz);

#define FAT32_ROOT_CLUSTER 2

#endif
