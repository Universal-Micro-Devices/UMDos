/* acpi.c - RSDP/XSDT/RSDT/FADT/MADT 解析与关机/重启（P14）
 *
 * 关机：FADT 的 PM1a_CNT 写入 SLP_TYPa|SLP_EN；没有 FADT 时退回
 *       QEMU 的 0x604 / 旧 Bochs 的 0xB004。
 * 重启：优先 FADT 的 RESET_REG（ACPI 2.0+），否则用键盘控制器 0x64<-0xFE。
 */
#include "acpi.h"
#include "io.h"

static acpi_info_t g_acpi;

typedef struct {
    u8  sig[8];
    u8  csum;
    u8  oem[6];
    u8  rev;
    u32 rsdt;
    u32 len;
    u64 xsdt;
    u8  xcsum;
    u8  res[3];
} __attribute__((packed)) rsdp_t;

typedef struct {
    u8  sig[4];
    u32 len;
    u8  rev;
    u8  csum;
    u8  oem[6];
    u8  oem_tbl[8];
    u32 oem_rev;
    u32 creator;
    u32 creator_rev;
} __attribute__((packed)) sdt_t;

typedef struct {
    sdt_t h;
    u32 firmware_ctrl;
    u32 dsdt;
    u8  res0;
    u8  preferred_pm;
    u16 sci_int;
    u32 smi_cmd;
    u8  acpi_enable;
    u8  acpi_disable;
    u8  s4bios_req;
    u8  pstate_cnt;
    u32 pm1a_evt_blk;
    u32 pm1b_evt_blk;
    u32 pm1a_cnt_blk;
    u32 pm1b_cnt_blk;
    u32 pm2_cnt_blk;
    u32 pm_tmr_blk;
    u32 gpe0_blk;
    u32 gpe1_blk;
    u8  pm1_evt_len;
    u8  pm1_cnt_len;
    u8  pm2_cnt_len;
    u8  pm_tmr_len;
    u8  gpe0_len;
    u8  gpe1_len;
    u8  gpe1_base;
    u8  cst_cnt;
    u16 p_lvl2_lat;
    u16 p_lvl3_lat;
    u16 flush_size;
    u16 duty_offset;
    u16 duty_width;
    u8  day_alrm;
    u8  mon_alrm;
    u8  century;
    u16 iapc_boot_arch;
    u8  res1;
    u32 flags;
    /* ACPI 2.0 起： */
    u8  res2[12];
    u64 x_firmware_ctrl;
    u64 x_dsdt;
    u8  x_pm1a_evt[12];
    u8  x_pm1b_evt[12];
    u8  x_pm1a_cnt[12];
    u8  x_pm1b_cnt[12];
} __attribute__((packed)) fadt_t;

static u8 sum8(const u8 *p, u32 n)
{
    u8 s = 0;
    for (u32 i = 0; i < n; i++) s = (u8)(s + p[i]);
    return s;
}

static const u8 *g_root;          /* XSDT 或 RSDT */
static int g_root_is_xsdt;
static u32 g_root_count;

static const sdt_t *root_entry(u32 i)
{
    if (!g_root) return 0;
    if (g_root_is_xsdt) {
        u64 p = 0;
        for (int k = 0; k < 8; k++) p |= (u64)((const u8 *)g_root)[36 + i * 8 + k] << (k * 8);
        return (const sdt_t *)(u64)p;
    }
    u32 p = 0;
    for (int k = 0; k < 4; k++) p |= (u32)((const u8 *)g_root)[36 + i * 4 + k] << (k * 8);
    return (const sdt_t *)(u64)p;
}

static int sig4(const u8 *p, const char *s)
{
    for (int i = 0; i < 4; i++)
        if (p[i] != (u8)s[i]) return 0;
    return 1;
}

const acpi_info_t *acpi_info(void) { return &g_acpi; }

