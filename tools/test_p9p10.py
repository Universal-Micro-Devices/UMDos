#!/usr/bin/env python3
"""P9 + P10 验收：UMDFS 校验/日志/增量写 + 进程抽象/每进程 fd 表/ fork//proc。

验收点（串口）：
  - fs selftest: ck=1 rw=11 jrnl=1 data=1 checksum/journal/write_at ok
  - proc: kernel pid=0, /proc ready
  - vfs selftest: auth/dev/stat/readdir/rw/proc ok
  - proc: fork -> pid=1
  - proc selftest: fork/fd/tasks/proc ok
  - 系统仍能登录进入桌面
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
        "ck=1 rw=11 jrnl=1 data=1",
        "checksum/journal/write_at ok",
        "[kernel] proc: kernel pid=0, /proc ready",
        "vfs selftest: auth/dev/stat/readdir/rw/proc ok",
        "[kernel] proc: fork -> pid=",
        "proc selftest: fork/fd/tasks/proc ok",
        "user selftest: elf/map/syscall ok",
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

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
