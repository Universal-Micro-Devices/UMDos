/* ahci.c - AHCI (SATA) 驱动（P16/P17）
 *
 * P16：PCI 枚举 + HBA/端口初始化 + IDENTIFY/READ/WRITE（轮询 PxIS）
 * P17：遍历所有已实现端口，每个 SATA 盘独立命令列表/FIS/PRDT，注册为多块设备
 */
#include "ahci.h"
#include "pci.h"
#include "io.h"
#include "mem.h"

#define HBA_CLB_SIZE 1024
#define HBA_FIS_SIZE 256
#define HBA_CT_SIZE  256
#define SATA_SIG_ATA 0x00000101u

typedef struct {
    int used;
    u32 port_regs;
    u32 sectors;
    char model[41];
    u8 id[512] __attribute__((aligned(16)));
    u8 cl[1024] __attribute__((aligned(1024)));
    u8 fis[256] __attribute__((aligned(256)));
    u8 ct[256] __attribute__((aligned(256)));
} ahci_disk_t;

static volatile u8 *g_abar;
static ahci_disk_t g_disks[AHCI_MAX_DISKS];
static int g_ndisks;

static u32 abar_read(u32 off) { return *(volatile u32 *)(g_abar + off); }
static void abar_write(u32 off, u32 v) { *(volatile u32 *)(g_abar + off) = v; }

static void memset8(void *d, int c, u32 n)
{
    u8 *p = (u8 *)d;
    while (n--) *p++ = (u8)c;
}

static int disk_cmd_once(ahci_disk_t *d, u8 cmd, u32 lba, u32 count, void *buf)
{
    memset8(d->ct, 0, HBA_CT_SIZE);
    u8 *f = d->ct;
    f[0] = 0x27;
    f[1] = 0x80;
    f[2] = cmd;
    f[4] = (u8)(lba & 0xFF);
    f[5] = (u8)((lba >> 8) & 0xFF);
    f[6] = (u8)((lba >> 16) & 0xFF);
    f[7] = 0x40;
    f[8] = (u8)((lba >> 24) & 0xFF);
    f[12] = (u8)(count & 0xFF);
    f[13] = (u8)((count >> 8) & 0xFF);

    u64 dba = (u64)(u64)buf;
    u32 dbc = (count ? count : 1) * 512 - 1;
    u8 *p = d->ct + 0x80;
    for (int i = 0; i < 8; i++) p[i] = (u8)((dba >> (8 * i)) & 0xFF);
    p[8] = (u8)(dbc & 0xFF);
    p[9] = (u8)((dbc >> 8) & 0xFF);
    p[10] = (u8)((dbc >> 16) & 0xFF);

    memset8(d->cl, 0, HBA_CLB_SIZE);
    u64 ctba = (u64)(u64)d->ct;
    for (int i = 0; i < 8; i++) d->cl[i] = (u8)((ctba >> (8 * i)) & 0xFF);
    d->cl[8] = 0x05;
    d->cl[12] = 1;

    abar_write(d->port_regs + 0x10, 0xFFFFFFFFu);
    abar_write(d->port_regs + 0x38, 1);
    for (u32 t = 0; t < 20000000u; t++) {
        u32 is = abar_read(d->port_regs + 0x10);
        if (is & 0x8u) {
            abar_write(d->port_regs + 0x10, is);
            u32 tfd = abar_read(d->port_regs + 0x20);
            return (tfd & 0x01) ? -2 : 0;
        }
    }
    return -3;
}

