#!/usr/bin/env python3
"""验证 UMDFS 写回磁盘：第一次启动新建文件并保存，第二次启动（同一块盘）还能看到它。"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emu
from emu import Machine, keycodes, list_addr
from unicorn.x86_const import UC_X86_REG_RIP
from emu_ui import Kernel

BIN = emu.BIN
SHOTS = os.path.join(emu.ROOT, "shots")


def boot(image):
    """P2 四级链：boot -> loader -> head -> kernel"""
    m = Machine(edd=False, drive=0x00, image=image)
    m.cmos = {0x00: 0x56, 0x02: 0x34, 0x04: 0x12, 0x0A: 0x00, 0x0B: 0x02}
    m.cmos_idx = 0
    long64, _ = list_addr(os.path.join(BIN, "head.lst"), lambda s: "long64:" in s, 0x9000)
    r = m.run_until(long64)
    if r != "ok":
        raise SystemExit("16-bit chain failed: %r" % r)
    mem = bytes(m.mu.mem_read(0, 0x100000))
    k = Kernel(mem, fb_base=m.fb, w=m.xres, h=m.yres, disk_bytes=image)
    k.mu.reg_write(UC_X86_REG_RIP, long64)
    return k


def login_and_open_files(k):
    k.run_until({0x10})
    k.run_until({0x20})
    k.type_text("user")
    k.type_text("\t")
    k.type_text("umdos")
    k.type_text("\r")
    k.run_until({0x30})
    k.type_text("\r")          # 桌面 Enter 打开文件管理器
    k.run_until({0x31})


def list_fs(img, lba=512):
    sb = img[lba * 512:lba * 512 + 512]
    magic = bytes(sb[0:6])
    bs, bc, fc, ds, dblk, dcnt = struct.unpack_from("<6I", sb, 8)
    out = []
    for i in range(dcnt):
        off = (lba + dblk) * 512 + i * 64
        name = bytes(img[off:off + 32]).split(b'\0')[0].decode("ascii", "replace")
        size, first = struct.unpack_from("<II", img, off + 32)
        if img[off + 40] and name:
            out.append((name, size))
    return magic, fc, out


def main():
    print("== boot 1: create PERSIST.TXT ==")
    k = boot(open(emu.IMG, "rb").read())
    login_and_open_files(k)
    k.type_text("n")
    k.run_until({0x37})
    k.type_text("\b" * 7)
    k.type_text("PERSIST.TXT")
    k.type_text("\r")
    k.run_until({0x36})
    k.type_text("\r")                      # 新建后已选中，直接打开
    k.run_until({0x34})
    k.type_text("still here after reboot.")
    k.type_text("\x1b")                    # 保存 -> 写回磁盘
    k.run_until({0x35})
    k.shot(os.path.join(SHOTS, "p1-created.png"))
    img2 = bytes(k.img)
    print("   on-disk:", list_fs(img2))

    print("== boot 2: same disk image ==")
    k2 = boot(img2)
    login_and_open_files(k2)
    k2.shot(os.path.join(SHOTS, "p2-after-reboot.png"))
    magic, fc, files = list_fs(k2.img)
    print("   on-disk:", magic, "files =", fc)
    for name, size in files:
        print("     %-12s %4d" % (name, size))
    want = len("still here after reboot.")
    ok = any(n == "PERSIST.TXT" and s == want for n, s in files)
    print("RESULT:", "PASS - 文件在重启后仍然存在" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
