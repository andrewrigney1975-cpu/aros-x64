#!/usr/bin/env python3
"""
Generates Workbench-2.04-style icon PNGs for workbench.bas: a floppy
disk icon (desktop), a folder/drawer icon and a generic file icon
(both used in the file-browser window's grid). Saved flat/opaque with
the SAME grey the desktop/window background already uses (RECT
11184810 = 0xAAAAAA), since basix_rt_drawpng blits fully opaque with
no alpha compositing (see kernel/png.inc) -- baking the matching
background in is how these blend into the desktop without any
"transparent" pixel support existing at all.

Run this after editing the icon logic below; outputs go to assets/,
copy the ones you want live onto the exFAT test VHD as *.png next to
workbench.bas (see testdata/README.md for how the dev VHD is staged).
"""
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets")
os.makedirs(OUT, exist_ok=True)

BG = (170, 170, 170)       # 0xAAAAAA -- desktop/window background
BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
TITLE = (137, 138, 205)    # ~0x898ACD -- status bar / title bar blue
SHADOW = (110, 110, 110)   # mid grey for 3D bevel shading
HILITE = (221, 221, 221)   # light grey for 3D bevel highlight


def new_icon(w, h):
    im = Image.new("RGB", (w, h), BG)
    return im, ImageDraw.Draw(im)


def bevel_rect(d, x0, y0, x1, y1, fill):
    """A simple beveled rectangle: fill + light top/left edge, dark
    bottom/right edge, black 1px outline -- the classic chunky-3D
    look every OCS/ECS Workbench icon uses instead of anti-aliasing."""
    d.rectangle([x0, y0, x1, y1], fill=fill, outline=BLACK)
    d.line([x0 + 1, y0 + 1, x1 - 1, y0 + 1], fill=HILITE)
    d.line([x0 + 1, y0 + 1, x0 + 1, y1 - 1], fill=HILITE)
    d.line([x0 + 1, y1 - 1, x1 - 1, y1 - 1], fill=SHADOW)
    d.line([x1 - 1, y0 + 1, x1 - 1, y1 - 1], fill=SHADOW)


def gen_disk(w=48, h=32):
    im, d = new_icon(w, h)
    bx0, by0, bx1, by1 = 4, 3, w - 5, h - 3
    bevel_rect(d, bx0, by0, bx1, by1, WHITE)
    # metal shutter band across the top third
    d.rectangle([bx0 + 2, by0 + 2, bx1 - 2, by0 + 8], fill=SHADOW, outline=BLACK)
    d.rectangle([bx0 + 5, by0 + 3, bx0 + 9, by0 + 7], fill=BLACK)  # write-protect notch
    # label rectangle lower half
    d.rectangle([bx0 + 4, by0 + 12, bx1 - 4, by1 - 4], fill=TITLE, outline=BLACK)
    for ly in range(by0 + 15, by1 - 5, 3):
        d.line([bx0 + 6, ly, bx1 - 6, ly], fill=WHITE)
    im.save(os.path.join(OUT, "icon_disk.png"))


def gen_folder(w=40, h=28):
    im, d = new_icon(w, h)
    # tab (top-left, narrower)
    bevel_rect(d, 3, 2, 18, 8, TITLE)
    # body
    bevel_rect(d, 3, 7, w - 4, h - 3, TITLE)
    im.save(os.path.join(OUT, "icon_folder.png"))


def file_outline(d, w, h):
    """Shared "folded-corner document" silhouette every per-type file
    icon below starts from -- bevel-edged (light top/left, dark
    bottom/right on the corner fold) the same convention as bevel_rect,
    not a flat black outline, so these read consistently with every
    other beveled surface in the UI (window chrome, gadget boxes).
    Returns the body's own (x0, y0, x1, y1) so callers can draw their
    distinguishing mark inside it.
    """
    x0, y0, x1, y1 = 6, 2, w - 7, h - 3
    fold = 7
    d.polygon(
        [(x0, y0), (x1 - fold, y0), (x1, y0 + fold), (x1, y1), (x0, y1)],
        fill=WHITE, outline=BLACK,
    )
    # Bevel edges on the body -- light top/left, dark bottom/right,
    # matching bevel_rect's own convention (kept as separate lines
    # here rather than reusing bevel_rect itself since this shape
    # isn't a plain rectangle -- the top-right corner is folded).
    d.line([x0 + 1, y0 + 1, x1 - fold, y0 + 1], fill=HILITE)
    d.line([x0 + 1, y0 + 1, x0 + 1, y1 - 1], fill=HILITE)
    d.line([x0 + 1, y1 - 1, x1 - 1, y1 - 1], fill=SHADOW)
    d.line([x1 - 1, y0 + fold, x1 - 1, y1 - 1], fill=SHADOW)
    # The fold itself -- small triangle, its own light/dark pairing.
    d.polygon([(x1 - fold, y0), (x1, y0 + fold), (x1 - fold, y0 + fold)],
              fill=SHADOW, outline=BLACK)
    d.line([x1 - fold, y0 + 1, x1 - fold, y0 + fold], fill=HILITE)
    return x0, y0, x1, y1


