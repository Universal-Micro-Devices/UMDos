#!/usr/bin/env python3
"""Generate kernel 2bpp bitmap from the official UMDos logo PNG.

Usage: python tools/gen_umdlogo.py <logo.png> <out.h> [size]
White background is transparent; colors quantized to blue / navy.
"""
import sys
from PIL import Image

SRC = sys.argv[1]
OUT = sys.argv[2]
SIZE = int(sys.argv[3]) if len(sys.argv) > 3 else 28

im = Image.open(SRC).convert("RGB")
px = im.load()
W, H = im.size

# Locate the icon (left part): scan columns, stop at the first >=10px gap.
cols = []
for x in range(int(W * 0.55)):
    n = 0
    for y in range(H):
        r, g, b = px[x, y]
        if not (r > 245 and g > 245 and b > 245):
            n += 1
    cols.append(n)

start = next(i for i, n in enumerate(cols) if n > 0)
end = start
gap = 0
for x in range(start, len(cols)):
    if cols[x] > 0:
        end = x
        gap = 0
    else:
        gap += 1
        if gap >= 10:
            break
minx, maxx = start, end
miny, maxy = H, 0
for y in range(H):
    for x in range(minx, maxx + 1):
        r, g, b = px[x, y]
        if not (r > 245 and g > 245 and b > 245):
            miny = min(miny, y)
            maxy = max(maxy, y)

box = (minx, miny, maxx + 1, maxy + 1)
icon = im.crop(box).resize((SIZE, SIZE), Image.NEAREST)

BLUE = (99, 170, 255)
NAVY = (25, 16, 41)


def classify(p):
    r, g, b = p
    if r > 240 and g > 240 and b > 240:
        return 0

    def d(c):
        return (r - c[0]) ** 2 + (g - c[1]) ** 2 + (b - c[2]) ** 2

    return 1 if d(BLUE) <= d(NAVY) else 2


stride = (SIZE + 3) // 4
data = []
for y in range(SIZE):
    for i in range(stride):
        b = 0
        for k in range(4):
            x = i * 4 + k
            v = classify(icon.getpixel((x, y))) if x < SIZE else 0
            b = (b << 2) | v
        data.append(b)

lines = []
lines.append("/* UMDos official logo (%dx%d, 2bpp): 0=transparent 1=blue 2=navy */" % (SIZE, SIZE))
lines.append("#ifndef UMDLOGO_H")
lines.append("#define UMDLOGO_H")
lines.append("")
lines.append("#define UMD_LOGO_W %d" % SIZE)
lines.append("#define UMD_LOGO_H %d" % SIZE)
lines.append("#define UMD_LOGO_STRIDE %d" % stride)
lines.append("")
lines.append("static const u8 umd_logo[] = {")
for i in range(0, len(data), 12):
    lines.append("    " + ", ".join("0x%02X" % v for v in data[i:i + 12]) + ",")
lines.append("};")
lines.append("")
lines.append("#endif")
open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
print("umdlogo.h: %dx%d, %d bytes, bbox=%s" % (SIZE, SIZE, len(data), box))