/* P18：端口复位（COMRESET）+ 重试 */
static void port_reset(ahci_disk_t *d)
{
    abar_write(d->port_regs + 0x18, 0);                      /* PxCMD.ST = 0 */
    for (u32 t = 0; t < 1000000u && (abar_read(d->port_regs + 0x18) & 0x8000u); t++)
        ;
    abar_write(d->port_regs + 0x2C, 0x301u);                  /* PxSCTL DET=1 */
    for (volatile u32 i = 0; i < 200000u; i++)
        ;
    abar_write(d->port_regs + 0x2C, 0);
    for (u32 t = 0; t < 2000000u; t++) {
        if ((abar_read(d->port_regs + 0x28) & 0x0F) == 0x03) break;
    }
    abar_write(d->port_regs + 0x10, 0xFFFFFFFFu);
    u64 cl = (u64)(u64)d->cl, fb = (u64)(u64)d->fis;
    abar_write(d->port_regs + 0x00, (u32)cl);
    abar_write(d->port_regs + 0x04, (u32)(cl >> 32));
    abar_write(d->port_regs + 0x08, (u32)fb);
    abar_write(d->port_regs + 0x0C, (u32)(fb >> 32));
    abar_write(d->port_regs + 0x18, 0x1 | 0x10);
}

static int disk_cmd(ahci_disk_t *d, u8 cmd, u32 lba, u32 count, void *buf)
{
    if (!d || !d->used || !g_abar) return -1;
    int rc = disk_cmd_once(d, cmd, lba, count, buf);
    if (rc == 0) return 0;
    serial_puts("[kernel] ahci: port reset (cmd error)\n");
    port_reset(d);
    rc = disk_cmd_once(d, cmd, lba, count, buf);
    return rc;
}

static int disk_identify(ahci_disk_t *d)
{
    if (disk_cmd(d, 0xEC, 0, 1, d->id) != 0) return -1;
    int m = 0;
    for (int w = 27; w <= 46 && m < 40; w++) {
        d->model[m++] = (char)d->id[w * 2 + 1];
        d->model[m++] = (char)d->id[w * 2];
    }
    while (m > 0 && d->model[m - 1] == ' ') m--;
    d->model[m] = 0;
    u64 cap = (u64)d->id[200] | ((u64)d->id[201] << 8) |
              ((u64)d->id[202] << 16) | ((u64)d->id[203] << 24) |
              ((u64)d->id[204] << 32) | ((u64)d->id[205] << 40) |
              ((u64)d->id[206] << 48) | ((u64)d->id[207] << 56);
    d->sectors = cap ? (u32)cap : 0;
    return 0;
}

static void port_init(ahci_disk_t *d, u32 port_regs)
{
    d->port_regs = port_regs;
    abar_write(port_regs + 0x18, 0);
    for (u32 t = 0; t < 1000000u && (abar_read(port_regs + 0x18) & 0x8000u); t++)
        ;
    memset8(d->cl, 0, HBA_CLB_SIZE);
    memset8(d->fis, 0, HBA_FIS_SIZE);
    u64 cl = (u64)(u64)d->cl, fb = (u64)(u64)d->fis;
    abar_write(port_regs + 0x00, (u32)cl);
    abar_write(port_regs + 0x04, (u32)(cl >> 32));
    abar_write(port_regs + 0x08, (u32)fb);
    abar_write(port_regs + 0x0C, (u32)(fb >> 32));
    abar_write(port_regs + 0x14, 0);
    abar_write(port_regs + 0x18, 0x1 | 0x10);
}

int ahci_init(void)
{
    pci_dev_t dev;
    g_ndisks = 0;
    for (int i = 0; i < AHCI_MAX_DISKS; i++) g_disks[i].used = 0;
    if (pci_find_ahci(&dev) != 0) return 0;
    pci_enable(dev.bus, dev.dev, dev.func);
    if (!dev.bar5) return 0;
    g_abar = (volatile u8 *)(u64)dev.bar5;

    u32 pi = abar_read(0x0C);
    if (!pi) return 0;
    abar_write(0x04, 0x80000000u);                    /* AE */
    abar_write(0x04, abar_read(0x04) | 0x1u);          /* HR */
    for (u32 t = 0; t < 1000000u && (abar_read(0x04) & 1u); t++)
        ;

    for (int p = 0; p < 32 && g_ndisks < AHCI_MAX_DISKS; p++) {
        if (!(pi & (1u << p))) continue;
        u32 pr = 0x100u + (u32)p * 0x80u;
        u32 sig = abar_read(pr + 0x24);
        u32 ssts = abar_read(pr + 0x28);
        if ((ssts & 0x0F) != 0x03) continue;           /* no device */
        if (sig != SATA_SIG_ATA) continue;             /* 只处理 SATA 盘 */
        ahci_disk_t *d = &g_disks[g_ndisks];
        d->used = 1;
        port_init(d, pr);
        if (disk_identify(d) != 0) { serial_puts("[ahci] identify failed\n"); d->used = 0; continue; }
        serial_puts("[kernel] ahci: disk");
        {
            char b[8];
            itou((u64)g_ndisks, b, 10, 0);
            serial_puts(b);
        }
        serial_puts(" model=\"");
        serial_puts(d->model);
        serial_puts("\" sectors=");
        {
            char b[16];
            itou(d->sectors, b, 10, 0);
            serial_puts(b);
        }
        serial_puts("\n");
        g_ndisks++;
    }
    return g_ndisks;
}

