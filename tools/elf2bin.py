#!/usr/bin/env python3
"""把 ELF64 可执行文件转成平铺二进制（按链接地址摆放，跳过 .bss）。

用法: python tools/elf2bin.py kernel.elf kernel.bin
"""
import struct
import sys


def convert(src, dst):
    data = open(src, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit(f"{src}: not an ELF64 file")
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]

    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = struct.unpack_from("<I", data, off)[0]
        if p_type != 1:            # PT_LOAD
            continue
        p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack_from("<QQQQQ", data, off + 8)
        if p_filesz:
            segs.append((p_vaddr, p_offset, p_filesz, p_memsz))
    if not segs:
        raise SystemExit("no loadable segments")

    base = min(s[0] for s in segs)
    end = max(s[0] - base + s[2] for s in segs)
    out = bytearray(end)
    for vaddr, off, filesz, _ in segs:
        out[vaddr - base:vaddr - base + filesz] = data[off:off + filesz]
    open(dst, "wb").write(out)
    bss_end = max(s[0] - base + s[3] for s in segs)
    print(f"  elf2bin: base=0x{base:X} file={len(out)} bss_end={bss_end}")
    return base, len(out), bss_end


if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2])
