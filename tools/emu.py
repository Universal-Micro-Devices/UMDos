#!/usr/bin/env python3
"""在 Unicorn 里真实执行 UMDos 引导链（16 位实模式 + 64 位长模式），并截图 / 断言。"""
import os, re, struct, sys, json
from unicorn import *
from unicorn.x86_const import *
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "bin")
VERSION = open(os.path.join(ROOT, "VERSION"), encoding="utf-8").read().strip()
IMG = os.path.join(ROOT, "UMDos %s floppy.img" % VERSION)
OUT = os.path.join(ROOT, "shots")
os.makedirs(OUT, exist_ok=True)

REG = dict(ax=UC_X86_REG_AX, bx=UC_X86_REG_BX, cx=UC_X86_REG_CX, dx=UC_X86_REG_DX,
           si=UC_X86_REG_SI, di=UC_X86_REG_DI, bp=UC_X86_REG_BP, sp=UC_X86_REG_SP,
           ds=UC_X86_REG_DS, es=UC_X86_REG_ES, ss=UC_X86_REG_SS, cs=UC_X86_REG_CS,
           ip=UC_X86_REG_IP, flags=UC_X86_REG_EFLAGS)

EGA = [(0,0,0),(0,0,42),(0,42,0),(0,42,42),(42,0,0),(42,0,42),(42,21,0),(42,42,42),
       (21,21,21),(21,21,63),(21,63,21),(21,63,63),(63,21,21),(63,21,63),(63,63,21),(63,63,63)]

