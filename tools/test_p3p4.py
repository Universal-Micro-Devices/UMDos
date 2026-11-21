#!/usr/bin/env python3
"""P3 + P4 验收：IDT/异常/panic、PIC/PIT/IRQ 自检、E820/页帧/堆/页表自检。

验收点：
  - 串口日志出现 idt ready / pic remapped / pit 100Hz / irq selftest ok
  - 串口日志出现 E820 条目数、页帧统计、堆地址、mem selftest ok
  - 键盘（IRQ 路径 + 轮询回退）能完成登录
  - F9 触发除零异常 -> panic 屏（截图）且串口输出 PANIC
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emu
from emu import Machine, list_addr
from unicorn.x86_const import UC_X86_REG_RIP
from emu_ui import Kernel

SHOTS = os.path.join(emu.ROOT, "shots")


def main():
    os.makedirs(SHOTS, exist_ok=True)
    long64, _ = list_addr(os.path.join(emu.BIN, "head.lst"), lambda s: "long64:" in s, 0x9000)
    m = Machine(edd=False, drive=0x00)
    m.cmos = {0x00: 0x56, 0x02: 0x34, 0x04: 0x12, 0x0A: 0x00, 0x0B: 0x02}
    m.cmos_idx = 0
    print("16-bit chain:", m.run_until(long64))
    ok = True
    ok &= all(s in m.serial for s in ("[boot]", "[loader]", "[head]"))
    ok &= "e820 ok" in m.serial

    mem = bytes(m.mu.mem_read(0, 0x100000))
    k = Kernel(mem, fb_base=m.fb, w=m.xres, h=m.yres)
    k.mu.reg_write(UC_X86_REG_RIP, long64)

    k.run_until({0x10})
    k.run_until({0x20})
    k.settle()

    need = [
        "[kernel] idt ready",
        "irq: pic remapped",
        "irq selftest: kbd ok, pit ok",
        "mem: e820 entries=",
        "mem selftest: kmalloc/vmm ok",
    ]
    for s in need:
        good = s in k.serial
        print("[%s] %s" % ("OK" if good else "FAIL", s))
        ok &= good

    # 登录（键盘路径）
    k.type_text("user")
    k.type_text("\t")
    k.type_text("umdos")
    k.type_text("\r")
    code = k.run_until({0x30})
    print("[%s] login -> desktop" % ("OK" if code == 0x30 else "FAIL"))
    ok &= (code == 0x30)
    k.settle()
    k.shot(os.path.join(SHOTS, "p3-desktop.png"))

    # F9：除零异常 -> panic 屏
    k.fkey(9)
    k.run_until(set(), budget=8_000_000)     # 耗尽预算即可，panic 不会再有标记
    k.settle()
    k.shot(os.path.join(SHOTS, "p3-panic.png"))
    good = "PANIC exc=" in k.serial and "rip=0x" in k.serial
    print("[%s] panic on divide error: %r" % ("OK" if good else "FAIL",
                                              k.serial[-120:].replace("\r\n", " | ")))
    ok &= good

    # IRQ1 注入：走真实的 IDT + 中断门 + 键盘 IRQ 处理，打开文件管理器
    k2_mem = bytes(m.mu.mem_read(0, 0x100000))
    k2 = Kernel(k2_mem, fb_base=m.fb, w=m.xres, h=m.yres)
    k2.mu.reg_write(UC_X86_REG_RIP, long64)
    k2.run_until({0x10}); k2.run_until({0x20})
    k2.type_text("user"); k2.type_text("\t"); k2.type_text("umdos"); k2.type_text("\r")
    k2.run_until({0x30})
    k2.settle()
    k2.queue.append(("kbd", 0x1C)); k2.queue.append(("kbd", 0x9C))
    irq_ok = k2.deliver_int(33)
    k2.run(1_500_000)
    k2.deliver_int(33)
    k2.run(3_000_000)
    good = irq_ok and ("[kernel] irq1 sc=0x1C" in k2.serial)
    print("[%s] IRQ1 经 IDT 进入处理程序 (sc=0x1C)" % ("OK" if good else "FAIL"))
    ok &= good

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
