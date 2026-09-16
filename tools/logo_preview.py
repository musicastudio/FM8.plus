#!/usr/bin/env python3
"""Preview FM8.plus's "FM8+" wordmark against the real FM8 artwork, without building anything.

The shipped code builds the wordmark at runtime from the user's own FM8 (src/core/rsrc.cpp: widen
the control rect in FRM 5 and 15, widen PICTURE 193 and draw the "+" into the new space), so nothing
here is compiled in and no Native Instruments data is ever checked in. This renders the same edit
with tools/fm8gui.py so a layout change can be judged before it is written in C++.

    python tools/logo_preview.py [FM8.exe] [outdir]

Writes <outdir>/logo.png (the wordmark alone, 8x), <outdir>/5.png (the header bar) and
<outdir>/3.png (the whole editor). Defaults to build/gui/logo-preview.
"""
from __future__ import annotations

import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fm8gui as g  # noqa: E402

INSTALLED = r"C:\Program Files\Native Instruments\FM8\FM8.exe"

# The geometry, mirroring src/core/rsrc.cpp. The stock control and picture are 95x23 at (21,35): the
# control moves LOGO_SHIFT px left and grows PLUS_W px right, and the picture widens to match.
LOGO_PIC = 193
LOGO_SHIFT = 11
PLUS_W = 22
PLUS_COLOUR = (107, 125, 134, 255)
PLUS_CX, PLUS_CY = 107.4, 12.46
PLUS_HALF, PLUS_THICK, PLUS_SLANT = 7.37, 1.515, 0.1767
STOCK_RECT = 'rect="21,35,116,58"'
WIDE_RECT = f'rect="{21 - LOGO_SHIFT},35,{116 + PLUS_W - LOGO_SHIFT},58"'


def plus_picture(src_png):
    """The stock wordmark widened by PLUS_W with the "+" drawn on, as PNG bytes."""
    from PIL import Image, ImageDraw
    src = Image.open(src_png).convert("RGBA")
    out = Image.new("RGBA", (src.width + PLUS_W, src.height), (0, 0, 0, 0))
    out.paste(src, (0, 0))
    a, t, k = PLUS_HALF, PLUS_THICK, 8      # k = supersampling factor for the antialiased edges
    xs = (a, t, t, -t, -t, -a, -a, -t, -t, t, t, a)
    ys = (-t, -t, -a, -a, -t, -t, t, t, a, a, t, t)
    pts = [((PLUS_CX + x - y * PLUS_SLANT) * k, (PLUS_CY + y) * k) for x, y in zip(xs, ys)]
    big = Image.new("RGBA", (out.width * k, out.height * k), (0, 0, 0, 0))
    ImageDraw.Draw(big).polygon(pts, fill=PLUS_COLOUR)
    out.alpha_composite(big.resize(out.size, Image.LANCZOS))
    return out


def main():
    args = [a for a in sys.argv[1:]]
    pe = args[0] if args else (INSTALLED if os.path.exists(INSTALLED) else g.DEFAULT_PE)
    out = args[1] if len(args) > 1 else os.path.join(g.GUI_DIR, "logo-preview")
    os.makedirs(out, exist_ok=True)

    rsrc = g.extract(pe, os.path.join(g.GUI_DIR, "rsrc"))
    # A scratch resource tree with our widened wordmark, and scratch XML with the widened rect, so
    # the previewer composites the real editor exactly as FM8 will draw it.
    scratch = os.path.join(out, "rsrc")
    if os.path.exists(scratch):
        shutil.rmtree(scratch)
    shutil.copytree(rsrc, scratch)
    logo = plus_picture(os.path.join(rsrc, "PICTURE", f"{LOGO_PIC}.png"))
    logo.save(os.path.join(scratch, "PICTURE", f"{LOGO_PIC}.png"))
    logo.resize((logo.width * 8, logo.height * 8), 0).save(os.path.join(out, "logo.png"))

    xml = os.path.join(out, "xml")
    g.dump(rsrc, xml)
    for fid in (5, 15):
        path = os.path.join(xml, f"{fid}.xml")
        text = open(path, encoding="utf-8").read()
        if STOCK_RECT not in text:
            raise SystemExit(f"FRM {fid}: wordmark rect {STOCK_RECT} not found; the geometry moved")
        open(path, "w", encoding="utf-8").write(text.replace(STOCK_RECT, WIDE_RECT))

    g.preview(scratch, out, xmldir=xml)
    print(f"wordmark -> {os.path.join(out, 'logo.png')} (8x)")
    print(f"header    -> {os.path.join(out, '5.png')}")
    print(f"editor    -> {os.path.join(out, '3.png')}")


if __name__ == "__main__":
    main()
