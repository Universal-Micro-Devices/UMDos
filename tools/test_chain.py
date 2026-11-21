#!/usr/bin/env python3
"""P2 启动链验收：boot -> loader -> head -> kernel（含串口日志与 /config/auth 登录）。

覆盖：
  - 软盘 CHS 启动
  - 硬盘 EDD 启动
  - 只读光盘启动
  - 串口日志包含 [boot] [loader] [head] [kernel]
  - 内核登录使用 /config/auth（SHA3-256 + 盐）
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emu
from emu import Machine, list_addr
from unicorn.x86_const import UC_X86_REG_RIP
from emu_ui import Kernel

SHOTS = os.path.join(emu.ROOT, "shots")
LONG64 = None


def boot_16(edd, drive, readonly):
    m = Machine(edd=edd, drive=drive, readonly=readonly)
    m.cmos = {0x00: 0x56, 0x02: 0x34, 0x04: 0x12, 0x0A: 0x00, 0x0B: 0x02}
    m.cmos_idx = 0
    r = m.run_until(LONG64)
    return m, r


def check_serial(tag, text, need):
    ok = all(n in text for n in need)
    print("[%s] %-10s serial: %r" % ("OK" if ok else "FAIL", tag, text.replace("\r\n", " | ")))
    return ok


def main():
    global LONG64
    LONG64, _ = list_addr(os.path.join(emu.BIN, "head.lst"),
                          lambda s: "long64:" in s, 0x9000)
    print("head.long64 = 0x%X" % LONG64)

    ok = True

    # 1) 三种启动介质都到长模式
    for tag, edd, drive, ro in (("floppy", False, 0x00, False),
                                ("hdd", True, 0x80, False),
                                ("cd-rom", True, 0x9F, True)):
        m, r = boot_16(edd, drive, ro)
        ok &= (r == "ok")
        ok &= check_serial(tag, m.serial, ["[boot]", "[loader]", "[head]"])

    # 2) 完整启动到内核桌面（用软盘场景的内存继续跑 64 位）
    m, r = boot_16(False, 0x00, False)
    mem = bytes(m.mu.mem_read(0, 0x100000))
    k = Kernel(mem, fb_base=m.fb, w=m.xres, h=m.yres)
    k.mu.reg_write(UC_X86_REG_RIP, LONG64)

    code = k.run_until({0x10})
    print("splash marker:", hex(code) if code else None)
    ok &= (code == 0x10)

    code = k.run_until({0x20})
    print("login marker:", hex(code) if code else None)
    ok &= (code == 0x20)
    k.settle()
    k.shot(os.path.join(SHOTS, "p2-login.png"))

    # 3) 用 /config/auth 里的默认账号登录
    k.type_text("user")
    k.type_text("\t")
    k.type_text("umdos")
    k.type_text("\r")
    code = k.run_until({0x30})
    print("desktop marker:", hex(code) if code else None)
    ok &= (code == 0x30)
    k.settle()
    k.shot(os.path.join(SHOTS, "p2-desktop.png"))

    # 4) 错误密码必须被拒绝
    k2_mem = bytes(m.mu.mem_read(0, 0x100000))
    k2 = Kernel(k2_mem, fb_base=m.fb, w=m.xres, h=m.yres)
    k2.mu.reg_write(UC_X86_REG_RIP, LONG64)
    k2.run_until({0x10})
    k2.run_until({0x20})
    k2.type_text("user")
    k2.type_text("\t")
    k2.type_text("wrongpw")
    k2.type_text("\r")
    k2.settle()
    still_login = k2.run_until({0x30}) is None
    print("[%s] wrong password rejected" % ("OK" if still_login else "FAIL"))
    ok &= still_login

    print("kernel serial:", repr(k.serial.replace("\r\n", " | ")))
    ok &= check_serial("kernel", k.serial, ["[kernel] init", "[kernel] splash start",
                                            "[kernel] login ready", "[kernel] login ok"])

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
