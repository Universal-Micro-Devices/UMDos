/* ata.c - 安装引导用 ATA PIO：扫描 4 个设备，可读写 */
#include "ata.h"
#include "io.h"

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01

static u16 base_port(int channel) { return channel ? 0x170 : 0x1F0; }
static u16 ctrl_port(int channel) { return channel ? 0x376 : 0x3F6; }

static int wait_status(int channel, u8 want, int loops)
{
    u16 port = (u16)(base_port(channel) + 7);
    for (int i = 0; i < loops; i++) {
        u8 st = inb(port);
        if (st == 0xFF) return -1;
        if (st & ST_ERR) return -2;
        if (st & ST_BSY) continue;
        if ((st & want) == want) return 0;
    }
    return -3;
}

static int identify(int channel, int master, ata_dev *dev)
{
    u16 b = base_port(channel);
    outb((u16)(b + 6), (u8)(master ? 0xA0 : 0xB0));
    for (int i = 0; i < 4; i++) (void)inb((u16)(b + 7));
    outb((u16)(b + 2), 0);
    outb((u16)(b + 3), 0);
    outb((u16)(b + 4), 0);
    outb((u16)(b + 5), 0);
    outb((u16)(b + 7), 0xEC);
    u8 st = inb((u16)(b + 7));
    if (st == 0 || st == 0xFF)
        return -1;
    if (wait_status(channel, ST_DRQ, 1000000) != 0)
        return -2;

    u16 id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(b);
    if (id[0] == 0)
        return -3;

    u32 secs = ((u32)id[61] << 16) | id[60];
    dev->present = 1;
    dev->channel = channel;
    dev->master = master;
    dev->sectors = secs;
    return 0;
}

int ata_scan(ata_dev out[4])
{
    int n = 0;
    for (int ch = 0; ch < 2; ch++) {
        for (int m = 0; m < 2; m++) {
            ata_dev d;
            d.present = 0;
            d.channel = ch;
            d.master = m;
            d.sectors = 0;
            if (identify(ch, m, &d) == 0 && n < 4) {
                out[n++] = d;
            }
        }
    }
    return n;
}

static int setup(const ata_dev *d, u32 lba, u8 count, u8 cmd)
{
    if (!d->present) return -1;
    u16 b = base_port(d->channel);
    if (wait_status(d->channel, 0, 1000000) != 0) return -1;
    outb((u16)(b + 6), (u8)(0xE0 | (d->master ? 0 : 0x10) | ((lba >> 24) & 0x0F)));
    for (int i = 0; i < 4; i++) (void)inb((u16)(b + 7));
    outb((u16)(b + 2), count);
    outb((u16)(b + 3), (u8)(lba & 0xFF));
    outb((u16)(b + 4), (u8)((lba >> 8) & 0xFF));
    outb((u16)(b + 5), (u8)((lba >> 16) & 0xFF));
    outb((u16)(b + 7), cmd);
    return 0;
}

int ata_read_dev(const ata_dev *d, u32 lba, u8 count, void *buf)
{
    if (count == 0) return -1;
    if (setup(d, lba, count, 0x20) != 0) return -1;
    u16 b = base_port(d->channel);
    u16 *p = (u16 *)buf;
    for (int s = 0; s < count; s++) {
        if (wait_status(d->channel, ST_DRQ, 2000000) != 0) return -2;
        for (int w = 0; w < 256; w++)
            p[s * 256 + w] = inw(b);
    }
    return 0;
}

int ata_write_dev(const ata_dev *d, u32 lba, u8 count, const void *buf)
{
    if (count == 0) return -1;
    if (setup(d, lba, count, 0x30) != 0) return -1;
    u16 b = base_port(d->channel);
    const u16 *p = (const u16 *)buf;
    for (int s = 0; s < count; s++) {
        if (wait_status(d->channel, ST_DRQ, 2000000) != 0) return -2;
        for (int w = 0; w < 256; w++)
            outw(b, p[s * 256 + w]);
    }
    return 0;
}

int ata_flush_dev(const ata_dev *d)
{
    if (!d->present) return -1;
    outb((u16)(base_port(d->channel) + 7), 0xE7);
    return wait_status(d->channel, 0, 2000000) == 0 ? 0 : -1;
}
