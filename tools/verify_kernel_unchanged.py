#!/usr/bin/env python3
"""dev.1 内核源码未改动的比对脚本。

读取 docs/audit.md 附录 A 的基线，逐个重算当前文件的 SHA-256，输出比对结果，
并生成 releases/<版本>/KERNEL-UNCHANGED.sha256 清单。
"""
import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_baseline():
    text = open(os.path.join(ROOT, "docs", "audit.md"), encoding="utf-8").read()
    i = text.find("附录 A")
    block = text[i:text.find("```", text.find("```", i) + 3)] if i >= 0 else ""
    out = []
    for line in block.splitlines():
        m = re.match(r"^([A-Za-z0-9_./-]+)\s+([0-9a-f]{64})\s*$", line.strip())
        if m:
            out.append((m.group(1), m.group(2)))
    return out


def main():
    base = read_baseline()
    if not base:
        print("no baseline found in docs/audit.md")
        return 1
    version = open(os.path.join(ROOT, "VERSION"), encoding="utf-8").read().strip()
    lines = []
    bad = 0
    for rel, want in base:
        path = os.path.join(ROOT, rel.replace("/", os.sep))
        got = hashlib.sha256(open(path, "rb").read()).hexdigest() if os.path.exists(path) else "MISSING"
        status = "same" if got == want else "CHANGED"
        if got != want:
            bad += 1
        lines.append("%s  %s  %s" % (got, status, rel))
        print("%-10s %s" % (status, rel))
    out_dir = os.path.join(ROOT, "releases", version)
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "KERNEL-UNCHANGED.sha256")
    header = ("# dev.1 内核源码未改动证明\n"
              "# 基线来自 docs/audit.md 附录 A（审计时记录），下面是当前文件的 SHA-256\n"
              "# 格式: <sha256>  <same|CHANGED>  <path>\n")
    open(out, "w", encoding="utf-8", newline="\n").write(header + "\n".join(lines) + "\n")
    print("\n%d/%d files identical, written %s" % (len(base) - bad, len(base), out))
    return 0 if bad == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
