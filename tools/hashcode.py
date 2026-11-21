#!/usr/bin/env python3
"""算出激活码的 SHA-256，并输出可直接粘贴进 src/loader.asm 的 db 行。

用法:  python tools/hashcode.py 你的激活码
"""
import hashlib
import sys

code = sys.argv[1] if len(sys.argv) > 1 else "UMDOS2024"
if len(code) > 55:
    sys.exit("激活码不能超过 55 字节（loader 的 SHA-256 是单块实现）")

digest = hashlib.sha256(code.encode()).digest()
print(f'sha256("{code}") = {digest.hex()}')
print()
print("把下面这些行替换 src/loader.asm 里 expected_activate: 之后的 4 行：")
for i in range(0, 32, 8):
    print("    db " + ", ".join("0x%02X" % b for b in digest[i:i + 8]))
print()
print("然后重新 python build.py")
