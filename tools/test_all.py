#!/usr/bin/env python3
"""UMDos 回归测试：软盘/CHS、硬盘/EDD、只读介质、错误激活码、错误密码。"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emu
from emu import Machine, keycodes, list_addr, OUT

BIN = emu.BIN
A = {}
A["idle"] = list_addr(os.path.join(BIN, "head.lst"), lambda s: s.rstrip().endswith("hlt"), 0x8800)[0]
A["farj"] = list_addr(os.path.join(BIN, "firstmain.lst"), lambda s: "jmp 0x08:long64" in s, 0xC000)[0]
A["long64"] = list_addr(os.path.join(BIN, "firstmain.lst"), lambda s: "long64:" in s, 0xC000)[0]
A["nm"] = 0

CMOS = {0x00: 0x56, 0x02: 0x34, 0x04: 0x12, 0x0A: 0x00, 0x0B: 0x02}


def new_machine(edd, drive, readonly=False):
    m = Machine(edd=edd, drive=drive, readonly=readonly)
    m.cmos = dict(CMOS)
    m.cmos_idx = 0
    return m


def boot_stage(m, activation="UMDOS2024", user="umdos", pw="pass1234"):
    m.keys = keycodes(f"{user}\r{pw}\r{activation}\r")
    return m.run_until(A["idle"])


def login(m, user="umdos", pw="pass1234"):
    m.keys = keycodes(f"{user}\t{pw}\r")
    return m.run_until(0xA000)


def to_long_mode(m):
    # 16 位链跑到长模式跳转；C 内核本身由 tools/emu_ui.py 负责验证
    return m.run_until(A["farj"])


def full_run(edd, drive, readonly, tag):
    m = new_machine(edd, drive, readonly)
    r1 = boot_stage(m)
    r2 = login(m)
    r3 = to_long_mode(m)
    ok = (r1 == "ok" and r2 == "ok" and r3 == "ok")
    print(f"[{'PASS' if ok else 'FAIL'}] {tag}: boot={r1} login={r2} 64bit={r3}")
    m.shot_vga(os.path.join(OUT, f"t-{tag}.png"))
    return ok


def wrong_activation():
    m = new_machine(False, 0x00)
    m.keys = keycodes("umdos\rpass1234\rBADCODE\r")
    r = m.run_until(A["idle"])
    text = "".join(m.text)
    ok = ("Wrong activation code" in text) and (m.get16("ip") < 0xA000)
    print(f"[{'PASS' if ok else 'FAIL'}] 错误激活码被拒绝 (ip=0x{m.get16('ip'):04X})")
    return ok


def wrong_password():
    m = new_machine(False, 0x00)
    boot_stage(m)
    r = m.run_until(0xA000)          # 应停在 head 的 idle，而不是进入 hello
    m.keys = keycodes("umdos\tWRONG\r")
    r = m.run_until(0xA000)
    ok = (r != "ok")
    print(f"[{'PASS' if ok else 'FAIL'}] 错误密码被拒绝 ({r}, ip=0x{m.get16('ip'):04X})")
    m.shot_vga(os.path.join(OUT, "t-wrong-pass.png"))
    return ok


def hostile_bios():
    """模拟不保留 ES 的 BIOS：所有 INT 返回后 ES 都是脏的。"""
    m = new_machine(False, 0x00)
    m.clobber_es = True
    r1 = boot_stage(m)
    r2 = login(m)
    r3 = to_long_mode(m)
    ok = (r1 == "ok" and r2 == "ok" and r3 == "ok")
    print(f"[{'PASS' if ok else 'FAIL'}] 不保留 ES 的 BIOS: boot={r1} login={r2} 64bit={r3}")
    return ok


if __name__ == "__main__":
    results = [
        full_run(False, 0x00, False, "floppy-chs"),
        full_run(True, 0x80, False, "hdd-edd"),
        full_run(True, 0x80, True, "readonly-cd"),
        hostile_bios(),
        wrong_activation(),
        wrong_password(),
    ]
    print("\n%d/%d passed" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)
