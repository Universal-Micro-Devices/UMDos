#!/usr/bin/env python3
"""构建 UMDos：汇编 6 个阶段 -> 1.44MB 软盘镜像 -> 16MB 硬盘镜像 -> El Torito 启动 ISO。

依赖：nasm、python3、pycdlib（pip install pycdlib）
"""
import os, subprocess, sys, struct, hashlib

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "src")
INC = os.path.join(SRC, "inc")
BIN = os.path.join(ROOT, "bin")
VERSION = open(os.path.join(ROOT, "VERSION"), encoding="utf-8").read().strip()
IMG = os.path.join(ROOT, "UMDos %s floppy.img" % VERSION)
HDD = os.path.join(ROOT, "UMDos %s hdd.img" % VERSION)
ISO = os.path.join(ROOT, "UMDos %s.iso" % VERSION)

# 阶段: 名字, 源文件, 扇区数, 起始 LBA, 加载地址
# P2 四级链：boot -> loader -> head -> kernel
STAGES = [
    ("boot",   "boot.asm",   1, 0, 0x7C00),
    ("loader", "loader.asm", 8, 1, 0x7E00),
    ("head",   "head.asm",   4, 9, 0x9000),
]
FLOPPY_SIZE = 1474560          # 1.44MB
HDD_SIZE = 16 * 1024 * 1024    # 16MB

# C 内核与文件系统镜像
KERNEL_LBA = 50
KERNEL_SECTORS = 320           # 96KB（覆盖 .bss，见 P2）
FS_LBA = 512
FS_SECTORS = 256               # 128KB
KERNEL_SRC = os.path.join(SRC, "kernel")
KERNEL_C = ["libk.c", "kmain.c", "gfx.c", "input.c", "idt.c", "irq.c", "mem.c",
            "gdt.c", "task.c", "syscall.c", "user.c", "vfs.c", "dev.c", "proc.c", "percpu.c", "smp.c", "pci.c", "ahci.c", "term.c", "sh.c", "sec.c", "acpi.c",
            "sha256.c", "sha3.c", "auth.c", "fs.c", "ata.c", "ui.c"]
ZIG = [sys.executable, "-m", "ziglang", "cc"]
CFLAGS = ["-target", "x86_64-freestanding", "-ffreestanding", "-fno-stack-protector",
          "-fno-pic", "-mno-red-zone", "-mgeneral-regs-only",
          "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
          "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter"]


