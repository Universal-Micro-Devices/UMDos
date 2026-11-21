#!/usr/bin/env python3
"""生成 UMDFS 文件系统镜像（带目录树）。

布局（块大小 512）：
  block 0       超级块
  block 1..4    目录项表（32 项 × 64 字节）
  block 5       块位图
  block 8..     数据区

目录项：name[32] size u32 first u32 used u8 type u8 pad[2] parent u32
  type: 0=文件 1=目录；parent = 0xFFFFFFFF 表示根目录
"""
import hashlib
import os
import secrets
import struct

MAGIC = b"UMDFS1\x00\x00"
MAGIC2 = 0x46533032          # "FS02"（带校验和）
BLOCK = 512
DATA_START = 8
DIRENT_BLOCK = 1
DIRENT_COUNT = 32
BITMAP_BLOCK = 5
ROOT = 0xFFFFFFFF

ROOT_FILES = [
    ("README.TXT",
     "UMDos 64-bit kernel\n"
     "===================\n"
     "\n"
     "This file lives in a small UMDFS filesystem that the\n"
     "kernel mounts from the disk image.\n"
     "\n"
     "Directories: DOCS and SYSTEM.\n"
     "Open them with a double click.\n"),
    ("MOTD.TXT",
     "A bosom friend afar brings a distant land near.\n"
     "\n"
     "  -- Wang Bo, 'Sending Du Shaofu to Shuzhou'\n"
     "\n"
     "Hai nei cun zhi ji, tian ya ruo bi lin.\n"),
    ("NOTES.TXT",
     "The desktop is drawn straight into the framebuffer.\n"
     "Click 'This PC' to open a file manager window.\n"
     "You can open several windows and drag their title bars.\n"),
    ("CONTACT.TXT",
     "UMDos demo system\n"
     "Mail: umdos@example.invalid\n"),
]

DIRS = [
    ("DOCS", [
        ("INTRO.TXT",
         "Welcome to the DOCS directory.\n"
         "\n"
         "This is a real subdirectory in UMDFS: the dirent\n"
         "carries a parent index, and the file manager can\n"
         "walk down with a double click and up with Backspace.\n"),
        ("CHANGELOG.TXT",
         "v0.1  16-bit boot chain\n"
         "v0.2  64-bit C kernel, VBE graphics\n"
         "v0.3  UMDFS read-only filesystem\n"
         "v0.4  write support + text editor\n"
         "v0.5  ATA write-back\n"
         "v0.6  directories and multiple windows\n"),
    ]),
    ("SYSTEM", [
        ("SYSTEM.TXT",
         "Boot chain:\n"
         "  boot -> loader -> head -> hello -> firstmain -> nextmain\n"
         "  -> 64-bit C kernel\n"
         "\n"
         "Video: VBE linear framebuffer, 32 bpp\n"
         "Filesystem: UMDFS, 128 KB image at LBA 512\n"
         "Disk: ATA PIO write-back when booted from the hard disk\n"),
        ("SOURCES.TXT",
         "kernel/libk.c     memory and string helpers\n"
         "kernel/kmain.c    boot sequence\n"
         "kernel/gfx.c      framebuffer drawing\n"
         "kernel/input.c    PS/2 keyboard and mouse\n"
         "kernel/sha256.c   password verification\n"
         "kernel/fs.c       UMDFS reader/writer\n"
         "kernel/ata.c      ATA PIO driver\n"
         "kernel/ui.c       desktop, windows, editor\n"),
    ]),
]


def build_auth():
    """生成 /CONFIG/AUTH：SHA3-256 + 随机盐，不含明文密码。"""
    here = os.path.dirname(os.path.abspath(__file__))
    version = open(os.path.join(here, "VERSION"), encoding="utf-8").read().strip()
    lines = ["# UMDos /config/auth", "version=" + version]
    for name, user, pw, key in (
            ("user", "user", "umdos", "user_auth"),
            ("administrator", "administrator", "admin", "admin_auth")):
        salt = secrets.token_bytes(8)
        h = hashlib.sha3_256(salt + pw.encode()).hexdigest()
        lines.append("%s=%s" % (name, user))
        lines.append("%s=sha3-256$%s$%s" % (key, salt.hex(), h))
    return "\n".join(lines) + "\n"


