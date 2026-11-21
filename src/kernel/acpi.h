/* acpi.h - ACPI 表解析与电源管理（P14） */
#ifndef ACPI_H
#define ACPI_H

#include "types.h"

typedef struct {
    int  rsdp_ok;
    int  fadt_ok;
    int  madt_ok;
    int  xsdt;
    u32  tables;
    u32  lapic_count;
    u16  pm1a_cnt;
    u16  pm1b_cnt;
    u16  reset_port;
    u8   reset_value;
    u8   slp_typa;
    char oem[8];
} acpi_info_t;

const acpi_info_t *acpi_init(void);
const acpi_info_t *acpi_info(void);
u32 acpi_lapic_ids(u32 *out, u32 cap);

int acpi_shutdown(void);          /* 0 = 已发出关机命令 */
int acpi_reboot(void);            /* 0 = 已发出复位命令 */

#endif