def gen_file(w=40, h=28):
    """Fallback icon for any extension with no dedicated icon below --
    the plain document silhouette, no distinguishing mark."""
    im, d = new_icon(w, h)
    file_outline(d, w, h)
    im.save(os.path.join(OUT, "icon_file.png"))


def gen_file_txt(w=40, h=28):
    """.TXT -- horizontal ruled lines, like a page of text."""
    im, d = new_icon(w, h)
    x0, y0, x1, y1 = file_outline(d, w, h)
    for ly in range(y0 + 8, y1 - 2, 4):
        d.line([x0 + 3, ly, x1 - 3, ly], fill=SHADOW)
    im.save(os.path.join(OUT, "icon_txt.png"))


def gen_file_bas(w=40, h=28):
    """.BAS -- a solid title-blue "code" bar plus a couple of short
    ragged lines beneath it (suggesting indented source lines),
    distinct at a glance from the plain ruled-line .TXT icon."""
    im, d = new_icon(w, h)
    x0, y0, x1, y1 = file_outline(d, w, h)
    d.rectangle([x0 + 3, y0 + 4, x1 - 4, y0 + 8], fill=TITLE, outline=BLACK)
    d.line([x0 + 3, y0 + 13, x0 + 14, y0 + 13], fill=SHADOW)
    d.line([x0 + 6, y0 + 17, x0 + 20, y0 + 17], fill=SHADOW)
    d.line([x0 + 3, y0 + 21, x0 + 11, y0 + 21], fill=SHADOW)
    im.save(os.path.join(OUT, "icon_bas.png"))


def gen_file_png(w=40, h=28):
    """.PNG -- classic tiny mountain + sun "image file" glyph."""
    im, d = new_icon(w, h)
    x0, y0, x1, y1 = file_outline(d, w, h)
    d.rectangle([x0 + 3, y0 + 5, x1 - 4, y1 - 5], fill=WHITE, outline=BLACK)
    d.ellipse([x1 - 12, y0 + 7, x1 - 8, y0 + 11], fill=TITLE, outline=BLACK)
    d.polygon(
        [(x0 + 4, y1 - 6), (x0 + 11, y0 + 15), (x0 + 16, y1 - 10),
         (x0 + 20, y0 + 18), (x1 - 5, y1 - 6)],
        fill=SHADOW, outline=BLACK,
    )
    im.save(os.path.join(OUT, "icon_png.png"))


def gen_file_axb(w=40, h=28):
    """.AXB (compiled executable) -- a filled right-pointing "run"
    triangle, distinct from the passive document-style icons above."""
    im, d = new_icon(w, h)
    x0, y0, x1, y1 = file_outline(d, w, h)
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2 + 1
    d.polygon(
        [(cx - 6, cy - 8), (cx - 6, cy + 8), (cx + 8, cy)],
        fill=TITLE, outline=BLACK,
    )
    im.save(os.path.join(OUT, "icon_axb.png"))


def gen_close_gadget(w=12, h=12):
    im, d = new_icon(w, h)
    bevel_rect(d, 0, 0, w - 1, h - 1, WHITE)
    d.line([3, 3, w - 4, h - 4], fill=BLACK, width=2)
    d.line([w - 4, 3, 3, h - 4], fill=BLACK, width=2)
    im.save(os.path.join(OUT, "icon_close.png"))


gen_disk()
gen_folder()
gen_file()
gen_file_txt()
gen_file_bas()
gen_file_png()
gen_file_axb()
gen_close_gadget()
print("Wrote icon_disk/folder/file/txt/bas/png/axb/close.png to", OUT)
