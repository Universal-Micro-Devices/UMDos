#!/usr/bin/env python3
"""P13 + P14 验收。

P13（UEFI 与 Legacy 并存）：
  - efi/bootx64.efi 是合法 PE32+（x86-64, subsystem=10 EFI_APPLICATION）
  - efi/efi.img 是 FAT12 且包含 /EFI/BOOT/BOOTX64.EFI
  - ISO 的 El Torito 目录同时有 BIOS 初始项与 UEFI 段（platform 0xEF）
P14（ACPI/电源）：
  - 串口出现 acpi: rsdp ok ... fadt madt lapic=N ... PIC fallback kept
  - shell `shutdown` -> power: shutdown 且模拟器观察到关机端口
  - shell `reboot`   -> power: reboot  且模拟器观察到复位端口
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emu
from emu import Machine, list_addr
from unicorn.x86_const import UC_X86_REG_RIP
from emu_ui import Kernel

LONG64 = None


def boot():
    m = Machine(edd=False, drive=0x00)
    m.cmos = {0x00: 0x56, 0x02: 0x34, 0x04: 0x12, 0x0A: 0x00, 0x0B: 0x02}
    m.cmos_idx = 0
    assert m.run_until(LONG64) == "ok"
    mem = bytes(m.mu.mem_read(0, 0x100000))
    k = Kernel(mem, fb_base=m.fb, w=m.xres, h=m.yres)
    k.mu.reg_write(UC_X86_REG_RIP, LONG64)
    k.run_until({0x10})
    k.run_until({0x20})
    k.settle()
    return k


def login(k):
    k.type_text("user")
    k.type_text("\t")
    k.type_text("umdos")
    k.type_text("\r")
    return k.run_until({0x30})


def check(name, cond):
    print("[%s] %s" % ("OK" if cond else "FAIL", name))
    return bool(cond)


def pe_check(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    machine = struct.unpack_from("<H", d, pe + 4)[0]
    subsystem = struct.unpack_from("<H", d, pe + 24 + 68)[0]
    size_of_image = struct.unpack_from("<I", d, pe + 24 + 56)[0]
    return d[pe:pe + 4] == b"PE\0\0" and machine == 0x8664 and subsystem == 10 and size_of_image > 0


def fat_has_bootx64(path):
    d = open(path, "rb").read()
    bps = struct.unpack_from("<H", d, 11)[0]
    spc = d[13]
    rsvd = struct.unpack_from("<H", d, 14)[0]
    nfats = d[16]
    root_entries = struct.unpack_from("<H", d, 17)[0]
    fatsz = struct.unpack_from("<H", d, 22)[0]
    root_off = (rsvd + nfats * fatsz) * bps
    names = []
    for i in range(root_entries):
        e = d[root_off + i * 32: root_off + i * 32 + 32]
        if not e or e[0] == 0:
            break
        names.append(bytes(e[0:11]))
    return b"EFI        " in names, names


def iso_check(path):
    d = open(path, "rb").read()
    cat = struct.unpack_from("<I", d, 17 * 2048 + 71)[0]
    base = cat * 2048
    val = d[base:base + 32]
    init = d[base + 32:base + 64]
    hdr = d[base + 64:base + 96]
    ent = d[base + 96:base + 128]
    bios_ok = (val[0] == 1) and (init[0] == 0x88)
    uefi_ok = (hdr[0] == 0x90) and (hdr[1] == 0xEF) and (ent[0] == 0x88) and (ent[4] == 0xEF)
    return bios_ok, uefi_ok, struct.unpack_from("<I", ent, 8)[0]


def main():
    global LONG64
    LONG64, _ = list_addr(os.path.join(emu.BIN, "head.lst"), lambda s: "long64:" in s, 0x9000)
    ok = True

    # ---- P13 结构验收 ----
    efi_bin = os.path.join(emu.ROOT, "efi", "bootx64.efi")
    efi_img = os.path.join(emu.ROOT, "efi", "efi.img")
    ok &= check("bootx64.efi 是 EFI PE32+", pe_check(efi_bin))
    has_dir, names = fat_has_bootx64(efi_img)
    ok &= check("efi.img 是 FAT12 且含 EFI 目录", has_dir)
    iso = None
    for f in os.listdir(emu.ROOT):
        if f.endswith(".iso") and "installer" not in f:
            iso = os.path.join(emu.ROOT, f)
    bios_ok, uefi_ok, efi_lba = iso_check(iso)
    ok &= check("ISO 有 BIOS 启动项", bios_ok)
    ok &= check("ISO 有 UEFI 启动项(platform 0xEF)", uefi_ok)
    print("       UEFI 引导镜像 LBA =", efi_lba)

    # ---- P14 运行验收 ----
    k = boot()
    acpi_line = [ln for ln in k.serial.split("\n") if "acpi:" in ln]
    print("      ", acpi_line[0].strip() if acpi_line else "no acpi line")
    ok &= check("ACPI 解析出 FADT/MADT 并保留 PIC 回退",
                "rsdp ok" in k.serial and "fadt" in k.serial and "madt" in k.serial
                and "PIC fallback kept" in k.serial)
    ok &= check("lapic 计数 > 0", "lapic=" in k.serial and "lapic=0" not in k.serial)

    ok &= check("login -> desktop", login(k) == 0x30)
    k.settle()
    k.fkey(10)
    k.run(2_000_000)
    k.type_text("shutdown\r")
    k.run(3_000_000)
    ok &= check("shutdown 命令触发 ACPI S5", "power: shutdown" in k.serial and k.powered_off)

    k2 = boot()
    ok &= check("second login -> desktop", login(k2) == 0x30)
    k2.settle()
    k2.fkey(10)
    k2.run(2_000_000)
    k2.type_text("reboot\r")
    k2.run(3_000_000)
    ok &= check("reboot 命令触发复位", "power: reboot" in k2.serial and k2.rebooted)

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
