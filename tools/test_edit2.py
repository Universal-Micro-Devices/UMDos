#!/usr/bin/env python3
"""第二轮编辑器特性：查找替换、选中/复制粘贴、缩进、文件重命名。"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emu
from test_persist import boot, login_and_open_files, list_fs

SHOTS = os.path.join(emu.ROOT, "shots")
LB = 512


def read_file(img, name):
    dblk = struct.unpack_from("<I", img, LB * 512 + 24)[0]
    for i in range(32):
        off = (LB + dblk) * 512 + i * 64
        nm = bytes(img[off:off + 32]).split(b'\0')[0].decode("ascii", "replace")
        if nm != name:
            continue
        size, first = struct.unpack_from("<II", img, off + 32)
        return bytes(img[(LB + first) * 512:(LB + first) * 512 + size]).decode("ascii", "replace")
    return None


def main():
    k = boot(open(emu.IMG, "rb").read())
    login_and_open_files(k)

    # 选中 MOTD.TXT（第二行）
    k.key("down")
    k.key("down")
    k.settle()

    def open_motd():
        k.type_text("\r")
        print("   editor:", hex(k.run_until({0x34}) or 0))

    def save_close():
        k.type_text("\x1b")
        print("   saved:", hex(k.run_until({0x35}) or 0))

    # ---------------- A. 查找 + 替换 ----------------
    open_motd()
    k.ctrl_key('f')
    k.type_text("bosom")
    k.type_text("\r")              # 定位到匹配
    k.type_text("\x1b")            # 关掉查找栏
    k.settle()
    k.shot(os.path.join(SHOTS, "f1-find.png"))

    k.ctrl_key('h')
    k.type_text("bosom")
    k.type_text("\t")              # 切到替换框
    k.type_text("friend")
    k.type_text("\r")              # 替换当前并找下一个
    k.type_text("\x1b")
    k.settle()
    k.shot(os.path.join(SHOTS, "f2-replace.png"))
    save_close()
    k.settle()
    txt = read_file(k.img, "MOTD.TXT")
    ok_a = txt is not None and "A friend friend afar" in txt
    print("[%s] 查找替换" % ("OK" if ok_a else "FAIL"))

    # ---------------- B. 全选 + 复制 + 粘贴 ----------------
    open_motd()
    k.ctrl_key('a')
    k.ctrl_key('c')
    k.key("end")
    k.type_text("\r")
    k.ctrl_key('v')
    k.settle()
    k.shot(os.path.join(SHOTS, "f3-paste.png"))
    save_close()
    k.settle()
    txt = read_file(k.img, "MOTD.TXT")
    ok_b = txt is not None and txt.count("A friend friend afar") == 2
    print("[%s] 全选/复制/粘贴 (出现 %d 次)" % ("OK" if ok_b else "FAIL",
                                          txt.count("A friend friend afar") if txt else -1))

    # ---------------- C. Tab 缩进 / Shift+Tab 反缩进 ----------------
    open_motd()
    k.ctrl_ext(0x47)                        # Ctrl+Home 到文件开头
    k.type_text("\t")                       # 插入 4 个空格
    k.settle()
    k.shot(os.path.join(SHOTS, "f4-indent.png"))
    save_close()
    k.settle()
    txt = read_file(k.img, "MOTD.TXT")
    ok_c1 = txt is not None and txt.startswith("    A friend friend afar")
    print("[%s] Tab 缩进" % ("OK" if ok_c1 else "FAIL"))

    open_motd()
    k.ctrl_ext(0x47)                        # Ctrl+Home
    k.shift_key(0x0F)                       # Shift+Tab 反缩进
    k.settle()
    save_close()
    k.settle()
    txt = read_file(k.img, "MOTD.TXT")
    ok_c2 = txt is not None and txt.startswith("A friend friend afar")
    print("[%s] Shift+Tab 反缩进" % ("OK" if ok_c2 else "FAIL"))

    # ---------------- D. F2 重命名 ----------------
    k.fkey(2)
    print("   rename dialog:", hex(k.run_until({0x40}) or 0))
    k.settle()
    k.shot(os.path.join(SHOTS, "f5-rename.png"))
    k.type_text("\b" * 12)                  # 清空 MOTD.TXT
    k.type_text("QUOTE.TXT")
    k.type_text("\r")
    print("   renamed:", hex(k.run_until({0x41}) or 0))
    k.settle()
    k.shot(os.path.join(SHOTS, "f6-renamed.png"))

    magic, fc, files = list_fs(k.img)
    names = [n for n, s in files]
    ok_d = "QUOTE.TXT" in names and "MOTD.TXT" not in names
    print("[%s] F2 重命名 -> %s" % ("OK" if ok_d else "FAIL", names))

    ok = ok_a and ok_b and ok_c1 and ok_c2 and ok_d
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