def default_palette():
    pal = [[0,0,0] for _ in range(256)]
    for i,(r,g,b) in enumerate(EGA):
        pal[i] = [r*255//63, g*255//63, b*255//63]
    for i in range(16,32):
        v = (i-16)*4
        pal[i] = [v*255//63]*3
    for i in range(32,256):
        r = ((i>>5)&7)*9; g = ((i>>2)&7)*9; b = (i&3)*21
        pal[i] = [r*255//63, g*255//63, b*255//63]
    return pal

def list_addr(lst_path, pred, org):
    """在 NASM 清单里找到第一条满足 pred(整行文本) 的地址；
    若命中的是纯标号行（无地址），取其后第一条带地址的指令。"""
    lines = open(lst_path, encoding="latin-1").read().splitlines()
    for i, line in enumerate(lines):
        if not pred(line):
            continue
        m = re.match(r"^\s*\d+\s+([0-9A-Fa-f]{8})\s", line)
        if m:
            return org + int(m.group(1), 16), line.strip()
        for l2 in lines[i + 1:]:
            m2 = re.match(r"^\s*\d+\s+([0-9A-Fa-f]{8})\s", l2)
            if m2:
                return org + int(m2.group(1), 16), l2.strip()
    raise SystemExit(f"not found in {lst_path}")

class NeedKey(Exception):
    pass

class Machine:
    def __init__(self, edd=True, drive=0x80, readonly=False, image=None):
        self.img = bytearray(image if image is not None else open(IMG, "rb").read())
        self.edd, self.drive, self.readonly = edd, drive, readonly
        self.kbc = False            # 是否模拟一个能应答的 PS/2 控制器
        self.kbc_byte = 0xFA
        self.serial = ""
        self.powered_off = False
        self.rebooted = False
        self.clobber_es = False     # 模拟"不守规矩"的 BIOS：int 13h 弄脏 ES
        self.keys = []
        self.text = []
        self.dac = default_palette()
        self.dac_idx = 0; self.dac_state = 0
        self.mode = 3
        self.fb = 0xFD000000
        self.xres, self.yres, self.bytes_pp = 1280, 720, 4
        self.stride_bytes = 1280*4
        self.vbe_modes = [(0x118, 1280, 720), (0x119, 1024, 768), (0x11A, 800, 600)]
        self.int_log = []
        self.debug_kbd = False
        self.mu = Uc(UC_ARCH_X86, UC_MODE_16)
        self.mu.mem_map(0, 0x100000)
        self.write_acpi_tables()
        self.mu.mem_map(self.fb, 0x1000000)
        self.mu.mem_write(0x7C00, bytes(self.img[:512]))
        for r, v in ((UC_X86_REG_CS,0),(UC_X86_REG_DS,0),(UC_X86_REG_ES,0),(UC_X86_REG_SS,0),
                     (UC_X86_REG_SP,0x7C00),(UC_X86_REG_IP,0x7C00),(UC_X86_REG_DX,drive)):
            self.mu.reg_write(r, v)
        self.mu.reg_write(UC_X86_REG_EFLAGS, 2)
        self.mu.hook_add(UC_HOOK_INTR, self.on_int)
        self.mu.hook_add(UC_HOOK_INSN, self.on_in, object(), 1, 0, UC_X86_INS_IN)
        self.mu.hook_add(UC_HOOK_INSN, self.on_out, object(), 1, 0, UC_X86_INS_OUT)

    # ---------------- 寄存器/标志 ----------------
    def get16(self, n): return self.mu.reg_read(REG[n]) & 0xFFFF
    def set16(self, n, v): self.mu.reg_write(REG[n], v & 0xFFFF)
    def ah(self): return (self.get16("ax") >> 8) & 0xFF
    def al(self): return self.get16("ax") & 0xFF
    def set_ax(self, v): self.set16("ax", v)
    def cf(self, on):
        f = self.mu.reg_read(REG["flags"])
        self.mu.reg_write(REG["flags"], (f | 1) if on else (f & ~1))
    def set_zf(self, on):
        f = self.mu.reg_read(REG["flags"])
        self.mu.reg_write(REG["flags"], (f | 0x40) if on else (f & ~0x40))

    # ---------------- I/O ----------------
    def on_in(self, mu, port, size, ud):
        # Unicorn 的 IN 钩子靠"返回值"给寄存器赋值
        if port == 0x3C9:
            p = self.dac[self.dac_idx]
            return p[self.dac_state % 3]
        if port == 0x60:
            return self.kbc_byte if self.kbc else 0x00
        if port == 0x64:
            return 0x01 if self.kbc else 0x00
        if port == 0x71:
            return self.cmos.get(self.cmos_idx, 0)
        return 0xFF

    def on_out(self, mu, port, size, value, ud):
        if port == 0x3F8:
            self.serial += chr(value & 0xFF)
            return
        if port == 0x3C8:
            self.dac_idx = value & 0xFF; self.dac_state = 0
        elif port == 0x3C9:
            self.dac[self.dac_idx][self.dac_state] = value & 0x3F
            self.dac_state += 1
            if self.dac_state >= 3:
                self.dac_state = 0
                self.dac_idx = (self.dac_idx + 1) & 0xFF
        elif port == 0x70:
            self.cmos_idx = value & 0x7F
        return 0


    # ---------------- ACPI 表（P14 用） ----------------
    def write_acpi_tables(self):
        """在 0xE0000 区域放 RSDP/XSDT/FADT/MADT，供内核解析。"""
        import struct as _st

        def chk(buf):
            b = bytearray(buf)
            b[9] = (-sum(b)) & 0xFF if False else (256 - (sum(b) & 0xFF)) & 0xFF
            return bytes(b)

        def sdt(sig, length, body):
            b = bytearray(36 + len(body))
            b[0:4] = sig
            _st.pack_into('<I', b, 4, length)
            b[8] = 2                     # revision
            b[10:16] = b'UMDBIO'
            b[16:24] = b'UMDOSTBL'
            b[36:] = body
            b[9] = (256 - (sum(b[:9]) + sum(b[10:]))) & 0xFF
            return bytes(b)

        # FADT：pm1a_cnt_blk=0x604，RESET_REG 支持（flags bit10），RESET_REG 在 +0x74
        fadt = bytearray(0xF4)
        _st.pack_into('<I', fadt, 0x40, 0x604)      # PM1a_CNT_BLK
        _st.pack_into('<I', fadt, 0x88, 1 << 10)    # flags: RESET_REG_SUP
        fadt[0x74] = 1                              # address space = I/O
        _st.pack_into('<I', fadt, 0x74 + 4, 0xCF9)  # reset port
        fadt[0x74 + 12] = 0x06                      # reset value
        fadt_blob = sdt(b'FACP', 0xF4, bytes(fadt))

        # MADT：两条 LAPIC 记录
        madt_body = bytearray(8)
        _st.pack_into('<I', madt_body, 0, 0xFEE00000)
        rec = bytes([0, 8, 0, 1]) + bytes(4)
        madt_blob = sdt(b'APIC', 44 + len(madt_body) + 2 * len(rec),
                        bytes(madt_body) + rec + rec)

        # XSDT 指向 FADT/MADT
        xsdt_body = _st.pack('<QQ', 0xE3000, 0xE4000)
        xsdt_blob = sdt(b'XSDT', 36 + len(xsdt_body), xsdt_body)

        # RSDP v2
        rsdp = bytearray(36)
        rsdp[0:8] = b'RSD PTR '
        rsdp[9:15] = b'UMDBIO'
        rsdp[15] = 2
        _st.pack_into('<I', rsdp, 16, 0)            # RSDT 不使用
        _st.pack_into('<I', rsdp, 20, 36)
        _st.pack_into('<Q', rsdp, 24, 0xE2000)      # XSDT
        rsdp[8] = (256 - (sum(rsdp[:8]) + sum(rsdp[9:]))) & 0xFF
        rsdp[32] = (256 - (sum(rsdp[:32]) + sum(rsdp[33:]))) & 0xFF

        self.mu.mem_write(0xE0000, bytes(rsdp))
        self.mu.mem_write(0xE2000, xsdt_blob)
        self.mu.mem_write(0xE3000, fadt_blob)
        self.mu.mem_write(0xE4000, madt_blob)

    # ---------------- 中断 ----------------
    def on_int(self, mu, intno, ud):
        self.int_log.append((intno, self.get16("ip"), self.get16("ax")))
        if intno == 0x10: self.bios_video()
        elif intno == 0x13: self.bios_disk()
        elif intno == 0x16: self.bios_kbd()
        elif intno == 0x15: self.bios_mem()              # E820 / 延时
        elif intno == 0x1A:
            self.cf(True); self.set16("ax", 0x0000)      # 无 PXE
        else:
            pass
        if self.clobber_es:                               # 模拟不守规矩的 BIOS
            self.set16("es", 0xDEAD)

    def bios_mem(self):
        """INT 15h：E820 内存图（单条 64MB 可用 + 低端保留），其余功能立即返回。"""
        if self.get16("ax") != 0xE820:
            self.set16("ax", 0x8600)
            return
        # 固定内存图：0-0x9FC00 可用, 0xF0000-0x100000 保留, 0x100000-64MB 可用
        table = [
            (0x000000, 0x09FC00, 1),
            (0x0F0000, 0x010000, 2),
            (0x100000, 0x3F00000, 1),
        ]
        idx = self.get16("bx")
        if idx >= len(table):
            self.cf(True)
            self.set16("ax", 0)
            return
        base, length, typ = table[idx]
        seg = self.get16("es"); di = self.get16("di")
        addr = (seg << 4) + di
        self.mu.mem_write(addr, struct.pack("<QQII", base, length, typ, 1))
        self.set16("bx", idx + 1)
        self.mu.reg_write(UC_X86_REG_EAX, 0x534D4150)   # EAX = "SMAP"
        self.set16("cx", 24)
        self.cf(False)

    def bios_video(self):
        ah = self.ah()
        if ah == 0x00:
            self.mode = self.al()
            if self.mode == 0x13:
                self.dac = default_palette()
                self.mu.mem_write(0xA0000, b"\x00" * 64000)
            self.set16("ax", 0)
        elif ah == 0x0E:
            ch = chr(self.al())
            if ch == "\r": self.text.append("\n")
            elif ch == "\n": pass
            elif ch == "\x08":
                if self.text: self.text.pop()
            else: self.text.append(ch)
            self.set16("ax", self.get16("ax"))
        elif ah == 0x4F:
            al = self.al()
            if al == 0x00: self.vbe_info()
            elif al == 0x01: self.vbe_mode_info()
            elif al == 0x02: self.vbe_set_mode()
            else: self.set_ax(0x014F)
        else:
            self.set_ax(0x0100)

    def vbe_info(self):
        base = (self.get16("es") << 4) + self.get16("di")
        w = bytearray(512)
        w[0:4] = b"VBE2"
        struct.pack_into("<H", w, 4, 0x0300)
        struct.pack_into("<HH", w, 0x0E, 0xE400, 0x0000)   # VideoModePtr
        struct.pack_into("<H", w, 0x12, 0x0100)
        self.mu.mem_write(base, bytes(w))
        modes = b"".join(struct.pack("<H", m) for m, _, _ in self.vbe_modes) + b"\xFF\xFF"
        self.mu.mem_write(0xE400, modes)
        self.set_ax(0x004F)

    def vbe_mode_info(self):
        cx = self.get16("cx") & 0x3FFF
        base = (self.get16("es") << 4) + self.get16("di")
        info = dict((m, (x, y)) for m, x, y in self.vbe_modes).get(cx)
        if not info:
            self.set_ax(0x014F)
            return
        x, y = info
        w = bytearray(256)
        struct.pack_into("<H", w, 0x00, 0x009B)
        struct.pack_into("<H", w, 0x10, x * 4)
        struct.pack_into("<H", w, 0x12, x)
        struct.pack_into("<H", w, 0x14, y)
        w[0x19] = 32
        struct.pack_into("<I", w, 0x28, self.fb)
        self.mu.mem_write(base, bytes(w))
        self.set_ax(0x004F)

    def vbe_set_mode(self):
        cx = self.get16("bx") & 0x3FFF
        info = dict((m, (x, y)) for m, x, y in self.vbe_modes).get(cx)
        if not info:
            self.set_ax(0x014F); return
        self.xres, self.yres = info
        self.stride_bytes = self.xres * 4
        self.mode = 0x13
        self.set_ax(0x004F)

    def bios_disk(self):
        ah = self.ah()
        if ah == 0x08:
            self.set16("cx", (18 & 0x3F) | 0)
            self.set16("dx", (1 << 8) | self.drive)
            self.set16("ax", 0x0000)
            self.cf(False); return
        if ah == 0x42:
            if not self.edd:
                self.set16("ax", 0x0100); self.cf(True); return
            base = (self.get16("ds") << 4) + self.get16("si")
            raw = bytes(self.mu.mem_read(base, 16))
            cnt = struct.unpack_from("<H", raw, 2)[0]
            off, seg = struct.unpack_from("<HH", raw, 4)
            lba = struct.unpack_from("<Q", raw, 8)[0]
            data = bytes(self.img[lba*512:(lba+cnt)*512])
            if len(data) != cnt*512 or lba*512 + len(data) > len(self.img):
                self.set16("ax", 0x0900); self.cf(True); return
            self.mu.mem_write((seg << 4) + off, data)
            self.set16("ax", 0x0000); self.cf(False); return
        if ah == 0x43:
            if not self.edd or self.readonly:
                self.set16("ax", 0x0300); self.cf(True); return
            base = (self.get16("ds") << 4) + self.get16("si")
            raw = bytes(self.mu.mem_read(base, 16))
            cnt = struct.unpack_from("<H", raw, 2)[0]
            off, seg = struct.unpack_from("<HH", raw, 4)
            lba = struct.unpack_from("<Q", raw, 8)[0]
            data = bytes(self.mu.mem_read((seg << 4) + off, cnt*512))
            if lba*512 + len(data) <= len(self.img):
                self.img[lba*512:lba*512+len(data)] = data
            self.set16("ax", 0x0000); self.cf(False); return
        if ah == 0x02:
            cnt = self.al() or 1
            cx = self.get16("cx"); dx = self.get16("dx")
            sector = cx & 0x3F
            cyl = ((cx >> 8) & 0xFF) | ((cx & 0xC0) << 2)
            head = (dx >> 8) & 0xFF
            lba = (cyl * 2 + head) * 18 + (sector - 1)
            data = bytes(self.img[lba*512:(lba+cnt)*512])
            if len(data) != cnt*512:
                self.set16("ax", 0x0C00); self.cf(True); return
            self.mu.mem_write((self.get16("es") << 4) + self.get16("bx"), data)
            self.set16("ax", cnt); self.cf(False); return
        self.set16("ax", 0x0100); self.cf(True)

    def bios_kbd(self):
        ah = self.ah()
        if self.debug_kbd:
            print("   int16 ah=%02X keys=%d ip=0x%04X" % (ah, len(self.keys), self.get16("ip")))
        if ah == 0x01:
            if self.keys:
                self.set16("ax", (0x02 << 8) | self.keys[0]); self.set_zf(False)
            else:
                self.set16("ax", 0); self.set_zf(True)
        elif ah == 0x00:
            if not self.keys:
                raise NeedKey()
            k = self.keys.pop(0)
            self.set16("ax", (0x02 << 8) | k)
        else:
            self.set16("ax", 0)

    # ---------------- 运行 ----------------
    def run_until(self, target, limit=400_000_000):
        # Unicorn 把 HLT 当成“停机”并返回，所以每次 hlt 后要续跑
        for _ in range(500):
            ip = self.get16("ip")
            if ip == target:
                return "ok"
            try:
                self.mu.emu_start(ip, target, count=limit)
            except NeedKey:
                return "needkey"
            except UcError as e:
                print("   emulation error:", e, "ip=0x%04X cs=0x%04X"
                      % (self.get16("ip"), self.get16("cs")))
                print("   last ints:", self.int_log[-12:])
                return "error"
            if self.get16("ip") == target:
                return "ok"
            if not self.keys:
                return "idle"
        return "timeout"

    def shot_vga(self, path):
        data = bytes(self.mu.mem_read(0xA0000, 64000))
        arr = np.frombuffer(data, np.uint8).reshape(200, 320)
        pal = np.array(self.dac, dtype=np.uint8)
        rgb = pal[arr]
        Image.fromarray(rgb).resize((960, 600), Image.NEAREST).save(path)
        return path

    def shot_fb(self, path, mu):
        h, stride = self.yres, self.stride_bytes
        raw = bytes(mu.mem_read(self.fb, stride*h))
        arr = np.frombuffer(raw, np.uint8).reshape(h, stride//4, 4)
        arr = arr[:, :self.xres, 2::-1]     # BGRX -> RGB
        Image.fromarray(arr).save(path)
        return path


def keycodes(s):
    return [ord(c) for c in s]

def main():
    m = Machine(edd=False, drive=0x00, readonly=False)   # 模拟软盘 + CHS
    m.cmos = {0x00: 0x56, 0x02: 0x34, 0x04: 0x12, 0x0A: 0x00, 0x0B: 0x02}   # BCD + 24h
    m.cmos_idx = 0

    idle = list_addr(os.path.join(BIN, "head.lst"), lambda s: s.rstrip().endswith("hlt"), 0x8800)[0]
    farj, _ = list_addr(os.path.join(BIN, "firstmain.lst"), lambda s: "jmp 0x08:long64" in s, 0xC000)
    long64, _ = list_addr(os.path.join(BIN, "firstmain.lst"), lambda s: "long64:" in s, 0xC000)

    # 1) boot 的用户名/密码 + 激活码
    m.keys = keycodes("umdos\rpass1234\rUMDOS2024\r")
    r = m.run_until(idle)
    print("phase1 (boot/loader/head UI):", r, "ip=0x%04X" % m.get16("ip"))
    m.shot_vga(os.path.join(OUT, "1-login.png"))
    print("  text:", "".join(m.text)[-200:].replace("\n", " | "))

    # 2) 登录
    m.keys = keycodes("umdos\tpass1234\r")
    r = m.run_until(0xA000)
    print("phase2 (login -> hello):", r, "ip=0x%04X" % m.get16("ip"))
    m.shot_vga(os.path.join(OUT, "2-login-ok.png"))

    # 3) hello + firstmain 到长模式跳转前
    r = m.run_until(farj)
    print("phase3 (hello + firstmain):", r, "ip=0x%04X" % m.get16("ip"))
    m.shot_vga(os.path.join(OUT, "3-hello.png"))

    # 4) 切 64 位
    mem = bytes(m.mu.mem_read(0, 0x100000))
    mu = Uc(UC_ARCH_X86, UC_MODE_64)
    mu.mem_map(0, 0x100000)
    mu.mem_map(m.fb, 0x1000000)
    mu.mem_write(0, mem)
    for r_, v in ((UC_X86_REG_CS,0x08),(UC_X86_REG_DS,0),(UC_X86_REG_ES,0),
                  (UC_X86_REG_SS,0),(UC_X86_REG_RSP,0x90000),(UC_X86_REG_RIP,long64),
                  (UC_X86_REG_CR0,0x80000001),(UC_X86_REG_CR3,0x1000),(UC_X86_REG_CR4,0x20),
                  (UC_X86_REG_EFLAGS,2)):
        mu.reg_write(r_, v)
    # Unicorn 的 GDTR 写入不保留基址，且长模式下 DS/ES/SS 已无实际作用，
    # 因此这里不需要装载 GDT。
    try:
        mu.emu_start(long64, 0, count=80_000_000)
        ok = True
    except UcError as e:
        ok = False
        print("  64-bit error:", e, "rip=0x%X" % mu.reg_read(UC_X86_REG_RIP))
    print("phase4 (long mode -> C kernel):", "ok" if ok else "FAILED",
          "rip=0x%X" % mu.reg_read(UC_X86_REG_RIP))
    m.shot_fb(os.path.join(OUT, "4-kernel.png"), mu)
    print("screenshots in", OUT)
    return 0 if (r == "ok" and ok) else 1

if __name__ == "__main__":
    sys.exit(main())
