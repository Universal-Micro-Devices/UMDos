/* dev.c - 设备注册表与内置设备（P8）
 *
 * 输入设备（kbd/mouse）把 input.c 的事件流转成字节流：
 *   read() 返回 sizeof(input_event) 字节，没有事件时返回 0（非阻塞）。
 * serial / ata 分别是字符设备和块设备，用来演示设备模型和文件描述符路由。
 */
#include "dev.h"
#include "io.h"
#include "input.h"
#include "ata.h"
#include "ahci.h"

static dev_t *g_devs[DEV_MAX];
static int g_ndev;

/* ---------------- serial ---------------- */
static int ser_write(dev_t *d, const void *buf, u32 len)
{
    const char *p = (const char *)buf;
    for (u32 i = 0; i < len; i++) serial_putc(p[i]);
    d->tx += len;
    return (int)len;
}

static int ser_read(dev_t *d, void *buf, u32 len)
{
    (void)d; (void)buf; (void)len;
    return 0;
}

/* ---------------- kbd / mouse（事件化） ---------------- */
typedef struct {
    int want;               /* EV_KEY 或 EV_MOUSE */
} input_priv_t;

static input_priv_t kbd_priv = { EV_KEY };
static input_priv_t mouse_priv = { EV_MOUSE };

static int in_read(dev_t *d, void *buf, u32 len)
{
    input_priv_t *pv = (input_priv_t *)d->priv;
    input_event ev;
    if (len < sizeof(ev)) return -1;
    for (int i = 0; i < 16; i++) {
        if (!input_poll(&ev)) return 0;
        if (ev.type == pv->want) {
            for (u32 k = 0; k < sizeof(ev); k++) ((u8 *)buf)[k] = ((u8 *)&ev)[k];
            d->rx += sizeof(ev);
            return (int)sizeof(ev);
        }
    }
    return 0;
}

/* ---------------- ahci（块设备，按 512 字节扇区） ---------------- */
static dev_t g_ahci_devs[AHCI_MAX_DISKS];

static int ahci_dev_index(dev_t *d)
{
    for (int i = 0; i < AHCI_MAX_DISKS; i++)
        if (d == &g_ahci_devs[i]) return i;
    return -1;
}

static int ahci_dev_read(dev_t *d, void *buf, u32 len)
{
    int i = ahci_dev_index(d);
    if (i < 0 || len < 512) return -1;
    if (ahci_disk_read(i, 0, 1, buf) != 0) return -1;
    d->rx += 512;
    return 512;
}

static int ahci_dev_write(dev_t *d, const void *buf, u32 len)
{
    int i = ahci_dev_index(d);
    if (i < 0 || len < 512) return -1;
    if (ahci_disk_write(i, 0, 1, buf) != 0) return -1;
    d->tx += 512;
    return 512;
}

/* ---------------- ata（块设备，按 512 字节扇区） ---------------- */
static int ata_dev_read(dev_t *d, void *buf, u32 len)
{
    if (len < 512) return -1;
    if (ata_read(0, 1, buf) != 0) return -1;
    d->rx += 512;
    return 512;
}

/* ---------------- 注册表 ---------------- */
void dev_init(void)
{
    static dev_t serial, kbd, mouse, ata;
    static int done;
    if (done) return;
    done = 1;

    strcpy_s(serial.name, sizeof(serial.name), "serial");
    serial.type = DEV_CHAR;
    serial.write = ser_write;
    serial.read = ser_read;
    dev_register(&serial);

    strcpy_s(kbd.name, sizeof(kbd.name), "kbd");
    kbd.type = DEV_CHAR;
    kbd.irq = 1;
    kbd.priv = &kbd_priv;
    kbd.read = in_read;
    dev_register(&kbd);

    strcpy_s(mouse.name, sizeof(mouse.name), "mouse");
    mouse.type = DEV_CHAR;
    mouse.irq = 12;
    mouse.priv = &mouse_priv;
    mouse.read = in_read;
    dev_register(&mouse);

    for (int i = 0; i < ahci_disk_count(); i++) {
        char nm[8];
        nm[0] = 'a'; nm[1] = 'h'; nm[2] = 'c'; nm[3] = 'i';
        nm[4] = (char)('0' + i);
        nm[5] = 0;
        strcpy_s(g_ahci_devs[i].name, sizeof(g_ahci_devs[i].name), nm);
        g_ahci_devs[i].type = DEV_BLOCK;
        g_ahci_devs[i].read = ahci_dev_read;
        g_ahci_devs[i].write = ahci_dev_write;
        dev_register(&g_ahci_devs[i]);
    }

    strcpy_s(ata.name, sizeof(ata.name), "ata");
    ata.type = DEV_BLOCK;
    ata.irq = 14;
    ata.read = ata_dev_read;
    dev_register(&ata);

    serial_puts("[kernel] dev: registered serial/kbd/mouse");
    for (int i = 0; i < ahci_disk_count(); i++) {
        char b[8];
        serial_puts("/ahci");
        itou((u64)i, b, 10, 0);
        serial_puts(b);
    }
    serial_puts("/ata\n");
}

int dev_register(dev_t *d)
{
    if (!d || g_ndev >= DEV_MAX) return -1;
    g_devs[g_ndev++] = d;
    return 0;
}

dev_t *dev_find(const char *name)
{
    for (int i = 0; i < g_ndev; i++)
        if (strcmp(g_devs[i]->name, name) == 0) return g_devs[i];
    return 0;
}

dev_t *dev_at(int index)
{
    return (index >= 0 && index < g_ndev) ? g_devs[index] : 0;
}

int dev_count(void) { return g_ndev; }

u32 dev_irq_total(void)
{
    u32 n = 0;
    for (int i = 0; i < g_ndev; i++)
        if (g_devs[i]->irq) n++;
    return n;
}
