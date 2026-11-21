#!/usr/bin/env python3
"""编辑器光标 / 行编辑测试：方向键、Home/End、插入、退格、Delete。"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emu
from test_persist import boot, login_and_open_files, list_fs

SHOTS = os.path.join(emu.ROOT, "shots")


def main():
    k = boot(open(emu.IMG, "rb").read())
    login_and_open_files(k)
    print("file manager open")

    # 光标下移两行选中 MOTD.TXT，回车打开
    k.key("down")
    k.key("down")
    k.settle()
    k.type_text("\r")
    print("  marker", hex(k.run_until({0x34}) or 0), "(editor)")

    # Home 到行首，Up 到上一行，End 到行尾
    k.key("home")
    k.key("up")
    k.key("end")
    k.type_text("!")
    k.key("home")
    k.type_text(">> ")
    k.key("up")
    k.type_text("inserted line")
    k.key("up")
    k.key("down")
    k.key("down")
    k.key("end")
    k.type_text("<<")
    k.settle()
    k.shot(os.path.join(SHOTS, "e1-cursor.png"))

    # Del：把光标移到行首再删一个字符（删掉 ">>" 的第一个 '>'）
    k.key("home")
    k.key("delete")
    k.settle()
    k.shot(os.path.join(SHOTS, "e2-after-del.png"))

    k.type_text("\x1b")                     # 保存
    print("  marker", hex(k.run_until({0x35}) or 0), "(saved)")
    k.shot(os.path.join(SHOTS, "e3-saved.png"))

    magic, fc, files = list_fs(k.img)
    print("on-disk:", magic, "files =", fc)
    for name, size in files:
        print("   %-12s %4d" % (name, size))

    # 读出 MOTD.TXT 的内容做断言
    lb = 512
    dblk = struct.unpack_from("<I", k.img, lb * 512 + 24)[0]
    content = b""
    for i in range(32):
        off = (lb + dblk) * 512 + i * 64
        name = bytes(k.img[off:off + 32]).split(b'\0')[0].decode("ascii", "replace")
        if name != "MOTD.TXT":
            continue
        size, first = struct.unpack_from("<II", k.img, off + 32)
        content = bytes(k.img[(lb + first) * 512:(lb + first) * 512 + size])
        break
    print("---- MOTD.TXT ----")
    print(content.decode("ascii", "replace"))
    print("------------------")

    text = content.decode("ascii", "replace")
    checks = [
        ("> Hai nei cun zhi ji" in text, "Home 定位行首 + Delete 删掉第一个 '>'"),
        ("inserted line\n" in text, "Up 到空行后插入整行"),
        ("bi lin.!<<" in text, "End 后追加 ! 和 <<"),
        (text.count("\n") >= 5, "行结构保持不变"),
    ]
    ok = all(c[0] for c in checks)
    for good, what in checks:
        print("  [%s] %s" % ("OK" if good else "FAIL", what))
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
