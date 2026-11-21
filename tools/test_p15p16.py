#!/usr/bin/env python3
"""P15/P16 验收：SMP 基础设施（percpu/spinlock/MADT）+ AHCI（PCI/identify/read）"""
import os, sys, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import emu
from emu import Machine, list_addr
from emu_ui import Kernel
from unicorn.x86_const import UC_X86_REG_RIP

PY = sys.executable
IMG = os.path.join(ROOT, "UMDos v2.6.112-dev.11 floppy.img")


def build():
    subprocess.check_call([PY, os.path.join(ROOT, "build.py")], cwd=ROOT,
                          stdout=subprocess.DEVNULL)


def main():
    build()
    long64, _ = list_addr(os.path.join(emu.BIN, "head.lst"),
                          lambda s: "long64:" in s, 0x9000)
    m = Machine(edd=False, drive=0x00)
    m.cmos = {0x00: 0x56, 0x02: 0x34, 0x04: 0x12, 0x0A: 0x00, 0x0B: 0x02}
    m.cmos_idx = 0
    if m.run_until(long64) != "ok":
        print("RESULT: FAIL - 16 位启动失败")
        return 1
    mem = bytes(m.mu.mem_read(0, 0x100000))
    k = Kernel(mem, fb_base=m.fb, w=m.xres, h=m.yres)
    k.mu.reg_write(UC_X86_REG_RIP, long64)
    k.run_until({0x10})
    k.run_until({0x20})
    k.settle()
    k.type_text("user")
    k.type_text("\t")
    k.type_text("umdos")
    k.type_text("\r")
    mk = k.run_until({0x30})
    k.settle()

    s = k.serial
    checks = [
        ("percpu 初始化", "[kernel] percpu: bsp apic_id=" in s),
        ("MADT LAPIC 列表", "[kernel] smp: madt lapic=" in s),
        ("BSP/单核路径", "[kernel] smp: BSP only, AP bring-up skipped" in s),
        ("percpu 自检", "[kernel] percpu selftest: bsp/stack/cpuid ok" in s),
        ("自旋锁自检", "[kernel] smp selftest: spinlock/bsp ok" in s),
        ("PCI/AHCI 多盘", 'ahci: disk1 model="UMDos Virtual SATA B"' in s),
        ("AHCI 读扇区", "disk1 read ok" in s),
        ("块设备注册", "dev: registered serial/kbd/mouse/ahci0/ahci1/ata" in s),
        ("越界错误路径", "ahci: disk1 range check ok" in s),
        ("端口复位重试", "ahci: port reset (cmd error)" in s),
        ("写/读回环", "ahci: disk1 write/read ok" in s),
        ("恢复后可用", "ahci: disk1 recheck ok" in s),
        ("桌面标记 0x30", mk == 0x30),
    ]
    ok = True
    for name, good in checks:
        print("  %-14s %s" % (name, "OK" if good else "FAIL"))
        if not good:
            ok = False
    if not ok:
        print("serial tail:", repr(s[-400:]))
        print("RESULT: FAIL")
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
