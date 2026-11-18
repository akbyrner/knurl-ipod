#!/usr/bin/env python3
"""Render the Milestone 0 splash (320x240) in Knurl's Graphite Glass look and write it as
raw little-endian RGB565 for the firmware, plus a 2x PNG preview.

Look and numbers come from Knurl (github.com/akbyrner/knurl, ui/Chrome.kt, Plane.kt, Fonts.kt):
graphite #18181B, bone #ECEBE6, one orange accent #FF6A1A, glass = white 7% with a 12% border,
Archivo Expanded ExtraBold titles, IBM Plex Mono Medium caps labels, Inter for body text."""
import random
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 320, 240
FONTS = Path(__file__).resolve().parent.parent / "assets" / "fonts"

GRAPHITE = (0x18, 0x18, 0x1B)
BONE = (0xEC, 0xEB, 0xE6)
LABEL_GRAY = (0x8E, 0x90, 0x97)
INFO_TEXT = (0xD0, 0xD0, 0xD4)
TIME_GRAY = (0xB0, 0xB0, 0xB5)
GLYPH = (0xC4, 0xC4, 0xC4)
WASH = (0x8E, 0x9C, 0x3A)  # an olive cover colour, like the screenshots


def font(name, size, **axes):
    f = ImageFont.truetype(str(FONTS / name), size)
    if axes:
        try:
            names = [a["name"].decode() if isinstance(a["name"], bytes) else a["name"] for a in f.get_variation_axes()]
            values = []
            for a, n in zip(f.get_variation_axes(), names):
                key = {"Weight": "wght", "Width": "wdth", "Optical size": "opsz"}.get(n, n)
                values.append(axes.get(key, a["default"]))
            f.set_variation_by_axes(values)
        except OSError:
            pass  # not a variable font
    return f


def spaced(draw, xy, text, fnt, fill, tracking, anchor_center=False):
    """Draw text with letter spacing (tracking in px)."""
    widths = [draw.textlength(c, font=fnt) for c in text]
    total = sum(widths) + tracking * (len(text) - 1)
    x, y = xy
    if anchor_center:
        x -= total / 2
    for c, w in zip(text, widths):
        draw.text((x, y), c, font=fnt, fill=fill)
        x += w + tracking
    return total


def render():
    img = Image.new("RGB", (W, H), GRAPHITE)
    px = img.load()
    rnd = random.Random(7)
    for y in range(H):
        # cover wash fading into graphite (Plane.kt: transparent at top, solid graphite by ~62%)
        t = y / H
        wash = max(0.0, 0.62 - t) / 0.62
        wash = wash ** 1.3 * 0.55
        for x in range(W):
            # soft light from the top edge
            dx, dy = (x - W / 2) / (W * 0.9), y / (W * 0.9)
            light = max(0.0, 1 - (dx * dx + dy * dy) ** 0.5) * 0.07
            r = GRAPHITE[0] * (1 - wash) + WASH[0] * wash
            g = GRAPHITE[1] * (1 - wash) + WASH[1] * wash
            b = GRAPHITE[2] * (1 - wash) + WASH[2] * wash
            grain = rnd.randint(0, 18) / 255
            r, g, b = (c + (255 - c) * (light + grain) for c in (r, g, b))
            px[x, y] = (int(r), int(g), int(b))

    d = ImageDraw.Draw(img, "RGBA")
    mono9 = font("plex_mono_medium.ttf", 9)
    mono8 = font("plex_mono_medium.ttf", 8)

    # header: centred mono label, battery at the right (Chrome.kt ScreenHeader(big = false))
    spaced(d, (W / 2, 7), "HELLO, IPOD", mono9, (0xB8, 0xB9, 0xBE), 1.6, anchor_center=True)
    d.rounded_rectangle((292, 8, 306, 15), radius=2, outline=GLYPH, width=1)
    d.rectangle((294, 10, 302, 13), fill=GLYPH)
    d.rectangle((307, 10, 308, 13), fill=GLYPH)

    # wordmark
    wordmark = font("archivo.ttf", 40, wght=800, wdth=125)
    spaced(d, (W / 2, 52), "KNURL", wordmark, BONE, 2.0, anchor_center=True)
    spaced(d, (W / 2, 108), "HAND-ROLLED FIRMWARE  0.0.1", mono9, LABEL_GRAY, 1.4, anchor_center=True)

    # glass card (NowPlayingScreen.kt: white 7% fill, 12% border, rounded)
    d.rounded_rectangle((12, 146, 308, 226), radius=12, fill=(255, 255, 255, 18), outline=(255, 255, 255, 31), width=1)
    inter_b = font("inter.ttf", 14, wght=700)
    inter = font("inter.ttf", 11, wght=500)
    d.text((26, 154), "Milestone 0", font=inter_b, fill=(255, 255, 255))
    d.text((26, 173), "Boot  ·  screen  ·  timer  ·  hold switch", font=inter, fill=INFO_TEXT)
    # the firmware draws the notched bar at y=196..200, x=24..296
    spaced(d, (24, 208), "00:00", mono8, TIME_GRAY, 0.8)
    label = "FLIP HOLD TO EXIT"
    w = sum(d.textlength(c, font=mono8) for c in label) + 0.8 * (len(label) - 1)
    spaced(d, (296 - w, 208), label, mono8, TIME_GRAY, 0.8)
    return img


BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def to_rgb565(img):
    """Quantise to RGB565 with ordered dithering (so gradients don't band)."""
    out = bytearray()
    quant = Image.new("RGB", img.size)
    q = quant.load()
    p = img.load()
    for y in range(H):
        for x in range(W):
            r, g, b = p[x, y]
            t = (BAYER4[y & 3][x & 3] + 0.5) / 16 - 0.5
            r5 = min(31, max(0, int(r / 255 * 31 + 0.5 + t)))
            g6 = min(63, max(0, int(g / 255 * 63 + 0.5 + t)))
            b5 = min(31, max(0, int(b / 255 * 31 + 0.5 + t)))
            v = (r5 << 11) | (g6 << 5) | b5
            out += v.to_bytes(2, "little")
            q[x, y] = (r5 * 255 // 31, g6 * 255 // 63, b5 * 255 // 31)
    return bytes(out), quant


if __name__ == "__main__":
    raw_path, preview_path = sys.argv[1], sys.argv[2]
    raw, quant = to_rgb565(render())
    assert len(raw) == W * H * 2
    Path(raw_path).write_bytes(raw)
    quant.resize((W * 2, H * 2), Image.NEAREST).save(preview_path)
    print(f"{raw_path}: {len(raw):,} bytes; preview {preview_path}")
