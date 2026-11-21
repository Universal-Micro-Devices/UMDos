#!/usr/bin/env python3
"""生成安装引导用的占位图标（16x16 1bpp）。

说明：规格要求图标统一取自 microsoft/fluentui-system-icons。当前构建环境无法联网
（下载超时），也没有可用的 SVG 光栅化器，因此 dev.1 按规格允许的回退方案使用
内置占位图标；来源与原因见 docs/third-party-assets.md。
"""
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "assets", "icons", "generated")


def blank():
    return Image.new("L", (16, 16), 0)


def icon_disk():
    img = blank(); d = ImageDraw.Draw(img)
    d.rectangle([1, 2, 14, 13], outline=255)
    d.rectangle([3, 4, 12, 7], outline=255)
    d.rectangle([9, 10, 11, 11], fill=255)
    return img


def icon_partition():
    img = blank(); d = ImageDraw.Draw(img)
    d.rectangle([1, 2, 14, 13], outline=255)
    d.line([5, 2, 5, 13], fill=255)
    d.line([10, 2, 10, 13], fill=255)
    return img


def icon_format():
    img = blank(); d = ImageDraw.Draw(img)
    d.rectangle([2, 2, 13, 13], outline=255)
    for i in range(4):
        d.line([4 + i, 4 + i, 11, 4 + i], fill=255)
    return img


def icon_path():
    img = blank(); d = ImageDraw.Draw(img)
    d.rectangle([1, 4, 6, 6], fill=255)
    d.rectangle([1, 6, 14, 13], outline=255)
    d.line([4, 9, 11, 9], fill=255)
    return img


def icon_user():
    img = blank(); d = ImageDraw.Draw(img)
    d.ellipse([5, 2, 10, 7], outline=255)
    d.arc([2, 8, 13, 16], 180, 360, fill=255)
    return img


def icon_lock():
    img = blank(); d = ImageDraw.Draw(img)
    d.arc([5, 1, 10, 7], 180, 360, fill=255)
    d.rectangle([3, 7, 12, 14], outline=255)
    d.rectangle([7, 10, 8, 12], fill=255)
    return img


def icon_warn():
    img = blank(); d = ImageDraw.Draw(img)
    d.polygon([(8, 1), (15, 14), (1, 14)], outline=255)
    d.line([8, 5, 8, 10], fill=255)
    d.rectangle([7, 11, 8, 12], fill=255)
    return img


def icon_check():
    img = blank(); d = ImageDraw.Draw(img)
    d.line([2, 8, 6, 12], fill=255)
    d.line([6, 12, 13, 3], fill=255)
    return img


def icon_cross():
    img = blank(); d = ImageDraw.Draw(img)
    d.line([3, 3, 12, 12], fill=255)
    d.line([12, 3, 3, 12], fill=255)
    return img


ICONS = [
    ("ICON_DISK", icon_disk),
    ("ICON_PARTITION", icon_partition),
    ("ICON_FORMAT", icon_format),
    ("ICON_PATH", icon_path),
    ("ICON_USER", icon_user),
    ("ICON_LOCK", icon_lock),
    ("ICON_WARN", icon_warn),
    ("ICON_CHECK", icon_check),
    ("ICON_CROSS", icon_cross),
]


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    out = ["/* 自动生成：安装引导占位图标 16x16 1bpp",
           " * 来源：本仓库脚本绘制（非 fluentui）；原因见 docs/third-party-assets.md */",
           "#ifndef PLACEHOLDER_ICONS_H", "#define PLACEHOLDER_ICONS_H", "",
           "#include <stdint.h>", ""]
    for name, fn in ICONS:
        img = fn()
        rows = []
        for y in range(16):
            bits = 0
            for x in range(16):
                if img.getpixel((x, y)) > 96:
                    bits |= 1 << (15 - x)
            rows.append(bits)
        out.append("static const uint16_t %s[16] = { %s };" %
                   (name, ", ".join("0x%04X" % r for r in rows)))
    out += ["", "#endif", ""]
    path = os.path.join(OUT_DIR, "placeholder_icons.h")
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(out))
    print("placeholder icons:", len(ICONS), "->", path)


if __name__ == "__main__":
    main()
