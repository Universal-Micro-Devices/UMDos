#!/usr/bin/env python3
"""安装引导验收：启动 installer.img，走完安装向导，检查目标磁盘 64MB 镜像。

校验项：
  - MBR 分区项指向 LBA 2048
  - 分区内有合法的 FAT32 引导扇区
  - /CONFIG/AUTH 存在，内容为 sha3-256$salt$hash
  - 用 Python hashlib 重算 SHA3-256(salt+password)，与磁盘内容一致
  - 磁盘上不出现明文密码
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emu
from emu import Machine, keycodes, list_addr
from unicorn.x86_const import UC_X86_REG_RIP
from emu_ui import Kernel

INST_IMG = os.path.join(emu.ROOT, "bin", "installer", "installer.img")
SHOTS = os.path.join(emu.ROOT, "shots")
DISK_MB = 64


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def u32(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


def parse_fat32(img, part_lba):
    bs = img[part_lba * 512:part_lba * 512 + 512]
    if bs[510] != 0x55 or bs[511] != 0xAA:
        return None
    bps = u16(bs, 11)
    spc = bs[13]
    reserved = u16(bs, 14)
    nfats = bs[16]
    fatsz = u32(bs, 36)
    root = u32(bs, 44)
    if bps != 512:
        return None
    data_lba = part_lba + reserved + nfats * fatsz

    def cluster_lba(c):
        return data_lba + (c - 2) * spc

    def read_cluster(c):
        off = cluster_lba(c) * 512
        return img[off:off + spc * 512]

    def list_dir(c):
        out = []
        buf = read_cluster(c)
        for i in range(0, len(buf), 32):
            e = buf[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] == 0x0F:
                continue
            if e[0] == ord('.'):
                continue
            name = bytes(e[0:11]).decode("ascii", "replace")
            cl = ((e[21] << 8 | e[20]) << 16) | (e[27] << 8 | e[26])
            size = u32(e, 28)
            out.append((name, e[11], cl, size))
        return out

    root_entries = list_dir(root)
    config = [x for x in root_entries if x[0].startswith("CONFIG") and (x[1] & 0x10)]
    if not config:
        return {"error": "no CONFIG dir", "root": root_entries}
    cfg_cluster = config[0][2]
    entries = list_dir(cfg_cluster)
    auth = [x for x in entries if x[0].startswith("AUTH")]
    if not auth:
        return {"error": "no AUTH file", "dir": entries}
    cl, size = auth[0][2], auth[0][3]
    data = read_cluster(cl)[:size]
    return {"auth": data.decode("ascii", "replace"), "size": size,
            "root": root_entries, "dir": entries}


def main():
    os.makedirs(SHOTS, exist_ok=True)
    version = open(os.path.join(emu.ROOT, "VERSION"), encoding="utf-8").read().strip()
    inst = open(INST_IMG, "rb").read()

    # 1) 16 位链：boot -> firstmain -> 长模式
    m = Machine(edd=False, drive=0x00, image=inst)
    m.cmos = {0x00: 0x56, 0x02: 0x34, 0x04: 0x12, 0x0A: 0x00, 0x0B: 0x02}
    m.cmos_idx = 0
    farj, _ = list_addr(os.path.join(emu.BIN, "installer", "firstmain.bin.lst"),
                        lambda s: "jmp 0x08:long64" in s, 0xC000)
    long64, _ = list_addr(os.path.join(emu.BIN, "installer", "firstmain.bin.lst"),
                          lambda s: "long64:" in s, 0xC000)
    print("16-bit phase:", m.run_until(farj))
    mem = bytes(m.mu.mem_read(0, 0x100000))

    # 2) 64 位安装程序 + 一块 64MB 空磁盘
    disk = bytearray(DISK_MB * 1024 * 1024)
    k = Kernel(mem, fb_base=m.fb, w=m.xres, h=m.yres, disk_bytes=disk)
    k.mu.reg_write(UC_X86_REG_RIP, long64)
    print("installer start:", hex(k.run_until({0x40}) or 0))
    k.settle()
    k.shot(os.path.join(SHOTS, "i1-welcome.png"))

    # 3) 走完向导
    def step(keys, expect, shot=None):
        for kk in keys:
            k.type_text(kk)
        got = k.run_until(expect)
        k.settle()
        if shot:
            k.shot(os.path.join(SHOTS, shot))
        return got

    print("page1 (disk):", hex(step(["\r"], {0x61}, "i2-disk.png") or 0))
    print("page2 (partition):", hex(step(["\r"], {0x62}, "i3-partition.png") or 0))
    print("page3 (fs):", hex(step(["\r"], {0x63}, "i4-fs.png") or 0))
    print("page4 (path):", hex(step(["\r"], {0x64}, "i5-path.png") or 0))
    print("page5 (accounts):", hex(step(["\r"], {0x65}, "i6-accounts0.png") or 0))
    # 账户页：默认 user；Tab 到普通密码、输入；Tab 到管理员密码、输入
    k.type_text("\t")
    k.type_text("userpw123")
    k.type_text("\t")
    k.type_text("adminpw456")
    k.settle()
    k.shot(os.path.join(SHOTS, "i6-accounts.png"))
    print("page6 (risk):", hex(step(["\r"], {0x66}, "i7-risk.png") or 0))
    # 风险页按 Enter = 同意，开始写入
    k.type_text("\r")
    print("write done:", hex(k.run_until({0x50}, budget=60_000_000) or 0))
    k.settle()
    k.shot(os.path.join(SHOTS, "i8-done.png"))

    # 4) 检查目标磁盘
    img = bytes(k.img)
    print("disk size:", len(img))
    part_start = u32(img, 446 + 8)
    part_sectors = u32(img, 446 + 12)
    print("MBR: status=%02x type=%02x start=%d sectors=%d" %
          (img[446], img[450], part_start, part_sectors))
    ok = True
    if img[510] != 0x55 or img[511] != 0xAA or img[446] != 0x80 or img[450] != 0x0C:
        print("  [FAIL] MBR")
        ok = False
    if part_start != 2048:
        print("  [FAIL] partition start != 2048")
        ok = False

    info = parse_fat32(img, part_start)
    print("FAT32:", "ok" if info and "auth" in info else info)
    if not info or "auth" not in info:
        print("RESULT: FAIL (no auth file)")
        return 1

    auth = info["auth"]
    print("---- /config/auth ----")
    print(auth)
    print("----------------------")
    ok = ok and "sha3-256$" in auth
    ok = ok and "userpw123" not in auth and "adminpw456" not in auth

    # 重算哈希核对
    lines = dict(l.split("=", 1) for l in auth.strip().splitlines() if "=" in l)
    for key, pw in (("user_auth", "userpw123"), ("admin_auth", "adminpw456")):
        v = lines.get(key, "")
        parts = v.split("$")
        if len(parts) != 3 or parts[0] != "sha3-256":
            print("  [FAIL] %s format: %r" % (key, v))
            ok = False
            continue
        salt = bytes.fromhex(parts[1])
        want = hashlib.sha3_256(salt + pw.encode()).hexdigest()
        if want != parts[2]:
            print("  [FAIL] %s sha3 mismatch" % key)
            ok = False
        else:
            print("  [OK] %s sha3-256 校验通过 (salt %s)" % (key, parts[1]))
    print("version in auth:", lines.get("version"))
    if lines.get("version") != version:
        print("  [FAIL] version mismatch")
        ok = False

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