const acpi_info_t *acpi_init(void)
{
    for (u32 i = 0; i < sizeof(g_acpi); i++) ((u8 *)&g_acpi)[i] = 0;
    g_acpi.slp_typa = 5;
    g_root = 0;
    g_root_count = 0;

    /* 1) 在 0xE0000..0xFFFFF 里找 "RSD PTR " */
    rsdp_t *rsdp = 0;
    for (u32 addr = 0xE0000; addr < 0x100000; addr += 16) {
        const u8 *p = (const u8 *)(u64)addr;
        if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' &&
            p[4] == 'P' && p[5] == 'T' && p[6] == 'R' && p[7] == ' ') {
            rsdp = (rsdp_t *)(u64)addr;
            break;
        }
    }
    if (!rsdp) {
        serial_puts("[kernel] acpi: rsdp not found (PIC fallback stays)\n");
        return &g_acpi;
    }
    g_acpi.rsdp_ok = 1;
    for (int i = 0; i < 6; i++) g_acpi.oem[i] = rsdp->oem[i];

    /* 2) 优先 XSDT（ACPI 2.0+），否则 RSDT */
    if (rsdp->xsdt && rsdp->rev >= 2) {
        g_root = (const u8 *)(u64)rsdp->xsdt;
        g_root_is_xsdt = 1;
        g_acpi.xsdt = 1;
    } else if (rsdp->rsdt) {
        g_root = (const u8 *)(u64)rsdp->rsdt;
        g_root_is_xsdt = 0;
    }
    if (g_root) {
        const sdt_t *h = (const sdt_t *)g_root;
        if (sig4(h->sig, "XSDT") || sig4(h->sig, "RSDT"))
            g_root_count = (h->len - 36) / (g_root_is_xsdt ? 8 : 4);
    }
    g_acpi.tables = g_root_count;

    /* 3) 遍历找 FADT / MADT */
    for (u32 i = 0; i < g_root_count && i < 32; i++) {
        const sdt_t *t = root_entry(i);
        if (!t) continue;
        if (sig4(t->sig, "FACP")) {
            const fadt_t *f = (const fadt_t *)t;
            g_acpi.fadt_ok = 1;
            g_acpi.pm1a_cnt = (u16)f->pm1a_cnt_blk;
            g_acpi.pm1b_cnt = (u16)f->pm1b_cnt_blk;
            /* ACPI 2.0+ 的 RESET_REG 在扩展区，这里用常见布局兜底：
             * flags bit10 (RESET_REG_SUP) 时 RESET_REG 在 FADT + 0x74 起 */
            const u8 *raw = (const u8 *)f;
            if (f->h.len > 0x74 && (f->flags & (1u << 10))) {
                u32 gas_addr = 0;
                u8 space = raw[0x74 + 0];
                for (int k = 0; k < 4; k++) gas_addr |= (u32)raw[0x74 + 4 + k] << (k * 8);
                if (space == 1 && gas_addr) {
                    g_acpi.reset_port = (u16)gas_addr;
                    g_acpi.reset_value = raw[0x74 + 12];
                }
            }
        } else if (sig4(t->sig, "APIC")) {
            g_acpi.madt_ok = 1;
            /* MADT 头部之后是记录序列，统计类型 0（LAPIC） */
            const u8 *p = (const u8 *)t + 44;
            const u8 *end = (const u8 *)t + t->len;
            while (p + 2 <= end && p[1] >= 2) {
                if (p[0] == 0) g_acpi.lapic_count++;
                p += p[1];
            }
        }
    }

    serial_puts("[kernel] acpi: rsdp ok");
    if (g_acpi.xsdt) serial_puts(" xsdt");
    serial_puts(" tables=");
    {
        char b[16];
        itou(g_acpi.tables, b, 10, 0);
        serial_puts(b);
    }
    serial_puts(g_acpi.fadt_ok ? " fadt" : "");
    serial_puts(g_acpi.madt_ok ? " madt" : "");
    serial_puts(" lapic=");
    {
        char b[16];
        itou(g_acpi.lapic_count, b, 10, 0);
        serial_puts(b);
    }
    serial_puts(" PIC fallback kept\n");
    return &g_acpi;
}

int acpi_shutdown(void)
{
    serial_puts("[kernel] power: shutdown\n");
    if (g_acpi.fadt_ok && g_acpi.pm1a_cnt) {
        u16 v = (u16)((g_acpi.slp_typa << 10) | (1u << 13));
        outw(g_acpi.pm1a_cnt, v);
        if (g_acpi.pm1b_cnt) outw(g_acpi.pm1b_cnt, v);
    }
    /* QEMU / 旧 Bochs 的 ACPI 关机器 */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    cli();
    for (;;) hlt();
}

int acpi_reboot(void)
{
    serial_puts("[kernel] power: reboot\n");
    if (g_acpi.reset_port) {
        outb(g_acpi.reset_port, g_acpi.reset_value ? g_acpi.reset_value : 0x06);
    }
    /* 键盘控制器复位 */
    for (volatile int i = 0; i < 100000; i++)
        ;
    outb(0x64, 0xFE);
    cli();
    for (;;) hlt();
}
