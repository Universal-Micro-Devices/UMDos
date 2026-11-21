#!/usr/bin/env python3
"""单独验证 PS/2 鼠标通路：初始化、光标绘制、IRQ12 处理、点击切换焦点。"""
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emu
from emu import Machine, keycodes, list_addr, OUT

lst = os.path.join(emu.BIN, "head.lst")
addr = lambda name: list_addr(lst, lambda s, n=name: (n + ":") in s, 0x8800)[0]
def data_addr(name):
    pat = re.compile(r"\b" + name + r"\s+(db|dw)\b")
    return list_addr(lst, lambda s: pat.search(s) is not None, 0x8800)[0]
IDLE = list_addr(lst, lambda s: s.rstrip().endswith("hlt"), 0x8800)[0]
ISR = addr("mouse_isr")
A_MOK, A_FIELD = data_addr("m_ok"), data_addr("field")
A_MX, A_MY = data_addr("mx"), data_addr("my")

m = Machine(edd=False, drive=0x00)
m.cmos = {0x00: 0x56, 0x02: 0x34, 0x04: 0x12, 0x0A: 0x00, 0x0B: 0x02}
m.cmos_idx = 0
m.kbc = True
m.keys = keycodes("umdos\rpass1234\rUMDOS2024\r")
r1 = m.run_until(IDLE)
m.keys = keycodes("umdos\tpass1234\r")
r2 = m.run_until(0xA000)          # 登录成功后 head 已经跳走，这里只看初始化结果
ok_init = m.rd8(A_MOK) == 1 if hasattr(m, "rd8") else True
print("boot/login:", r1, r2)

# 回到登录前的状态重新来一遍，只跑 UI，不登录
m2 = Machine(edd=False, drive=0x00)
m2.cmos = {0x00: 0x56, 0x02: 0x34, 0x04: 0x12, 0x0A: 0x00, 0x0B: 0x02}
m2.cmos_idx = 0
m2.kbc = True
m2.keys = keycodes("umdos\rpass1234\rUMDOS2024\r")
print("run to idle:", m2.run_until(IDLE), hex(m2.get16("ip")))

mok = bytes(m2.mu.mem_read(A_MOK, 1))[0]
mx, my = struct.unpack("<HH", bytes(m2.mu.mem_read(A_MX, 4)))
field = bytes(m2.mu.mem_read(A_FIELD, 1))[0]
print(f"m_ok={mok} mouse=({mx},{my}) field={field}")
assert mok == 1, "鼠标初始化应当成功"
assert (mx, my) == (160, 100), "初始光标位置"

# 光标像素：5x5 块被 XOR 了 0x0F
px = bytes(m2.mu.mem_read(0xA0000 + my * 320 + mx, 1))[0]
bg = 1
print("cursor pixel =", hex(px), "expected", hex(bg ^ 0x0F))
assert px == (bg ^ 0x0F), "光标没有画出来"

# 手动触发 3 次 IRQ12：dx=+10, dy=+10（向下）
def irq12(byte):
    m2.kbc_byte = byte
    sp = m2.get16("sp") - 2
    m2.mu.mem_write(sp, struct.pack("<H", 0x0202))          # FLAGS
    sp -= 2; m2.mu.mem_write(sp, struct.pack("<H", 0x0000)) # CS
    sp -= 2; m2.mu.mem_write(sp, struct.pack("<H", 0x0600)) # 返回地址
    m2.set16("sp", sp)
    m2.mu.mem_write(0x0600, b"\xF4\xEB\xFE")
    m2.mu.emu_start(ISR, 0x0600, count=100000)

irq12(0x08)     # flags: bit3=1, 左键松开
irq12(0x0A)     # dx = +10
irq12(0xF6)     # dy = -10（PS/2 里 +Y 向上，屏幕上要向下就是负值）
mx, my = struct.unpack("<HH", bytes(m2.mu.mem_read(A_MX, 4)))
print(f"after IRQ12: mouse=({mx},{my})")
assert (mx, my) == (170, 110), "鼠标位移错误"

# 点击：flags bit0=1，位置在密码框内 -> field=1
irq12(0x09); irq12(0x00); irq12(0x00)
field = bytes(m2.mu.mem_read(A_FIELD, 1))[0]
print("field after click =", field)
assert field == 1, "点击应切到密码框"

print("mouse test: PASS")
