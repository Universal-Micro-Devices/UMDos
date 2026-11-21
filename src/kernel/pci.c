/* pci.c - PCI 配置空间（0xCF8/0xCFC）枚举（P16） */
#include "pci.h"
#include "io.h"

u32 pci_read32(u8 bus, u8 dev, u8 func, u8 off)
{
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)(dev & 0x1F) << 11) |
               ((u32)(func & 0x07) << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

void pci_write32(u8 bus, u8 dev, u8 func, u8 off, u32 v)
{
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)(dev & 0x1F) << 11) |
               ((u32)(func & 0x07) << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, v);
}

int pci_find_class(u8 cls, u8 sub, u8 prog, pci_dev_t *out)
{
    for (int dev = 0; dev < 32; dev++) {
        for (int func = 0; func < 8; func++) {
            u32 id = pci_read32(0, (u8)dev, (u8)func, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            u32 cc = pci_read32(0, (u8)dev, (u8)func, 0x08);
            u8 c = (u8)(cc >> 24), s = (u8)(cc >> 16), p = (u8)(cc >> 8);
            if (c == cls && s == sub && (prog == 0xFF || p == prog)) {
                out->vendor = (u16)(id & 0xFFFF);
                out->device = (u16)(id >> 16);
                out->bus = 0;
                out->dev = (u8)dev;
                out->func = (u8)func;
                out->bar5 = pci_read32(0, (u8)dev, (u8)func, 0x24) & ~0xFu;
                out->irq = (u8)(pci_read32(0, (u8)dev, (u8)func, 0x3C) & 0xFF);
                return 0;
            }
        }
    }
    return -1;
}

int pci_find_ahci(pci_dev_t *out)
{
    /* class 0x01 (mass storage) subclass 0x06 (SATA) prog-if 0x01 (AHCI) */
    return pci_find_class(0x01, 0x06, 0x01, out);
}

void pci_enable(u8 bus, u8 dev, u8 func)
{
    u32 cmd = pci_read32(bus, dev, func, 0x04);
    cmd |= 0x0002u | 0x0004u;        /* memory space + bus master */
    pci_write32(bus, dev, func, 0x04, cmd);
}
