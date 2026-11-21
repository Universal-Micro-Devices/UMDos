#!/usr/bin/env python3
"""校验 uMDos.iso 的 ISO9660 / El Torito 结构和内嵌启动镜像。"""
import os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION = open(os.path.join(ROOT, "VERSION"), encoding="utf-8").read().strip()
ISO = os.path.join(ROOT, "UMDos %s.iso" % VERSION)
IMG = os.path.join(ROOT, "UMDos %s floppy.img" % VERSION)
issues = []


def check(cond, msg):
    print(("[ok] " if cond else "[FAIL] ") + msg)
    if not cond:
        issues.append(msg)


data = open(ISO, "rb").read()
check(len(data) % 2048 == 0, f"ISO 大小按 2048 对齐 ({len(data)} 字节)")


def sec(n):
    return data[n * 2048:(n + 1) * 2048]


pvd = sec(16)
check(pvd[0] == 1 and pvd[1:6] == b"CD001", "LBA16 是主卷描述符 (CD001)")
brvd = sec(17)
check(brvd[0] == 0 and brvd[1:6] == b"CD001", "LBA17 是启动记录卷描述符")
check(brvd[7:39].rstrip(b"\x00") == b"EL TORITO SPECIFICATION", "启动系统标识 = EL TORITO SPECIFICATION")
term = sec(18)
check(term[0] == 255 and term[1:6] == b"CD001", "LBA18 是卷描述符结束符")

cat_lba = struct.unpack_from("<I", brvd, 71)[0]
cat = sec(cat_lba)
check(cat[0] == 1, "启动目录项: 校验项头 = 1")
check(cat[1] == 0, "平台 = x86")
total = sum(struct.unpack_from("<H", cat, i)[0] for i in range(0, 32, 2))
check(total & 0xFFFF == 0, "校验项 16 位累加和 = 0")
check(cat[30:32] == b"\x55\xAA", "校验项签名 0xAA55")

e = cat[32:64]
check(e[0] == 0x88, "启动项可引导 (0x88)")
check(e[1] == 2, "介质类型 = 2 (1.44MB 软盘仿真)")
sc = struct.unpack_from("<H", e, 6)[0]
check(sc in (1, 2880), f"扇区数字段 = {sc}（pycdlib 对软盘仿真写 1，规范允许 2880）")
rba = struct.unpack_from("<I", e, 8)[0]
boot = data[rba * 2048: rba * 2048 + 1474560]
check(len(boot) == 1474560, "启动镜像长度 = 1.44MB")
check(boot == open(IMG, "rb").read(), "ISO 内启动镜像与 uMDos.img 完全一致")
check(boot[510:512] == b"\x55\xAA", "启动镜像带 0xAA55 签名")

# 根目录
root_lba = struct.unpack_from("<I", pvd, 156 + 2)[0]
check(root_lba > 0, f"根目录位于 LBA {root_lba}")
root = sec(root_lba)
names = []
off = 0
while off < 2048 and root[off]:
    ln = root[off]
    if ln == 0:
        break
    nl = root[off + 32]
    name = root[off + 33: off + 33 + nl]
    names.append(name)
    off += ln
print("    根目录条目:", [n.decode("ascii", "replace") for n in names])
check(b"UMDOS.IMG;1" in names, "ISO 内包含 UMDOS.IMG")
check(b"README.TXT;1" in names, "ISO 内包含 README.TXT")

print("\n%s" % ("全部通过" if not issues else f"{len(issues)} 项失败"))
sys.exit(0 if not issues else 1)
