#!/usr/bin/env python3
"""版本号自增工具：只递增 v2.6.112-dev.N 的 dev 序号。

规则（来自项目规格）：
  - 中间版本序列是 v2.6.112-dev.N，N 从 1 开始递增；
  - 不占用最终版本号 v2.6.11 / v2.6.112；
  - 最终交付时由发布流程或人工把 VERSION 改成最终版本号，本脚本不做跨越。

用法：
  python tools/bump_version.py           # 显示当前版本与下一个版本
  python tools/bump_version.py --apply   # 实际写入 VERSION
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION_FILE = os.path.join(ROOT, "VERSION")

PATTERN = re.compile(r"^v2\.6\.112-dev\.(\d+)$")


def read():
    return open(VERSION_FILE, encoding="utf-8").read().strip()


def main(argv):
    cur = read()
    m = PATTERN.match(cur)
    if not m:
        print("当前版本 %r 不是 v2.6.112-dev.N 形式，拒绝自增（最终版本请人工设置）" % cur)
        return 2
    nxt = "v2.6.112-dev.%d" % (int(m.group(1)) + 1)
    print("当前版本:", cur)
    print("下一个版本:", nxt)
    if "--apply" in argv:
        open(VERSION_FILE, "w", encoding="utf-8", newline="\n").write(nxt + "\n")
        print("已写入", VERSION_FILE)
    else:
        print("（未写入；加 --apply 生效）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
