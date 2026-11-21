#!/usr/bin/env python3
"""构建 UMDos 安装引导（dev.1 骨架）。

与 build.py 的关系：
  - 不改动 build.py 主体，也不修改任何现有内核源文件；
  - 复用现有的 FirstMain / NextMain 两个汇编阶段（原样汇编，不改源码），
    安装引导只在 LBA0 放一个自己的引导扇区，并把安装程序放进 LBA50 起的内核位置。
  - 产物：bin/installer/installer.img（1.44MB 软盘镜像，供模拟器测试）
          releases/<版本>/UMDos installer <版本>.iso（El Torito 启动 ISO）
"""
import hashlib
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INST = os.path.join(ROOT, "installer")
ISRC = os.path.join(INST, "src")
BIN = os.path.join(ROOT, "bin", "installer")
RELEASES = ROOT   # dev.N 版本目录内构建，镜像就放本目录

FLOPPY_SIZE = 1474560
KERNEL_LBA = 50
KERNEL_SECTORS = 128
FIRSTMAIN_LBA = 30
FIRSTMAIN_SECTORS = 16
NEXTMAIN_LBA = 46
NEXTMAIN_SECTORS = 4

ZIG = [sys.executable, "-m", "ziglang", "cc"]
CFLAGS = ["-target", "x86_64-freestanding", "-ffreestanding", "-fno-stack-protector",
          "-fno-pic", "-mno-red-zone", "-mno-sse", "-mno-mmx", "-mno-80387",
          "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
          "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
          "-I", os.path.join(ROOT, "assets", "icons", "generated")]

C_FILES = ["libk.c", "gfx.c", "input.c", "ata.c", "sha3.c", "fat32.c", "main.c"]


