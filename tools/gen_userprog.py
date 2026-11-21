#!/usr/bin/env python3
"""把用户态 ELF 嵌入内核：生成 src/kernel/userprog.h

用法: python tools/gen_userprog.py bin/user.elf src/kernel/userprog.h
"""
import sys


def main(src, dst):
    data = open(src, "rb").read()
    out = ["/* 自动生成：内嵌用户态程序（ELF64），来源 %s */" % src.replace("\\", "/"),
           "#ifndef USERPROG_H", "#define USERPROG_H", "",
           "#include <stdint.h>", "",
           "static const unsigned char user_elf[] = {"]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        out.append("    " + ", ".join("0x%02X" % b for b in chunk) + ",")
    out += ["};", "",
            "static const unsigned int user_elf_len = %d;" % len(data),
            "", "#endif", ""]
    open(dst, "w", encoding="utf-8", newline="\n").write("\n".join(out))
    print("  userprog.h: %d bytes embedded" % len(data))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