int ahci_disk_count(void) { return g_ndisks; }
int ahci_present(void) { return g_ndisks > 0; }

u32 ahci_disk_sectors(int idx)
{
    return (idx >= 0 && idx < g_ndisks) ? g_disks[idx].sectors : 0;
}

const char *ahci_disk_model(int idx)
{
    return (idx >= 0 && idx < g_ndisks) ? g_disks[idx].model : "";
}

int ahci_disk_read(int idx, u32 lba, u8 count, void *buf)
{
    if (idx < 0 || idx >= g_ndisks) return -1;
    return disk_cmd(&g_disks[idx], 0x25, lba, count, buf);
}

int ahci_disk_write(int idx, u32 lba, u8 count, const void *buf)
{
    if (idx < 0 || idx >= g_ndisks) return -1;
    return disk_cmd(&g_disks[idx], 0x35, lba, count, (void *)(u64)buf);
}

int ahci_selftest(void)
{
    if (!g_ndisks) {
        serial_puts("[kernel] ahci: not found, ATA PIO fallback\n");
        return 0;
    }
    int all_ok = 1;
    for (int i = 0; i < g_ndisks; i++) {
        u8 sec[512];
        int rc = ahci_disk_read(i, 0, 1, sec);
        int ok = (rc == 0) && sec[510] == 0x55 && sec[511] == 0xAA;
        serial_puts("[kernel] ahci: disk");
        {
            char b[8];
            itou((u64)i, b, 10, 0);
            serial_puts(b);
        }
        serial_puts(ok ? " read ok\n" : " read FAILED\n");
        if (!ok) all_ok = 0;
    }

    if (g_ndisks > 1) {
        u8 tmp[512];
        /* 越界读：期望返回错误（走端口复位 + 重试路径） */
        int bad = ahci_disk_read(1, g_disks[1].sectors + 16u, 1, tmp);
        serial_puts(bad ? "[kernel] ahci: disk1 range check ok\n"
                        : "[kernel] ahci: disk1 range check FAILED\n");
        if (bad == 0) all_ok = 0;

        /* 写/读回环（写最后一扇区，不破坏引导区） */
        u8 wr[512], rd[512];
        for (int k = 0; k < 512; k++) wr[k] = (u8)(k * 7 + 3);
        int rc = ahci_disk_write(1, g_disks[1].sectors - 1u, 1, wr);
        if (rc == 0) rc = ahci_disk_read(1, g_disks[1].sectors - 1u, 1, rd);
        int same = (rc == 0);
        for (int k = 0; k < 512 && same; k++) if (wr[k] != rd[k]) same = 0;
        serial_puts(same ? "[kernel] ahci: disk1 write/read ok\n"
                         : "[kernel] ahci: disk1 write/read FAILED\n");
        if (!same) all_ok = 0;

        /* 错误恢复后仍能正常读 */
        int rc2 = ahci_disk_read(1, 0, 1, tmp);
        int ok2 = (rc2 == 0) && tmp[510] == 0x55 && tmp[511] == 0xAA;
        serial_puts(ok2 ? "[kernel] ahci: disk1 recheck ok\n"
                        : "[kernel] ahci: disk1 recheck FAILED\n");
        if (!ok2) all_ok = 0;
    }
    return all_ok ? 0 : -1;
}
