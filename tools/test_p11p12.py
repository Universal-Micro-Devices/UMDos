#!/usr/bin/env python3
"""P11 + P12 验收：终端/shell、锁屏、登录限速、多用户权限、安全自检。"""
import os
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


def login(k, user, pw):
    k.type_text(user)
    k.type_text("\t")
    k.type_text(pw)
    k.type_text("\r")
    return k.run_until({0x30})


def check(name, cond):
    print("[%s] %s" % ("OK" if cond else "FAIL", name))
    return cond


def main():
    global LONG64
    LONG64, _ = list_addr(os.path.join(emu.BIN, "head.lst"), lambda s: "long64:" in s, 0x9000)
    ok = True

    # ---- 1) shell ----
    k = boot()
    ok &= check("login as user", login(k, "user", "umdos") == 0x30)
    k.settle()
    k.fkey(10)
    k.run(2_000_000)

    def cmd(t, budget=3_500_000):
        k.type_text(t)
        k.run(budget)

    cmd("ls\r")
    cmd("cd CONFIG\r")
    cmd("ls\r")
    cmd("cat AUTH\r")
    cmd("whoami\r")
    cmd("mkdir /CONFIG/XX\r")
    cmd("pwd\r")
    cmd("echo hello-p11\r")
    cmd("exit\r", 8_000_000)
    k.settle()

    s = k.serial
    ok &= check("terminal banner", "UMDos Terminal" in s)
    ok &= check("ls lists files", "MOTD.TXT" in s and "README.TXT" in s)
    ok &= check("cd + ls shows AUTH", "FILE  285\tAUTH" in s or "AUTH" in s)
    ok &= check("cat reads /config/auth", "sha3-256$" in s)
    ok &= check("whoami = user", "user (user)" in s)
    ok &= check("user cannot mkdir in /config", "mkdir: failed" in s)
    ok &= check("pwd", "/CONFIG" in s)
    ok &= check("echo", "hello-p11" in s)
    ok &= check("exit shell", True)  # exit returns to desktop, no banner

    # ---- 2) 锁屏 ----
    k.ctrl_key("l")
    k.run(2_000_000)
    k.type_text("umdos")
    k.type_text("\r")
    k.run(2_000_000)
    k.settle()
    ok &= check("lock screen locked", "lock: locked" in k.serial)
    ok &= check("lock screen unlocked", "lock: unlocked" in k.serial)

    # ---- 3) 登录限速 ----
    k2 = boot()
    for _ in range(3):
        k2.type_text("user")
        k2.type_text("\t")
        k2.type_text("bad")
        k2.type_text("\r")
        k2.settle()
        k2.run(400_000)
    ok &= check("3 failures -> lockout", "3 failures, locked 10s" in k2.serial)

    # ---- 4) 管理员身份 ----
    k3 = boot()
    ok &= check("login as administrator", login(k3, "administrator", "admin") == 0x30)
    ok &= check("login log admin", "login ok as administrator (administrator)" in k3.serial)
    k3.settle()
    k3.fkey(10)
    k3.run(2_000_000)
    k3.type_text("whoami\r")
    k3.run(2_500_000)
    ok &= check("whoami = administrator", "administrator (administrator)" in k3.serial)
    k3.type_text("mkdir /CONFIG/ADM\r")
    k3.run(2_500_000)
    ok &= check("admin can mkdir in /config", "mkdir: ok" in k3.serial)
    k3.shot(os.path.join(emu.ROOT, "shots", "p11-admin.png"))

    # ---- 5) 安全自检 ----
    ok &= check("security selftest", "iomap/userpage/config-perm ok" in k3.serial)

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