def sh(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if p.returncode != 0:
        raise SystemExit("FAILED: %s\n%s\n%s" % (" ".join(cmd), p.stdout, p.stderr))
    return p.stdout


def read_version():
    v = open(os.path.join(ROOT, "VERSION"), encoding="utf-8").read().strip()
    if not v:
        raise SystemExit("VERSION is empty")
    return v


def gen_version_h(version):
    path = os.path.join(ISRC, "version.h")
    open(path, "w", encoding="utf-8", newline="\n").write(
        "/* 自动生成，请勿手改：来源是项目根目录 VERSION */\n"
        "#ifndef INSTALLER_VERSION_H\n#define INSTALLER_VERSION_H\n\n"
        '#define INSTALLER_VERSION "%s"\n\n#endif\n' % version)
    return path


def build_bin(version):
    os.makedirs(BIN, exist_ok=True)
    gen_version_h(version)

    # 1) 安装引导自己的引导扇区
    boot = os.path.join(BIN, "boot.bin")
    sh(["nasm", "-f", "bin", os.path.join(INST, "boot.asm"), "-o", boot])
    if os.path.getsize(boot) != 512:
        raise SystemExit("installer boot sector is not 512 bytes")

    # 2) 复用现有 FirstMain / NextMain：原样汇编，不改源码
    inc = os.path.join(ROOT, "src", "inc") + os.sep
    fm = os.path.join(BIN, "firstmain.bin")
    sh(["nasm", "-f", "bin", "-I", inc, "-l", fm + ".lst", os.path.join(ROOT, "src", "firstmain.asm"), "-o", fm])
    if os.path.getsize(fm) != FIRSTMAIN_SECTORS * 512:
        raise SystemExit("firstmain size mismatch")
    nm = os.path.join(BIN, "nextmain.bin")
    sh(["nasm", "-f", "bin", "-I", inc, "-l", nm + ".lst", os.path.join(ROOT, "src", "nextmain.asm"), "-o", nm])
    if os.path.getsize(nm) != NEXTMAIN_SECTORS * 512:
        raise SystemExit("nextmain size mismatch")

    # 3) C 部分
    obj = os.path.join(BIN, "obj")
    os.makedirs(obj, exist_ok=True)
    objs = []
    o = os.path.join(obj, "start.o")
    sh(["nasm", "-f", "elf64", os.path.join(ISRC, "start.asm"), "-o", o])
    objs.append(o)
    for c in C_FILES:
        o = os.path.join(obj, c[:-2] + ".o")
        sh(ZIG + CFLAGS + ["-c", os.path.join(ISRC, c), "-o", o])
        objs.append(o)

    elf = os.path.join(BIN, "installer.elf")
    sh(ZIG + ["-target", "x86_64-freestanding", "-nostdlib",
              "-Wl,-T," + os.path.join(ISRC, "link.ld"), "-o", elf] + objs)
    flat = os.path.join(BIN, "installer.bin")
    sh([sys.executable, os.path.join(ROOT, "tools", "elf2bin.py"), elf, flat])

    data = open(flat, "rb").read()
    need = KERNEL_SECTORS * 512
    if len(data) > need:
        raise SystemExit("installer too big: %d > %d" % (len(data), need))
    open(flat, "wb").write(data + b"\x00" * (need - len(data)))
    print("  installer.bin %6d / %d bytes (LBA %d @ 0x10000)" % (len(data), need, KERNEL_LBA))

    # 4) 1.44MB 镜像
    img = bytearray(FLOPPY_SIZE)
    img[0:512] = open(boot, "rb").read()
    img[FIRSTMAIN_LBA * 512:FIRSTMAIN_LBA * 512 + FIRSTMAIN_SECTORS * 512] = open(fm, "rb").read()
    img[NEXTMAIN_LBA * 512:NEXTMAIN_LBA * 512 + NEXTMAIN_SECTORS * 512] = open(nm, "rb").read()
    img[KERNEL_LBA * 512:KERNEL_LBA * 512 + need] = open(flat, "rb").read()
    img_path = os.path.join(BIN, "installer.img")
    open(img_path, "wb").write(img)
    print("  installer.img %d bytes sha256=%s" % (len(img), hashlib.sha256(img).hexdigest()[:16]))
    return img_path, img


README_ON_ISO = """UMDos installer %s

This CD boots the UMDos installation wizard (Legacy BIOS only in dev.1).
It is an installer-only temporary image; the complete public ISO comes after P13.

Usage (QEMU):
  qemu-img create -f raw disk.img 64M
  qemu-system-x86_64 -m 512M -cdrom "UMDos installer %s.iso" -hda disk.img -boot d

The wizard writes an MBR partition entry, a FAT32 volume at LBA 2048 and
/config/auth (sha3-256$salt$hash, no plaintext). EXT32 is experimental in dev.1
and only writes a marker.

Sources and build scripts are in the project folder, not on this CD.
"""


def build_iso(version, img_path, img_bytes, out_dir):
    import pycdlib
    os.makedirs(out_dir, exist_ok=True)
    readme = os.path.join(BIN, "README.TXT")
    open(readme, "w", encoding="ascii", newline="\r\n").write(
        README_ON_ISO % (version, version))

    iso_path = os.path.join(out_dir, "UMDos installer %s.iso" % version)
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=1, sys_ident="UMDOS", vol_ident="UMDOSINST",
            vol_set_ident=" ", pub_ident_str="UMDOS", preparer_ident_str="UMDOS",
            app_ident_str="UMDOS INSTALLER CD")
    iso.add_file(readme, iso_path="/README.TXT;1")
    iso.add_file(img_path, iso_path="/INSTALL.IMG;1")
    iso.add_eltorito("/INSTALL.IMG;1", bootcatfile="/BOOT.CAT;1",
                     media_name="floppy", boot_load_size=2880, platform_id=0)
    iso.write(iso_path)
    iso.close()

    data = open(iso_path, "rb").read()
    print("  %s: %d bytes sha256=%s" % (os.path.basename(iso_path), len(data),
                                        hashlib.sha256(data).hexdigest()[:16]))
    return iso_path


def main():
    version = read_version()
    print("== UMDos installer %s ==" % version)
    img_path, img_bytes = build_bin(version)
    out_dir = RELEASES
    iso = build_iso(version, img_path, img_bytes, out_dir)
    print("done ->", iso)
    return 0


if __name__ == "__main__":
    sys.exit(main())
