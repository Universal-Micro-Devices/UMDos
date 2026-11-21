#!/usr/bin/env python3
"""驱动 64 位 C 内核的图形界面：启动画面 -> 登录 -> 桌面 -> 文件管理器，并截图。"""
import os, sys, time, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emu
from emu import Machine, keycodes, list_addr, OUT
from unicorn import (Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_INSN, UC_HOOK_INTR,
                     UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE,
                     UC_HOOK_CODE, UcError)
from unicorn.x86_const import *
from PIL import Image
import numpy as np

BIN = emu.BIN
MARK = {}

class Marker(Exception):
    pass

# ---------------------------------------------------------------- 扫描码
SC = {
    '1': 0x02, '2': 0x03, '3': 0x04, '4': 0x05, '5': 0x06, '6': 0x07,
    '7': 0x08, '8': 0x09, '9': 0x0A, '0': 0x0B,
    '-': 0x0C, '=': 0x0D, '\b': 0x0E, '\t': 0x0F,
    'q': 0x10, 'w': 0x11, 'e': 0x12, 'r': 0x13, 't': 0x14, 'y': 0x15,
    'u': 0x16, 'i': 0x17, 'o': 0x18, 'p': 0x19, '[': 0x1A, ']': 0x1B,
    '\n': 0x1C, '\r': 0x1C,
    'a': 0x1E, 's': 0x1F, 'd': 0x20, 'f': 0x21, 'g': 0x22, 'h': 0x23,
    'j': 0x24, 'k': 0x25, 'l': 0x26, ';': 0x27, "'": 0x28, '`': 0x29,
    '\\': 0x2B,
    'z': 0x2C, 'x': 0x2D, 'c': 0x2E, 'v': 0x2F, 'b': 0x30, 'n': 0x31,
    'm': 0x32, ',': 0x33, '.': 0x34, '/': 0x35, ' ': 0x39,
    '\x1b': 0x01,
}


