#!/usr/bin/env python3
"""把 AP 跳板（bin/smp.tramp）嵌入内核：生成 src/kernel/smptramp.h"""
import sys

data = open(sys.argv[1], "rb").read()
out = ["/* 自动生成：AP 启动跳板（P15），来源 bin/smp.tramp */",
       "#ifndef SMPTRAMP_H", "#define SMPTRAMP_H", "",
       "static const unsigned char smp_tramp_blob[] = {"]
for i in range(0, len(data), 16):
    out.append("    " + ", ".join("0x%02X" % b for b in data[i:i + 16]) + ",")
out += ["};", "", "static const unsigned int smp_tramp_len = %d;" % len(data), "", "#endif", ""]
open(sys.argv[2], "w", encoding="utf-8", newline="\n").write("\n".join(out))
print("  smptramp.h: %d bytes" % len(data))