def fnv1a(data, h=2166136261):
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def calc_checksum(img):
    h = fnv1a(img[0:40])
    h = fnv1a(img[DIRENT_BLOCK * BLOCK: DIRENT_BLOCK * BLOCK + DIRENT_COUNT * 64], h)
    h = fnv1a(img[BITMAP_BLOCK * BLOCK: BITMAP_BLOCK * BLOCK + BLOCK], h)
    return h


def build(size_bytes):
    assert size_bytes % BLOCK == 0
    blocks = size_bytes // BLOCK
    img = bytearray(size_bytes)

    # ---- 收集所有条目（目录先占住索引，子文件指向它）----
    entries = []                       # (name, type, parent, data)
    for name, text in ROOT_FILES:
        entries.append((name, 0, ROOT, text.encode("ascii")))
    for dname, files in DIRS:
        didx = len(entries)
        entries.append((dname, 1, ROOT, b""))
        for name, text in files:
            entries.append((name, 0, didx, text.encode("ascii")))
    # /CONFIG/AUTH：内核登录用的认证文件
    cidx = len(entries)
    entries.append(("CONFIG", 1, ROOT, b""))
    entries.append(("AUTH", 0, cidx, build_auth().encode("ascii")))
    assert len(entries) <= DIRENT_COUNT, len(entries)

    # ---- 分配数据块 ----
    placed = []
    cursor = DATA_START
    for name, etype, parent, data in entries:
        if etype == 1:
            placed.append((name, etype, parent, b"", 0, 0))
            continue
        need = (len(data) + BLOCK - 1) // BLOCK
        if need == 0:
            need = 1
        placed.append((name, etype, parent, data, cursor, need))
        cursor += need

    # ---- 超级块 ----
    struct.pack_into("<8sIIIIIIII", img, 0, MAGIC, BLOCK, blocks, len(entries),
                     DATA_START, DIRENT_BLOCK, DIRENT_COUNT, BITMAP_BLOCK, MAGIC2)

    # ---- 目录项 ----
    for i, (name, etype, parent, data, first, need) in enumerate(placed):
        off = DIRENT_BLOCK * BLOCK + i * 64
        raw = name.encode("ascii")[:31]
        img[off:off + len(raw)] = raw
        struct.pack_into("<II", img, off + 32, len(data), first)
        img[off + 40] = 1                       # used
        img[off + 41] = etype                   # 0=file 1=dir
        struct.pack_into("<I", img, off + 44, parent)

    # ---- 位图 ----
    bm = bytearray(BLOCK)
    for b in range(0, DATA_START):
        bm[b // 8] |= 1 << (b % 8)
    for name, etype, parent, data, first, need in placed:
        for b in range(first, first + need):
            bm[b // 8] |= 1 << (b % 8)
    img[BITMAP_BLOCK * BLOCK:BITMAP_BLOCK * BLOCK + BLOCK] = bm

    # ---- 数据 ----
    for name, etype, parent, data, first, need in placed:
        if data:
            off = first * BLOCK
            img[off:off + len(data)] = data

    # ---- P9：校验和（FNV-1a，覆盖超级块前 40 字节 + 目录项 + 位图）----
    struct.pack_into("<II", img, 40, 0, 0)     # checksum 占位 / journal_off = 0
    struct.pack_into("<I", img, 40, calc_checksum(img))
    return bytes(img)


FILE_COUNT = len(ROOT_FILES) + sum(len(f) for _, f in DIRS) + len(DIRS) + 2


if __name__ == "__main__":
    data = build(128 * 1024)
    open("fs.img", "wb").write(data)
    print("fs.img", len(data), FILE_COUNT, "entries")