def sh(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if p.returncode != 0:
        raise SystemExit("FAILED: %s\n%s\n%s" % (" ".join(cmd), p.stdout, p.stderr))
    return p.stdout


def build_bins():
    os.makedirs(BIN, exist_ok=True)
    for name, src, secs, lba, addr in STAGES:
        out = os.path.join(BIN, name + ".bin")
        lst = os.path.join(BIN, name + ".lst")
        sh(["nasm", "-f", "bin", "-I", INC + os.sep, "-l", lst,
            os.path.join(SRC, src), "-o", out])
        size = os.path.getsize(out)
        need = secs * 512
        status = "OK" if size == need else "SIZE MISMATCH"
        print(f"  {name:10s} {size:6d} / {need:6d} bytes   LBA {lba:<3d} @ 0x{addr:04X}   {status}")
        if size != need:
            raise SystemExit(f"{name}: expected {need} bytes, got {size}")


def layout(buf):
    for name, src, secs, lba, addr in STAGES:
        data = open(os.path.join(BIN, name + ".bin"), "rb").read()
        off = lba * 512
        buf[off:off + len(data)] = data
    for name, lba in (("kernel.bin", KERNEL_LBA), ("fs.img", FS_LBA)):
        path = os.path.join(BIN, name)
        if os.path.exists(path):
            data = open(path, "rb").read()
            off = lba * 512
            buf[off:off + len(data)] = data


def build_user():
    """编译用户态演示程序（P6）：ELF64，链接到 USER_BASE，然后嵌入内核。"""
    out = os.path.join(BIN, "user.elf")
    sh(ZIG + ["-target", "x86_64-freestanding", "-ffreestanding", "-nostdlib",
              "-fno-stack-protector", "-fno-pic", "-mcmodel=large", "-mno-red-zone",
              "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-O2",
              "-Wl,-T," + os.path.join(ROOT, "user", "link.ld"),
              "-o", out,
              os.path.join(ROOT, "user", "libc.c"),
              os.path.join(ROOT, "user", "prog.c")])
    hdr = os.path.join(KERNEL_SRC, "userprog.h")
    sh([sys.executable, os.path.join(ROOT, "tools", "gen_userprog.py"), out, hdr])


def build_smp():
    """P15：汇编 AP 跳板并嵌入内核。"""
    out = os.path.join(BIN, "smp.tramp")
    sh(["nasm", "-f", "bin", os.path.join(KERNEL_SRC, "smp_tramp.asm"), "-o", out])
    hdr = os.path.join(KERNEL_SRC, "smptramp.h")
    sh([sys.executable, os.path.join(ROOT, "tools", "gen_smptramp.py"), out, hdr])


def build_kernel():
    """编译 C 内核（Zig/Clang，freestanding x86_64），链接到 0x10000 并转成平铺二进制。"""
    build_user()
    build_smp()
    obj = os.path.join(BIN, "kobj")
    os.makedirs(obj, exist_ok=True)
    objs = []
    o = os.path.join(obj, "start.o")
    sh(["nasm", "-f", "elf64", os.path.join(KERNEL_SRC, "start.asm"), "-o", o])
    objs.append(o)
    o = os.path.join(obj, "isr.o")
    sh(["nasm", "-f", "elf64", os.path.join(KERNEL_SRC, "isr.asm"), "-o", o])
    objs.append(o)
    o = os.path.join(obj, "switch.o")
    sh(["nasm", "-f", "elf64", os.path.join(KERNEL_SRC, "switch.asm"), "-o", o])
    objs.append(o)

    for c in KERNEL_C:
        path = os.path.join(KERNEL_SRC, c)
        if not os.path.exists(path):
            continue
        o = os.path.join(obj, c[:-2] + ".o")
        sh(ZIG + CFLAGS + ["-c", path, "-o", o])
        objs.append(o)

    elf = os.path.join(BIN, "kernel.elf")
    sh(ZIG + ["-target", "x86_64-freestanding", "-nostdlib",
              "-Wl,-T," + os.path.join(KERNEL_SRC, "link.ld"), "-o", elf] + objs)
    out = os.path.join(BIN, "kernel.bin")
    sh([sys.executable, os.path.join(ROOT, "tools", "elf2bin.py"), elf, out])

    data = open(out, "rb").read()
    need = KERNEL_SECTORS * 512
    if len(data) > need:
        raise SystemExit(f"kernel too big: {len(data)} > {need}")
    open(out, "wb").write(data + b"\x00" * (need - len(data)))
    print(f"  kernel.bin {len(data):6d} / {need} bytes   LBA {KERNEL_LBA} @ 0x10000   OK")


def build_fs():
    """生成 UMDFS 文件系统镜像（128KB），放在分区/固定 LBA。"""
    import fsgen
    data = fsgen.build(FS_SECTORS * 512)
    open(os.path.join(BIN, "fs.img"), "wb").write(data)
    print(f"  fs.img    {len(data):6d} bytes   LBA {FS_LBA}   "
          f"{fsgen.FILE_COUNT} files")


def build_img():
    img = bytearray(FLOPPY_SIZE)
    layout(img)
    open(IMG, "wb").write(img)
    print(f"  {os.path.basename(IMG)}: {len(img)} bytes, sha256={hashlib.sha256(img).hexdigest()[:16]}")


def build_hdd():
    """16MB 硬盘镜像：MBR 里写一个有效的活动分区表项，便于 BIOS/虚拟机把它当硬盘。"""
    img = bytearray(HDD_SIZE)
    layout(img)
    assert img[446:510] == b"\x00" * 64, "引导代码已经占到分区表位置"
    start_lba, sectors = FS_LBA, 2048
    part = bytearray(16)
    part[0] = 0x80                                  # 活动
    part[1:4] = bytes([0, 1, 1])                    # 起始 CHS（LBA 寻址时仅供参考）
    part[4] = 0x0C                                  # FAT32 LBA
    part[5:8] = bytes([254, 255, 255])
    struct.pack_into("<I", part, 8, start_lba)
    struct.pack_into("<I", part, 12, sectors)
    img[446:462] = part
    img[510:512] = b"\x55\xAA"
    open(HDD, "wb").write(img)
    print(f"  {os.path.basename(HDD)}: {len(img)} bytes, sha256={hashlib.sha256(img).hexdigest()[:16]}")


README_FOR_ISO = """UMDos - 16/64 bit boot chain demo

This CD uses El Torito 1.44MB floppy emulation.
The boot image (UMDOS.IMG) is also in the root of this ISO.

First boot asks for:
  - username / password (your choice)
  - activation code: UMDOS2024

Sources and build script are in the project folder, not on this CD.
"""


def build_efi():
    """P13：构建 UEFI 加载器与 FAT ESP 镜像。"""
    efi_dir = os.path.join(ROOT, "efi")
    embed = os.path.join(efi_dir, "efi_embed.h")
    sh([sys.executable, os.path.join(efi_dir, "gen_embed.py"),
        os.path.join(BIN, "kernel.bin"), os.path.join(BIN, "fs.img"), embed])
    out = os.path.join(efi_dir, "bootx64.efi")
    sh(ZIG + ["-target", "x86_64-uefi", "-nostdlib", "-fno-stack-protector",
              "-fno-builtin", "-fno-sanitize=all", "-mno-stack-arg-probe",
              "-I", efi_dir, "-o", out, os.path.join(efi_dir, "efi_loader.c")])
    img = os.path.join(efi_dir, "efi.img")
    sh([sys.executable, os.path.join(efi_dir, "mkfat.py"), out, img])
    print("  bootx64.efi + efi.img (FAT12) ready")


def build_iso():
    import pycdlib
    readme = os.path.join(BIN, "README.TXT")
    with open(readme, "w", encoding="ascii", newline="\r\n") as f:
        f.write(README_FOR_ISO)

    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=1, sys_ident="UMDOS", vol_ident="UMDOS",
            vol_set_ident=" ", pub_ident_str="UMDOS", preparer_ident_str="UMDOS",
            app_ident_str="UMDOS BOOTABLE CD")
    iso.add_file(readme, iso_path="/README.TXT;1")
    iso.add_file(IMG, iso_path="/UMDOS.IMG;1")
    iso.add_eltorito("/UMDOS.IMG;1", bootcatfile="/BOOT.CAT;1",
                     media_name="floppy", boot_load_size=2880, platform_id=0)
    # P13：UEFI 启动项用 FAT ESP 镜像，写在 /EFI.IMG;1
    efi_img_path = os.path.join(ROOT, "efi", "efi.img")
    if os.path.exists(efi_img_path):
        iso.add_file(efi_img_path, iso_path="/EFI.IMG;1")
        rec = iso.get_record(iso_path="/EFI.IMG;1")
        efi_lba = rec.extent_location()
        efi_sectors = os.path.getsize(efi_img_path) // 512
    else:
        efi_lba = 0
        efi_sectors = 0
    iso.write(ISO)
    iso.close()

    # 在 Boot Catalog 里追加 UEFI 段（platform 0xEF）：段头 0x90 + 条目 0x88
    if efi_lba:
        data = bytearray(open(ISO, "rb").read())
        cat_lba = struct.unpack_from("<I", data, 17 * 2048 + 71)[0]
        off = cat_lba * 2048 + 64
        data[off:off + 4] = b"\x90\xEF\x00\x00"
        off += 32
        data[off + 0] = 0x88          # bootable entry
        data[off + 1] = 0x00          # no emulation
        struct.pack_into("<H", data, off + 2, 0)
        data[off + 4] = 0xEF          # system type = UEFI
        data[off + 5] = 0
        struct.pack_into("<H", data, off + 6, efi_sectors)
        struct.pack_into("<I", data, off + 8, efi_lba)
        open(ISO, "wb").write(data)

    data = open(ISO, "rb").read()
    cat_lba = struct.unpack_from("<I", data, 17 * 2048 + 71)[0]
    boot_lba = struct.unpack_from("<I", data, cat_lba * 2048 + 32 + 8)[0]
    print(f"  {os.path.basename(ISO)}: {len(data)} bytes, boot image @ LBA {boot_lba}, "
          f"catalog @ {cat_lba}, sha256={hashlib.sha256(data).hexdigest()[:16]}")


if __name__ == "__main__":
    print("== assembling ==")
    build_bins()
    print("== kernel (C) ==")
    build_kernel()
    print("== filesystem image ==")
    build_fs()
    print("== floppy image ==")
    build_img()
    print("== hard disk image ==")
    build_hdd()
    print("== efi (P13) ==")
    build_efi()
    print("== iso ==")
    build_iso()
    print("done")
