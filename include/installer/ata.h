/* ata.h - 安装引导用 ATA 设备 */
#ifndef INST_ATA_H
#define INST_ATA_H

#include "types.h"

typedef struct {
    int present;
    int channel;        /* 0=主通道 1=从通道 */
    int master;         /* 1=主盘 0=从盘 */
    u32 sectors;
} ata_dev;

int ata_scan(ata_dev out[4]);
int ata_read_dev(const ata_dev *d, u32 lba, u8 count, void *buf);
int ata_write_dev(const ata_dev *d, u32 lba, u8 count, const void *buf);
int ata_flush_dev(const ata_dev *d);

#endif
