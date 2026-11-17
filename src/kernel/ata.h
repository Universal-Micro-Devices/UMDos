/* ata.h - ATA PIO（28 位 LBA）读写 */
#ifndef ATA_H
#define ATA_H

#include "types.h"

int  ata_init(void);                 /* 返回 0 表示主盘可用 */
int  ata_present(void);
u32  ata_sectors(void);
int  ata_read(u32 lba, u8 count, void *buf);
int  ata_write(u32 lba, u8 count, const void *buf);
int  ata_flush(void);

#endif
