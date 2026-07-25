#!/usr/bin/env python3
"""Prep a generated state illustration for the panel: resize to 360x360, apply
a circular vignette (clean dark rim under the progress arc), quantize to <=248
colors so build_face_bitmap.mjs can emit a palette header.
Usage: prep_state_art.py in.png out.png"""
import math
import sys

from PIL import Image

W = H = 360
CX = CY = 180.0
# rim band the ember arc draws in (see ui_buddy.h) — fade art to near-black there
R_ART = 168.0

src, dst = sys.argv[1], sys.argv[2]
im = Image.open(src).convert("RGB").resize((W, H), Image.LANCZOS)
px = im.load()
for y in range(H):
    for x in range(W):
        r = math.hypot(x - CX + 0.5, y - CY + 0.5)
        if r <= R_ART - 14:
            continue
        # smooth fade from full art to a deep navy rim, fully dark past R_ART
        t = min(1.0, max(0.0, (r - (R_ART - 14)) / 14.0))
        k = 1.0 - t * t * (3 - 2 * t)
        pr, pg, pb = px[x, y]
        px[x, y] = (int(pr * k + 6 * (1 - k)), int(pg * k + 7 * (1 - k)), int(pb * k + 14 * (1 - k)))

im = im.quantize(colors=248, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG).convert("RGB")
im.save(dst)
print(f"{dst}: quantized, {len(set(im.getdata()))} unique colors")
