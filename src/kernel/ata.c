/* ata.c - 主 IDE 通道的 ATA PIO 驱动（轮询，无中断） */
#include "ata.h"
#include "io.h"

#define ATA_DATA    0x1F0
#define ATA_ERROR   0x1F1
#define ATA_COUNT   0x1F2
#define ATA_LBA0    0x1F3
#define ATA_LBA1    0x1F4
#define ATA_LBA2    0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_STATUS  0x1F7
#define ATA_CMD     0x1F7
#define ATA_ALT     0x3F6

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01

static int present;
static u32 total_sectors;

static int wait_status(u8 want, int loops)
{
    for (int i = 0; i < loops; i++) {
        u8 st = inb(ATA_STATUS);
        if (st == 0xFF) return -1;          /* 没有设备 */
        if (st & ST_ERR) return -2;
        if (st & ST_BSY) continue;
        if ((st & want) == want) return 0;
    }
    return -3;
}

int ata_init(void)
{
    present = 0;
    total_sectors = 0;
    (void)inb(ATA_ALT);                      /* 清中断状态 */

    outb(ATA_DRIVE, 0xA0);                   /* 主盘 */
    for (int i = 0; i < 4; i++) (void)inb(ATA_STATUS);
    if (inb(ATA_STATUS) == 0xFF)
        return -1;                            /* 无控制器 */

    outb(ATA_COUNT, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_CMD, 0xEC);                     /* IDENTIFY */
    u8 st = inb(ATA_STATUS);
    if (st == 0 || st == 0xFF)
        return -1;
    if (wait_status(ST_DRQ, 1000000) != 0)
        return -2;

    u16 id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(ATA_DATA);

    if (id[0] == 0)
        return -3;
    total_sectors = ((u32)id[61] << 16) | id[60];
    present = 1;
    return 0;
}

int ata_present(void) { return present; }
u32 ata_sectors(void) { return total_sectors; }

static int setup(u32 lba, u8 count, u8 cmd)
{
    if (!present) return -1;
    if (wait_status(0, 1000000) != 0) return -1;
    outb(ATA_DRIVE, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
    for (int i = 0; i < 4; i++) (void)inb(ATA_STATUS);
    outb(ATA_COUNT, count);
    outb(ATA_LBA0, (u8)(lba & 0xFF));
    outb(ATA_LBA1, (u8)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (u8)((lba >> 16) & 0xFF));
    outb(ATA_CMD, cmd);
    return 0;
}

int ata_read(u32 lba, u8 count, void *buf)
{
    if (count == 0) return -1;
    if (setup(lba, count, 0x20) != 0) return -1;
    u16 *p = (u16 *)buf;
    for (int s = 0; s < count; s++) {
        if (wait_status(ST_DRQ, 2000000) != 0) return -2;
        for (int w = 0; w < 256; w++)
            p[s * 256 + w] = inw(ATA_DATA);
    }
    return 0;
}

int ata_write(u32 lba, u8 count, const void *buf)
{
    if (count == 0) return -1;
    if (setup(lba, count, 0x30) != 0) return -1;
    const u16 *p = (const u16 *)buf;
    for (int s = 0; s < count; s++) {
        if (wait_status(ST_DRQ, 2000000) != 0) return -2;
        for (int w = 0; w < 256; w++)
            outw(ATA_DATA, p[s * 256 + w]);
    }
    return 0;
}

int ata_flush(void)
{
    if (!present) return -1;
    outb(ATA_CMD, 0xE7);                     /* FLUSH CACHE */
    return wait_status(0, 2000000) == 0 ? 0 : -1;
}
