#!/usr/bin/env python3
"""P7 + P8 验收：VFS（文件描述符/路径/读写/dev）与设备注册表。

验收点：
  - 串口出现 dev 注册、vfs 挂载、vfs 自检通过
  - 用户态（模拟器下为内核等价自检）经系统调用 open/read /config/auth 成功
  - 系统随后仍能正常登录进入桌面
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emu
from emu import Machine, list_addr
from unicorn.x86_const import UC_X86_REG_RIP
from emu_ui import Kernel


def main():
    long64, _ = list_addr(os.path.join(emu.BIN, "head.lst"), lambda s: "long64:" in s, 0x9000)
    m = Machine(edd=False, drive=0x00)
    m.cmos = {0x00: 0x56, 0x02: 0x34, 0x04: 0x12, 0x0A: 0x00, 0x0B: 0x02}
    m.cmos_idx = 0
    print("16-bit chain:", m.run_until(long64))
    ok = True

    mem = bytes(m.mu.mem_read(0, 0x100000))
    k = Kernel(mem, fb_base=m.fb, w=m.xres, h=m.yres)
    k.mu.reg_write(UC_X86_REG_RIP, long64)
    k.run_until({0x10})
    k.run_until({0x20})
    k.settle()

    need = [
        "[kernel] storage: umdfs mounted",
        "[kernel] dev: registered serial/kbd/mouse/",
        "[kernel] vfs: mounted umdfs at /",
        "[kernel] vfs: /dev/serial write ok",
        "[kernel] vfs selftest: auth/dev/stat/readdir/rw/proc ok",
        "/vfs=11111",
        "[kernel] user selftest: elf/map/syscall ok",
    ]
    for s in need:
        good = s in k.serial
        print("[%s] %s" % ("OK" if good else "FAIL", s))
        ok &= good

    k.type_text("user")
    k.type_text("\t")
    k.type_text("umdos")
    k.type_text("\r")
    code = k.run_until({0x30})
    print("[%s] login -> desktop" % ("OK" if code == 0x30 else "FAIL"))
    ok &= (code == 0x30)
    k.settle()
    k.shot(os.path.join(emu.ROOT, "shots", "p7-desktop.png"))

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
