/* pci.h - PCI 配置空间枚举（P16） */
#ifndef PCI_H
#define PCI_H

#include "types.h"

typedef struct {
    u16 vendor;
    u16 device;
    u8  bus, dev, func;
    u32 bar5;
    u8  irq;
} pci_dev_t;

u32  pci_read32(u8 bus, u8 dev, u8 func, u8 off);
void pci_write32(u8 bus, u8 dev, u8 func, u8 off, u32 v);
int  pci_find_class(u8 cls, u8 sub, u8 prog, pci_dev_t *out);
int  pci_find_ahci(pci_dev_t *out);
void pci_enable(u8 bus, u8 dev, u8 func);

#endif
