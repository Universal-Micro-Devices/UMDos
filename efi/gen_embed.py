#!/usr/bin/env python3
"""把内核与文件系统镜像内嵌进 EFI 加载器：生成 efi/efi_embed.h"""
import sys


def emit(src, name, out):
    data = open(src, "rb").read()
    out.append("static const u8 %s[] = {" % name)
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        out.append("    " + ", ".join("0x%02X" % b for b in chunk) + ",")
    out.append("};")
    out.append("static const u32 %s_LEN = %d;" % (name, len(data)))
    out.append("")
    return len(data)


def main(kernel, fs, dst):
    lines = ["/* 自动生成：EFI 加载器内嵌镜像（P13） */",
             "#ifndef EFI_EMBED_H", "#define EFI_EMBED_H", ""]
    klen = emit(kernel, "EFI_KERNEL", lines)
    flen = emit(fs, "EFI_FS", lines)
    lines += ["#endif", ""]
    open(dst, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print("  efi_embed.h: kernel %d bytes, fs %d bytes" % (klen, flen))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