class Kernel:
    def __init__(self, mem, fb_base=0xFD000000, w=1280, h=720, disk_bytes=None):
        self.mu = Uc(UC_ARCH_X86, UC_MODE_64)
        self.mu.mem_map(0, 64 * 1024 * 1024)   # P4：映射 64MB 物理内存
        self.mu.mem_map(fb_base, 0x1000000)
        # P16: AHCI controller MMIO + PCI config
        self.ahci_base = 0xFE000000
        self.mu.mem_map(self.ahci_base, 0x2000)
        self.ahci_regs = bytearray(0x2000)
        self.pci_addr = 0
        self._ahci_init_regs()
        self._ahci_sync_all()
        self.mu.hook_add(UC_HOOK_MEM_WRITE, self.on_ahci_write, None,
                         self.ahci_base, self.ahci_base + 0x2000)
        self.mu.mem_write(0, mem)
        # bootinfo.env_flags(+112) bit0：模拟器没有 GDT/IDT 模型，让内核跳过 ring3
        self.mu.mem_write(0x7000 + 112, struct.pack("<I", 1))
        self.fb = fb_base
        self.w, self.h = w, h
        self.queue = []
        self.cmos_idx = 0
        self.vsec = 10
        self.keys = []
        for r, v in ((UC_X86_REG_CS, 0x08), (UC_X86_REG_DS, 0), (UC_X86_REG_ES, 0),
                     (UC_X86_REG_SS, 0), (UC_X86_REG_RSP, 0x90000),
                     (UC_X86_REG_CR0, 0x80000001), (UC_X86_REG_CR3, 0x1000),
                     (UC_X86_REG_CR4, 0x20), (UC_X86_REG_EFLAGS, 2)):
            self.mu.reg_write(r, v)
        self.mu.hook_add(UC_HOOK_INSN, self.on_in, object(), 1, 0, UC_X86_INS_IN)
        self.mu.hook_add(UC_HOOK_INTR, self.on_intr)
        self.mu.hook_add(UC_HOOK_INSN, self.on_out, object(), 1, 0, UC_X86_INS_OUT)
        self.log = []
        self.serial = ""
        self.powered_off = False
        self.rebooted = False
        if disk_bytes is None:
            self.img = bytearray(open(emu.IMG, "rb").read())
        else:
            self.img = bytearray(disk_bytes)
        self.img2 = bytearray(2048 * 512)
        self.img2[0:8] = b"UMD2SATA"
        self.img2[510] = 0x55
        self.img2[511] = 0xAA
        self.ata_lba = 0
        self.ata_cnt = 1
        self.ata_buf = b''
        self.ata_pos = 0
        self.ata_drq = False
        self.ata_write = False
        self.ata_master = 1

    # ---- 中断投递（Unicorn 不模拟 IDT 中断，这里按门描述符手工压栈跳转） ----
    def idt_base(self):
        return struct.unpack_from("<I", self.mu.mem_read(0x706C, 4), 0)[0]

    def deliver_int(self, vec):
        base = self.idt_base()
        if not base:
            return False
        g = bytes(self.mu.mem_read(base + vec * 16, 16))
        off_lo = g[0] | (g[1] << 8)
        off_mid = g[6] | (g[7] << 8)
        off_hi = struct.unpack_from("<I", g, 8)[0]
        target = off_lo | (off_mid << 16) | (off_hi << 32)
        if not target:
            return False
        rsp = self.mu.reg_read(UC_X86_REG_RSP)
        rflags = self.mu.reg_read(UC_X86_REG_EFLAGS)
        cs = self.mu.reg_read(UC_X86_REG_CS)
        rip = self.mu.reg_read(UC_X86_REG_RIP)
        if cs & 3:
            # ring3 -> ring0：切到内核栈，并按 CPU 的顺序压 SS/RSP/RFLAGS/CS/RIP
            ss = self.mu.reg_read(UC_X86_REG_SS)
            old_rsp = rsp
            rsp = 0x90000
            for val in (ss, old_rsp, rflags, cs, rip):
                rsp -= 8
                self.mu.mem_write(rsp, struct.pack("<Q", val))
            self.mu.reg_write(UC_X86_REG_SS, 0x10)
            self.mu.reg_write(UC_X86_REG_CS, 0x08)   # 中断门把 CPL 切回 ring0
        else:
            for val in (rflags, cs, rip):
                rsp -= 8
                self.mu.mem_write(rsp, struct.pack("<Q", val))
        self.mu.reg_write(UC_X86_REG_RSP, rsp)
        self.mu.reg_write(UC_X86_REG_RIP, target)
        return True

    def on_intr(self, mu, intno, ud):
        self.deliver_int(intno)

    # ---- I/O ----
    def ata_command(self, cmd):
        if cmd == 0xEC:                                  # IDENTIFY
            if not self.ata_master:
                self.ata_buf = b''
                self.ata_drq = False
                self.ata_write = False
                return
            idb = bytearray(512)
            struct.pack_into("<H", idb, 0, 0x0040)
            secs = len(self.img) // 512
            struct.pack_into("<H", idb, 120, secs & 0xFFFF)
            struct.pack_into("<H", idb, 122, (secs >> 16) & 0xFFFF)
            self.ata_buf = bytes(idb)
            self.ata_pos = 0
            self.ata_drq = True
            self.ata_write = False
        elif cmd == 0x20:                                # READ SECTORS
            off = self.ata_lba * 512
            self.ata_buf = bytes(self.img[off:off + self.ata_cnt * 512])
            self.ata_pos = 0
            self.ata_drq = True
            self.ata_write = False
        elif cmd == 0x30:                                # WRITE SECTORS
            self.ata_buf = bytearray(self.ata_cnt * 512)
            self.ata_pos = 0
            self.ata_drq = True
            self.ata_write = True
        elif cmd == 0xE7:                                # FLUSH CACHE
            self.ata_drq = False
            self.ata_write = False

    def ata_commit(self):
        off = self.ata_lba * 512
        self.img[off:off + len(self.ata_buf)] = self.ata_buf
        self.ata_drq = False
        self.ata_write = False
        self.ata_master = 1


    # ---------------- P16: AHCI emulation ----------------
    def _ahci_init_regs(self):
        r = self.ahci_regs

        def put(off, val, size=4):
            for i in range(size):
                r[off + i] = (val >> (8 * i)) & 0xFF

        put(0x00, 0x0F000001)        # CAP: 2 ports (CAP[4:0]=1)
        put(0x04, 0x80000000)        # GHC: AE
        put(0x0C, 0x00000003)        # PI: ports 0 and 1
        put(0x10, 0x00010300)        # VS
        for p in (0, 1):
            port = 0x100 + p * 0x80
            put(port + 0x24, 0x00000101)   # PxSIG: SATA
            put(port + 0x28, 0x00000123)   # PxSSTS: present, DET=3
            put(port + 0x20, 0x00000050)   # PxTFD: ready

    def _get(self, off, size):
        v = 0
        for i in range(size):
            v |= self.ahci_regs[off + i] << (8 * i)
        return v

    def _put(self, off, size, val):
        for i in range(size):
            self.ahci_regs[off + i] = (val >> (8 * i)) & 0xFF
        try:
            self.mu.mem_write(self.ahci_base + off, bytes(self.ahci_regs[off:off + size]))
        except Exception:
            pass

    def _ahci_sync_all(self):
        try:
            self.mu.mem_write(self.ahci_base, bytes(self.ahci_regs))
        except Exception:
            pass

    def _ahci_img(self, port):
        return self.img if port == 0 else self.img2

    def _identify(self, port):
        d = bytearray(512)

        def w(i, v):
            d[i * 2] = v & 0xFF
            d[i * 2 + 1] = (v >> 8) & 0xFF

        w(0, 0x0040)
        w(1, 0x3FFF)
        w(3, 16)
        w(6, 63)
        model = (b"UMDos Virtual SATA" if port == 0 else b"UMDos Virtual SATA B").ljust(40)
        for i in range(20):
            w(27 + i, (model[i * 2] << 8) | model[i * 2 + 1])
        w(49, 0x2F00)
        sec = len(self._ahci_img(port)) // 512
        w(60, sec & 0xFFFF)
        w(61, (sec >> 16) & 0xFFFF)
        w(100, sec & 0xFFFF)
        w(101, (sec >> 16) & 0xFFFF)
        w(102, 0)
        w(103, 0)
        return bytes(d)

    def _ahci_exec(self, port):
        base = 0x100 + port * 0x80
        clb = self._get(base, 4) | (self._get(base + 4, 4) << 32)
        if not clb:
            return
        ctba = int.from_bytes(bytes(self.mu.mem_read(clb, 8)), "little")
        if not ctba:
            return
        cfis = bytes(self.mu.mem_read(ctba, 64))
        cmd = cfis[2]
        lba = (cfis[4] | (cfis[5] << 8) | (cfis[6] << 16) |
               (cfis[8] << 24) | (cfis[9] << 32))
        count = cfis[12] | (cfis[13] << 8)
        if count == 0:
            count = 1
        dba = int.from_bytes(bytes(self.mu.mem_read(ctba + 0x80, 8)), "little")
        dbc = int.from_bytes(bytes(self.mu.mem_read(ctba + 0x88, 4)), "little") & 0x3FFFFF
        n = (dbc + 1) if dbc else count * 512
        img = self._ahci_img(port)
        cap = len(img) // 512
        if cmd in (0x25, 0x35) and (lba + count > cap):
            self._put(base + 0x38, 4, 0)
            self._put(base + 0x20, 4, 0x51)     # PxTFD: ERR
            return
        if cmd == 0xEC:
            self.mu.mem_write(dba, self._identify(port))
        elif cmd == 0x25:
            data = bytes(img[lba * 512:lba * 512 + n])
            if data:
                self.mu.mem_write(dba, data)
        elif cmd == 0x35:
            data = bytes(self.mu.mem_read(dba, n))
            img[lba * 512:lba * 512 + len(data)] = data
        self._put(base + 0x38, 4, 0)        # PxCI clear
        self._put(base + 0x20, 4, 0x50)     # PxTFD ready

    def on_ahci_write(self, mu, access, addr, size, value, ud):
        off = addr - self.ahci_base
        self._put(off, size, value)
        if off == 0x04 and (value & 1):
            self._put(0x04, 4, self._get(0x04, 4) & ~1)     # HR self-clear
        for p in (0, 1):
            base = 0x100 + p * 0x80
            if off == base + 0x38 and (value & 1):
                try:
                    self._ahci_exec(p)
                    self._put(base + 0x10, 4, 0x8)          # PxIS done
                except Exception as e:
                    print("[harness] ahci exec error port", p, e)

    def _pci_cfg(self):
        addr = self.pci_addr
        bus = (addr >> 16) & 0xFF
        dev = (addr >> 11) & 0x1F
        func = (addr >> 8) & 0x07
        off = addr & 0xFC
        if (bus, dev, func) != (0, 0, 0):
            return 0xFFFFFFFF
        regs = {
            0x00: 0x29228086,       # vendor 0x8086 device 0x2922
            0x04: 0x00000000,
            0x08: 0x01060100,       # class 01 / subclass 06 / prog-if 01
            0x24: 0xFE000000,       # BAR5
            0x3C: 0x0000000B,
        }
        return regs.get(off, 0)

    def on_in(self, mu, port, size, ud):
        if port == 0xCFC:
            return self._pci_cfg()
        if port == 0xCF8:
            return self.pci_addr
        if port == 0x1F0:
            if self.ata_pos + 2 <= len(self.ata_buf):
                w = self.ata_buf[self.ata_pos] | (self.ata_buf[self.ata_pos + 1] << 8)
                self.ata_pos += 2
                if self.ata_pos >= len(self.ata_buf):
                    self.ata_drq = False
                return w
            return 0
        if port in (0x1F7, 0x3F6):
            return 0x40 | (0x08 if self.ata_drq else 0x00)
        if port == 0x1F1:
            return 0
        if port == 0x64:
            if not self.queue:
                return 0
            return 0x01 | (0x20 if self.queue[0][0] == 'mouse' else 0)
        if port == 0x60:
            if not self.queue:
                return 0
            return self.queue.pop(0)[1]
        if port == 0x71:
            idx = self.cmos_idx
            if idx == 0x00:
                v = self.vsec
                self.vsec = (self.vsec + 1) % 60
                return ((v // 10) << 4) | (v % 10)
            if idx == 0x02:
                return 0x34
            if idx == 0x04:
                return 0x12
            if idx == 0x0A:
                return 0
            if idx == 0x0B:
                return 0x02
            return 0
        return 0xFF

    def on_out(self, mu, port, size, value, ud):
        if port == 0xCF8:
            self.pci_addr = value
            return
        if port == 0xCFC:
            return
        # P14：观察关机/复位端口（测试用）
        if port in (0x604, 0xB004) and value == 0x2000:
            self.powered_off = True
        if port == 0x64 and value == 0xFE:
            self.rebooted = True
        if port == 0xCF9:
            self.rebooted = True
        if port == 0x3F8:
            self.serial += chr(value & 0xFF)
            return
        if port == 0x1F6:
            self.ata_master = 0 if (value & 0x10) else 1
            self.ata_lba = (self.ata_lba & 0x00FFFFFF) | ((value & 0x0F) << 24)
        elif port == 0x1F2:
            self.ata_cnt = (value & 0xFF) or 256
        elif port == 0x1F3:
            self.ata_lba = (self.ata_lba & 0xFFFF00) | (value & 0xFF)
        elif port == 0x1F4:
            self.ata_lba = (self.ata_lba & 0xFF00FF) | ((value & 0xFF) << 8)
        elif port == 0x1F5:
            self.ata_lba = (self.ata_lba & 0x00FFFF) | ((value & 0xFF) << 16)
        elif port == 0x1F7:
            self.ata_command(value)
        elif port == 0x1F0:
            if self.ata_write and self.ata_pos < len(self.ata_buf):
                self.ata_buf[self.ata_pos] = value & 0xFF
                if size >= 2:
                    self.ata_buf[self.ata_pos + 1] = (value >> 8) & 0xFF
                self.ata_pos += size
                if self.ata_pos >= len(self.ata_buf):
                    self.ata_commit()
        if port == 0x70:
            self.cmos_idx = value & 0x7F
        if port == 0xE9:
            self.log.append(value)
            raise Marker(value)

    # ---- 输入 ----
    EXT = {'up': 0x48, 'down': 0x50, 'left': 0x4B, 'right': 0x4D,
           'home': 0x47, 'end': 0x4F, 'delete': 0x53, 'pgup': 0x49, 'pgdn': 0x51}

    def ctrl_key(self, ch):
        code = SC[ch]
        self.queue.append(('kbd', 0x1D))
        self.queue.append(('kbd', code))
        self.queue.append(('kbd', code | 0x80))
        self.queue.append(('kbd', 0x9D))

    def shift_key(self, code):
        self.queue.append(('kbd', 0x2A))
        self.queue.append(('kbd', code))
        self.queue.append(('kbd', code | 0x80))
        self.queue.append(('kbd', 0xAA))

    def ctrl_ext(self, code):
        self.queue.append(('kbd', 0x1D))
        self.queue.append(('kbd', 0xE0))
        self.queue.append(('kbd', code))
        self.queue.append(('kbd', 0xE0))
        self.queue.append(('kbd', code | 0x80))
        self.queue.append(('kbd', 0x9D))

    def fkey(self, n):
        code = 0x3B + (n - 1)
        self.queue.append(('kbd', code))
        self.queue.append(('kbd', code | 0x80))

    def key(self, name):
        code = self.EXT[name]
        self.queue.append(('kbd', 0xE0))
        self.queue.append(('kbd', code))
        self.queue.append(('kbd', 0xE0))
        self.queue.append(('kbd', code | 0x80))

    SHIFT = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7',
             '*': '8', '(': '9', ')': '0', '_': '-', '+': '=', '{': '[', '}': ']',
             '|': '\\\\', ':': ';', '"': "'", '<': ',', '>': '.', '?': '/', '~': ''}

    def type_text(self, text):
        for ch in text:
            shift = False
            if ch in self.SHIFT:
                base = self.SHIFT[ch]
                shift = True
            elif ch.isupper():
                base = ch.lower()
                shift = True
            else:
                base = ch
            code = SC.get(base)
            if code is None:
                continue
            if shift:
                self.queue.append(('kbd', 0x2A))
                self.queue.append(('kbd', code))
                self.queue.append(('kbd', code | 0x80))
                self.queue.append(('kbd', 0xAA))
            else:
                self.queue.append(('kbd', code))
                self.queue.append(('kbd', code | 0x80))

    def move_by(self, dx, dy, buttons=0):
        """dx,dy 是屏幕坐标增量（dy 向下为正）；buttons=1 表示按住左键拖动"""
        pdy = -dy
        while dx or pdy:
            sx = max(-127, min(127, dx))
            sy = max(-127, min(127, pdy))
            dx -= sx
            pdy -= sy
            flags = 0x08 | (buttons & 1) | (0x10 if sx < 0 else 0) | (0x20 if sy < 0 else 0)
            self.queue.append(('mouse', flags))
            self.queue.append(('mouse', sx & 0xFF))
            self.queue.append(('mouse', sy & 0xFF))

    def press(self):
        self.queue.append(('mouse', 0x09))
        self.queue.append(('mouse', 0))
        self.queue.append(('mouse', 0))

    def release(self):
        self.queue.append(('mouse', 0x08))
        self.queue.append(('mouse', 0))
        self.queue.append(('mouse', 0))

    def drag_by(self, dx, dy):
        self.press()
        for _ in range(3):
            self.move_by(dx // 3, dy // 3, buttons=1)
        self.release()

    def click(self):
        self.queue.append(('mouse', 0x09))
        self.queue.append(('mouse', 0))
        self.queue.append(('mouse', 0))
        self.queue.append(('mouse', 0x08))
        self.queue.append(('mouse', 0))
        self.queue.append(('mouse', 0))

    # ---- 运行 ----
    def settle(self, chunk=4_000_000, extra=8_000_000):
        """把排队的输入消化完，并让最后一次重绘画完"""
        for _ in range(400):
            self.run(chunk)
            if not self.queue:
                break
        self.run(extra)

    def run(self, budget=150_000_000):
        rip = self.mu.reg_read(UC_X86_REG_RIP)
        try:
            self.mu.emu_start(rip, 0, count=budget)
            return None
        except Marker as m:
            return m.args[0]
        except UcError as e:
            r = self.mu.reg_read(UC_X86_REG_RIP)
            print("   UcError", e, "rip=0x%X" % r,
                  "bytes=" + bytes(self.mu.mem_read(r, 12)).hex())
            raise

    def shot(self, path):
        stride = self.w
        raw = bytes(self.mu.mem_read(self.fb, stride * self.h * 4))
        arr = np.frombuffer(raw, np.uint8).reshape(self.h, stride, 4)
        img = arr[:, :, 2::-1]
        Image.fromarray(img).save(path)


    def run_until(self, wanted, budget=150_000_000, tries=400):
        for _ in range(tries):
            code = self.run(budget)
            if code is None:
                return None
            if code in wanted:
                return code
        return None


def main():
    os.makedirs(os.path.join(emu.ROOT, "shots"), exist_ok=True)
    shots = os.path.join(emu.ROOT, "shots")

    # 1) P2 四级链：boot -> loader -> head -> kernel
    m = Machine(edd=False, drive=0x00)
    m.cmos = {0x00: 0x56, 0x02: 0x34, 0x04: 0x12, 0x0A: 0x00, 0x0B: 0x02}
    m.cmos_idx = 0
    long64, _ = list_addr(os.path.join(BIN, "head.lst"), lambda s: "long64:" in s, 0x9000)
    print("16-bit chain:", m.run_until(long64))
    mem = bytes(m.mu.mem_read(0, 0x100000))

    # 2) 64 位内核
    k = Kernel(mem, fb_base=m.fb, w=m.xres, h=m.yres)
    k.mu.reg_write(UC_X86_REG_RIP, long64)
    print("kernel start")

    print("  marker", hex(k.run_until({0x10}) or 0))
    k.run(3_000_000)
    k.shot(os.path.join(shots, "k1-splash.png"))

    print("  marker", hex(k.run_until({0x14}) or 0))
    print("  marker", hex(k.run_until({0x20}) or 0))
    k.settle()
    k.shot(os.path.join(shots, "k2-login.png"))

    k.type_text("user")
    k.type_text("\t")
    k.type_text("umdos")
    k.type_text("\r")
    print("  marker", hex(k.run_until({0x30}) or 0))
    k.settle()
    k.shot(os.path.join(shots, "k3-desktop.png"))

    # 点击桌面上的“我的电脑”图标（图标在左下角）
    k.type_text("\r")          # desktop Enter opens My Computer
    print("  marker", hex(k.run_until({0x31}) or 0))          # 打开文件管理器窗口
    k.shot(os.path.join(shots, "k4-files.png"))

    # 选中第一行 README.TXT
    k.move_by(200 - 68, 186 - 518)
    k.click()
    print("  marker", hex(k.run_until({0x33}) or 0))          # 选中
    k.shot(os.path.join(shots, "k5-preview.png"))

    # 再点一次同一行 -> 编辑器窗口
    k.click()
    print("  marker", hex(k.run_until({0x34}) or 0))
    k.type_text("Edited by the C kernel.")
    k.shot(os.path.join(shots, "k8-editor.png"))
    k.type_text("\x1b")
    print("  marker", hex(k.run_until({0x35}) or 0))          # 保存并关闭编辑器
    k.shot(os.path.join(shots, "k9-saved.png"))

    # 进子目录 DOCS（第 5 行）
    k.move_by(0, 306 - 186)
    k.click()
    print("  marker", hex(k.run_until({0x33}) or 0))
    k.click()
    print("  marker", hex(k.run_until({0x3A}) or 0))          # 已经进入 DOCS
    k.shot(os.path.join(shots, "k11-subdir.png"))

    # 打开 DOCS 里的第一个文件
    k.move_by(0, 186 - 306)
    k.click()
    print("  marker", hex(k.run_until({0x33}) or 0))
    k.click()
    print("  marker", hex(k.run_until({0x34}) or 0))          # 编辑器
    k.shot(os.path.join(shots, "k12-editor2.png"))
    k.type_text("\x1b")
    print("  marker", hex(k.run_until({0x35}) or 0))

    # Backspace 返回上一级
    k.type_text("\b")
    print("  marker", hex(k.run_until({0x3B}) or 0))          # 回到根目录

    # 再点桌面图标 -> 第二个窗口
    k.move_by(68 - 200, 518 - 186)
    k.click()
    print("  marker", hex(k.run_until({0x31}) or 0))          # 第二个文件管理器
    k.run_until({0x3C})                                        # 吃掉这次点击的松开
    k.shot(os.path.join(shots, "k13-two-windows.png"))

    # 拖动最上面窗口的标题栏
    k.move_by(194 + 300 - 68, 94 + 18 - 518)
    k.drag_by(170, 80)
    print("  marker", hex(k.run_until({0x3C}) or 0))
    k.shot(os.path.join(shots, "k14-drag.png"))

    print("log:", [hex(x) for x in k.log])

    # 直接检查磁盘镜像里的 UMDFS，确认写回真的生效
    lba = 512
    sb = k.img[lba * 512:lba * 512 + 512]
    magic = bytes(sb[0:6])
    block_size, block_count, file_count, data_start, dblock, dcount = \
        struct.unpack_from("<6I", sb, 8)
    print("on-disk fs:", magic, "blocks=", block_count, "files=", file_count)
    for i in range(dcount):
        off = (lba + dblock) * 512 + i * 64
        name = bytes(k.img[off:off + 32]).split(b'\0')[0].decode("ascii", "replace")
        size, first = struct.unpack_from("<II", k.img, off + 32)
        if k.img[off + 40] and name:
            print("   %-12s %4d bytes  block %d" % (name, size, first))
    motd = k.img.find(b"MOTD.TXT")
    if motd >= 0:
        base = (motd // 64) * 64
        size = struct.unpack_from("<I", k.img, base + 32)[0]
        print("MOTD.TXT on disk:", size, "bytes")
    return 0

if __name__ == "__main__":
    sys.exit(main())
