#!/usr/bin/env python3
"""Turn the simulator's PPM frames into 2x PNGs plus one contact sheet (sheet.png)."""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

d = Path(sys.argv[1])
frames = sorted(d.glob("*.ppm"))
imgs = []
for f in frames:
    im = Image.open(f).convert("RGB")
    im.resize((640, 480), Image.NEAREST).save(f.with_suffix(".png"))
    imgs.append((f.stem, im))
    f.unlink()
cols = 3
rows = (len(imgs) + cols - 1) // cols
sheet = Image.new("RGB", (cols * 330 + 10, rows * 270 + 10), (40, 40, 44))
dr = ImageDraw.Draw(sheet)
for i, (name, im) in enumerate(imgs):
    x, y = 10 + (i % cols) * 330, 10 + (i // cols) * 270
    sheet.paste(im, (x, y))
    dr.text((x, y + 244), name, fill=(220, 220, 220))
sheet.save(d / "sheet.png")
print(f"shots: {len(imgs)} frames -> {d}/sheet.png")
