/* ahci.h - AHCI 驱动（P16/P17：多端口、多盘） */
#ifndef AHCI_H
#define AHCI_H

#include "types.h"

#define AHCI_MAX_DISKS 4

int  ahci_init(void);                  /* 返回识别到的磁盘数（0 = 没有） */
int  ahci_disk_count(void);
u32  ahci_disk_sectors(int idx);
const char *ahci_disk_model(int idx);
int  ahci_disk_read(int idx, u32 lba, u8 count, void *buf);
int  ahci_disk_write(int idx, u32 lba, u8 count, const void *buf);
int  ahci_selftest(void);
int  ahci_present(void);

#endif
