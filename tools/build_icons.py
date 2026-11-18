#!/usr/bin/env python3
"""Draw Knurl's line icons at 4x, scale them down, and store them as a 4-bit "font" so the
firmware draws (and tints) them exactly like text. Writes build/gen/icons.c and icons.h.
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

SS = 4  # supersampling


def canvas(w, h):
    img = Image.new("L", (w * SS, h * SS), 0)
    return img, ImageDraw.Draw(img)


def P(*v):  # icon units (pixels) -> supersampled pixels
    return [round(c * SS) for c in v]


def stroke(px):
    return max(1, round(px * SS))


def flow(d, s):      # a cover face-on with two tilted ones at the sides
    d.rounded_rectangle(P(4, 2, 10, 12), radius=SS, outline=255, width=stroke(1.4))
    d.line(P(1.2, 4, 1.2, 10), fill=255, width=stroke(1.4))
    d.line(P(12.8, 4, 12.8, 10), fill=255, width=stroke(1.4))


def albums(d, s):
    d.rounded_rectangle(P(1.5, 1.5, 12.5, 12.5), radius=2 * SS, outline=255, width=stroke(1.4))
    d.ellipse(P(4.5, 4.5, 9.5, 9.5), outline=255, width=stroke(1.3))
    d.ellipse(P(6.4, 6.4, 7.6, 7.6), fill=255)


def artists(d, s):
    d.ellipse(P(4.5, 1.5, 9.5, 6.5), outline=255, width=stroke(1.4))
    d.arc(P(1.5, 8, 12.5, 19), 200, 340, fill=255, width=stroke(1.4))


def songs(d, s):
    d.line(P(5.5, 10.5, 5.5, 2.5), fill=255, width=stroke(1.4))
    d.line(P(11.5, 9, 11.5, 1.5), fill=255, width=stroke(1.4))
    d.line(P(5.5, 2.8, 11.5, 1.5), fill=255, width=stroke(1.6))
    d.ellipse(P(2, 9, 6, 12.5), fill=255)
    d.ellipse(P(8, 7.5, 12, 11), fill=255)


def playlists(d, s):
    for y in (3, 6.5):
        d.line(P(1.5, y, 9.5, y), fill=255, width=stroke(1.4))
    d.line(P(1.5, 10, 6, 10), fill=255, width=stroke(1.4))
    d.line(P(11.5, 11, 11.5, 5), fill=255, width=stroke(1.4))
    d.line(P(11.5, 5, 13, 5.8), fill=255, width=stroke(1.4))
    d.ellipse(P(8, 9.5, 11.8, 12.8), fill=255)


def shuffle(d, s):
    d.line(P(1.5, 3.5, 4.5, 3.5, 9.5, 10.5, 12, 10.5), fill=255, width=stroke(1.4), joint="curve")
    d.line(P(1.5, 10.5, 4.5, 10.5, 9.5, 3.5, 12, 3.5), fill=255, width=stroke(1.4), joint="curve")
    for y in (3.5, 10.5):
        d.polygon(P(13, y, 10.5, y - 2.2, 10.5, y + 2.2), fill=255)


def settings(d, s):
    for y, kx in ((4, 9), (10, 5)):
        d.line(P(1.5, y, 12.5, y), fill=255, width=stroke(1.4))
        d.ellipse(P(kx - 2, y - 2, kx + 2, y + 2), fill=0, outline=255, width=stroke(1.4))


def chevron(d, s):
    d.line(P(2, 1.5, 6, 6, 2, 10.5), fill=255, width=stroke(1.6), joint="curve")


def heart(d, s):
    d.ellipse(P(2, 3, 9.5, 10.5), fill=255)
    d.ellipse(P(8.5, 3, 16, 10.5), fill=255)
    d.polygon(P(2.4, 8.4, 9, 15.6, 15.6, 8.4, 9, 6), fill=255)


def play(d, s):
    d.polygon(P(1.5, 0.8, 1.5, 11.2, 10.5, 6), fill=255)


def pause(d, s):
    d.rounded_rectangle(P(1, 0.8, 4, 11.2), radius=SS // 2, fill=255)
    d.rounded_rectangle(P(7, 0.8, 10, 11.2), radius=SS // 2, fill=255)


def clock(d, s):
    d.ellipse(P(1.5, 1.5, 16.5, 16.5), outline=255, width=stroke(1.6))
    d.line(P(9, 4.5, 9, 9, 12, 11), fill=255, width=stroke(1.6), joint="curve")


# Now Playing (ui/NowPlayingScreen.kt): Knurl draws these on a 16-unit grid; here 12 px.
K = 12 / 16


def KP(*v):
    return P(*[c * K for c in v])


def info_song(d, s):
    d.line(KP(6, 13, 6, 3.2, 14, 1.6, 14, 11.5), fill=255, width=stroke(1.6 * K), joint="curve")
    d.ellipse(KP(2, 11, 6, 15), fill=255)
    d.ellipse(KP(10, 9.5, 14, 13.5), fill=255)


def info_artist(d, s):
    d.ellipse(KP(5.1, 2.4, 10.9, 8.2), outline=255, width=stroke(1.6 * K))
    d.arc(KP(2.6, 10.1, 13.4, 19.1), 180, 360, fill=255, width=stroke(1.6 * K))


def info_album(d, s):
    d.ellipse(KP(1.6, 1.6, 14.4, 14.4), outline=255, width=stroke(1.6 * K))
    d.ellipse(KP(6.3, 6.3, 9.7, 9.7), fill=255)


def speaker(d, s, loud=False):
    x0, x1, x2 = (1, 3.5, 7) if loud else (2, 4.5, 8)
    d.polygon(KP(x0, 6, x1, 6, x2, 3, x2, 13, x1, 10, x0, 10), fill=255)
    if loud:
        d.arc(KP(6, 5, 13, 11), -50, 50, fill=255, width=stroke(1.5 * K))
        d.arc(KP(5, 1.7, 17, 14.3), -55, 55, fill=255, width=stroke(1.5 * K))


# name, width, height, painter
ICONS = [
    ("FLOW", 14, 14, flow), ("ALBUMS", 14, 14, albums), ("ARTISTS", 14, 14, artists),
    ("SONGS", 14, 14, songs), ("PLAYLISTS", 14, 14, playlists), ("SHUFFLE", 14, 14, shuffle),
    ("SETTINGS", 14, 14, settings), ("CHEVRON", 8, 12, chevron), ("HEART", 18, 18, heart),
    ("PLAY", 12, 12, play), ("PAUSE", 11, 12, pause), ("CLOCK", 18, 18, clock),
    ("INFO_SONG", 12, 12, info_song), ("INFO_ARTIST", 12, 12, info_artist), ("INFO_ALBUM", 12, 12, info_album),
    ("SPEAKER", 12, 12, lambda d, s: speaker(d, s)), ("SPEAKER_LOUD", 12, 12, lambda d, s: speaker(d, s, True)),
]
BASE = 0xE000


def main(out_dir):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    data, glyphs = bytearray(), []
    for n, (name, w, h, paint) in enumerate(ICONS):
        img, d = canvas(w, h)
        paint(d, SS)
        img = img.resize((w, h), Image.BOX)
        off = len(data)
        for y in range(h):
            row = [(img.getpixel((x, y)) * 15 + 127) // 255 for x in range(w)]
            for i in range(0, w, 2):
                data.append((row[i] << 4) | (row[i + 1] if i + 1 < w else 0))
        glyphs.append((BASE + n, 0, 0, w, h, w, off))
    c = ["/* Generated by tools/build_icons.py - do not edit. */", '#include "icons.h"',
         f"static const uint8_t icon_bits[{len(data)}] = {{"]
    for i in range(0, len(data), 24):
        c.append("    " + ", ".join(map(str, data[i:i + 24])) + ",")
    c.append("};")
    c.append(f"static const glyph_t icon_glyphs[{len(glyphs)}] = {{")
    c += ["    {%d, %d, %d, %d, %d, %d, %d}," % g for g in glyphs]
    c.append("};")
    c.append(f"const font_t font_icons = {{ icon_glyphs, {len(glyphs)}, icon_bits, 0, 0 }};")
    h = ["/* Generated by tools/build_icons.py - do not edit. */", "#pragma once", '#include "gfx/font.h"',
         "extern const font_t font_icons;"]
    for n, (name, w, hh, _) in enumerate(ICONS):
        h.append(f"#define ICON_{name} 0x{BASE + n:04X}  /* {w}x{hh} */")
    (out / "icons.c").write_text("\n".join(c) + "\n")
    (out / "icons.h").write_text("\n".join(h) + "\n")
    print(f"icons: {len(ICONS)} icons, {len(data)} bytes")


if __name__ == "__main__":
    main(sys.argv[1])
