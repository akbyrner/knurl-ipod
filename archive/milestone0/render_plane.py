#!/usr/bin/env python3
"""Render Knurl's Graphite Glass plane (graphite + cover wash + grain + top light) at 320x240
as dithered RGB565. Later the server renders one per album from its cover; for now this is
the default backdrop with an olive wash, like Knurl's screenshots.

    render_plane.py out.bin [preview.png] [wash hex, default 8E9C3A]
"""
import random
import sys
from pathlib import Path

from PIL import Image

W, H = 320, 240
GRAPHITE = (0x18, 0x18, 0x1B)
BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def plane(wash_rgb, tall=False):
    img = Image.new("RGB", (W, H))
    px = img.load()
    rnd = random.Random(7)
    fade = 0.68 if tall else 0.62
    strength = 0.62 if tall else 0.55
    for y in range(H):
        t = y / H
        wash = (max(0.0, fade - t) / fade) ** 1.3 * strength
        for x in range(W):
            dx, dy = (x - W / 2) / (W * 0.9), y / (W * 0.9)
            light = max(0.0, 1 - (dx * dx + dy * dy) ** 0.5) * 0.07
            base = [GRAPHITE[i] * (1 - wash) + wash_rgb[i] * wash for i in range(3)]
            grain = rnd.randint(0, 18) / 255
            px[x, y] = tuple(int(c + (255 - c) * (light + grain)) for c in base)
    return img


def to_rgb565(img):
    out = bytearray()
    quant = Image.new("RGB", img.size)
    q, p = quant.load(), img.load()
    for y in range(img.size[1]):
        for x in range(img.size[0]):
            r, g, b = p[x, y]
            t = (BAYER4[y & 3][x & 3] + 0.5) / 16 - 0.5
            r5 = min(31, max(0, int(r / 255 * 31 + 0.5 + t)))
            g6 = min(63, max(0, int(g / 255 * 63 + 0.5 + t)))
            b5 = min(31, max(0, int(b / 255 * 31 + 0.5 + t)))
            out += ((r5 << 11) | (g6 << 5) | b5).to_bytes(2, "little")
            q[x, y] = (r5 * 255 // 31, g6 * 255 // 63, b5 * 255 // 31)
    return bytes(out), quant


if __name__ == "__main__":
    wash = sys.argv[3] if len(sys.argv) > 3 else "8E9C3A"
    raw, quant = to_rgb565(plane(tuple(int(wash[i:i + 2], 16) for i in (0, 2, 4))))
    Path(sys.argv[1]).write_bytes(raw)
    if len(sys.argv) > 2:
        quant.save(sys.argv[2])
    print(f"{sys.argv[1]}: {len(raw):,} bytes")
