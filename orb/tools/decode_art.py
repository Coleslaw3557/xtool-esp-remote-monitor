#!/usr/bin/env python3
"""Decode a committed art_*.h header back into a PNG. Inverse of
build_face_bitmap.mjs — recovers the 360x360 prepped image, e.g. to rebuild
the style anchor for gen_state_art.py when /tmp/orb_state_art is gone:
    decode_art.py ../art_idle.h /tmp/orb_state_art/idle.png"""
import re
import sys

from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()

def carray(name):
    m = re.search(name + r"\[[^\]]*\]\s*=\s*\{(.*?)\};", text, re.S)
    return [int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", m.group(1))]

pal = carray("PALETTE")
px = carray("PIXELS")
im = Image.new("RGB", (360, 360))
im.putdata([(((c := pal[i]) >> 11 & 0x1F) << 3, (c >> 5 & 0x3F) << 2, (c & 0x1F) << 3) for i in px])
im.save(dst)
print(f"{dst}: {len(pal)} palette colors")
