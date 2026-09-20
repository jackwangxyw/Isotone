#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
#
# Cuts every icon Isotone ships from the one mark (docs/design/logo, locked
# 2026-09-19).
#
#   python tools/gen_icons.py            write the assets
#   python tools/gen_icons.py --check    fail if any of them is out of date
#
# Needs Pillow and nothing else: the mark is five rounded rectangles, so it is
# drawn directly rather than through an SVG renderer, and the SVG is emitted as
# text. That keeps the tool runnable anywhere Python is, which a cairosvg
# dependency would not.
#
# The runtime does not read these: ui/src/logomark.cpp draws the same geometry
# for the tray and the window, and ui/tests pins it to the numbers below. These
# files are for the things that cannot call into the app: Explorer, the
# installer, and the Linux icon theme.

import argparse
import io
import os
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The mark, in a 24 x 24 box (docs/design/logo/out/mark-locked.png).
BOX = 24.0
PROFILE = [-0.45, 1.00, -0.70, 0.70, -0.45]   # gain per bar, -1 to 1
WIDTH = 3.0                                    # bar width
PAD = 4.6                                      # box edge to the first bar's centre
RADIUS = WIDTH / 2.0                           # pill caps
TILE_RADIUS = 0.22                             # of the tile's side
TILE_INSET = 0.14                              # the glyph's margin inside a tile

GLYPH_DARK = (232, 235, 241)    # #e8ebf1, on a dark background
GLYPH_LIGHT = (27, 32, 37)      # #1b2025, on a light one
PLATE = (243, 245, 248)         # #f3f5f8, the light tile
TILE_DARK = (18, 21, 25)        # #121519

SUPERSAMPLE = 8   # drawn this many times over, then reduced: PIL has no AA


def bars():
    """Every bar as (x, top, width, height) in box units, centred on the ink.

    The bars are hung from the zero line at y = 12, but they are not symmetric
    about it, so the ink is not centred there. Centring on the zero line sits
    the mark high in a tile, so everything is shifted by the difference (owner,
    2026-09-19).
    """
    n = len(PROFILE)
    span = BOX - 2 * PAD
    step = span / (n - 1)
    half = span / 2.0
    mid = BOX / 2.0
    raw = []
    for i, v in enumerate(PROFILE):
        centre_x = PAD + i * step
        length = abs(v) * half
        if v >= 0:
            top = mid - length
            length += WIDTH / 2.0
        else:
            top = mid - WIDTH / 2.0
            length += WIDTH / 2.0
        raw.append((centre_x - WIDTH / 2.0, top, WIDTH, length))
    top_edge = min(y for _, y, _, _ in raw)
    bottom_edge = max(y + h for _, y, _, h in raw)
    shift = (BOX - top_edge - bottom_edge) / 2.0
    return [(x, y + shift, w, h) for x, y, w, h in raw]


def draw(size, glyph, tile=None, inset=None):
    """The mark at `size` px, optionally on a rounded tile."""
    inset = TILE_INSET if (inset is None and tile is not None) else (inset or 0.0)
    s = size * SUPERSAMPLE
    image = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(image)
    if tile is not None:
        r = s * TILE_RADIUS
        d.rounded_rectangle([0, 0, s - 1, s - 1], radius=r, fill=tile)
    k = s / BOX * (1.0 - inset)
    offset = s * inset / 2.0
    for x, y, w, h in bars():
        d.rounded_rectangle([offset + x * k, offset + y * k,
                             offset + (x + w) * k, offset + (y + h) * k],
                            radius=RADIUS * k, fill=glyph)
    return image.resize((size, size), Image.LANCZOS)


def svg(glyph="#e8ebf1"):
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="256" height="256">']
    for x, y, w, h in bars():
        out.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="{w:.2f}" height="{h:.2f}" '
                   f'rx="{RADIUS:.2f}" fill="{glyph}"/>')
    out.append("</svg>")
    return "\n".join(out) + "\n"


def ico(sizes=(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)):
    """Explorer and a pinned taskbar shortcut. The light plate, because it has
    to read on a dark taskbar and on a light one."""
    base = draw(256, GLYPH_LIGHT, tile=PLATE)
    buffer = io.BytesIO()
    base.save(buffer, format="ICO", sizes=[(n, n) for n in sizes])
    return buffer.getvalue()


def bmp(width, height, background, glyph, tile=None, mark=None):
    """NSIS wants BMP, and does not read an alpha channel, so the background is
    painted rather than left transparent."""
    image = Image.new("RGB", (width, height), background)
    mark = mark or min(width, height)
    glyph_image = draw(mark, glyph, tile=tile)
    image.paste(glyph_image, ((width - mark) // 2, (height - mark) // 2), glyph_image)
    return image


ASSETS = {}


def build():
    ASSETS.clear()
    ASSETS["ui/res/isotone.ico"] = ico()
    ASSETS["ui/res/isotone.svg"] = svg().encode("utf-8")

    # NSIS: the 164 x 314 welcome panel and the 150 x 57 header.
    for path, image in [
        ("windows/setup/welcome.bmp", welcome_panel()),
        ("windows/setup/header.bmp", bmp(150, 57, PLATE, GLYPH_LIGHT, mark=44)),
    ]:
        buffer = io.BytesIO()
        image.save(buffer, format="BMP")
        ASSETS[path] = buffer.getvalue()

    # The Linux icon theme. hicolor wants one PNG per size, plus the SVG.
    for size in (16, 22, 24, 32, 48, 64, 128, 256):
        buffer = io.BytesIO()
        draw(size, GLYPH_LIGHT, tile=PLATE).save(buffer, format="PNG")
        ASSETS[f"linux/packaging/icons/hicolor/{size}x{size}/apps/isotone.png"] = buffer.getvalue()
    ASSETS["linux/packaging/icons/hicolor/scalable/apps/isotone.svg"] = svg("#1b2025").encode("utf-8")
    return ASSETS


def welcome_panel():
    """The installer's left-hand panel: the mark on the app's own background."""
    image = Image.new("RGB", (164, 314), TILE_DARK)
    glyph_image = draw(76, GLYPH_DARK)
    image.paste(glyph_image, ((164 - 76) // 2, 62), glyph_image)
    return image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="compare with what is checked in instead of writing")
    args = parser.parse_args()

    build()
    stale = []
    for path, data in ASSETS.items():
        full = os.path.join(ROOT, path)
        if args.check:
            if not os.path.exists(full) or open(full, "rb").read() != data:
                stale.append(path)
            continue
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data)
        print(f"{path}  {len(data):,} bytes")

    if args.check:
        if stale:
            print("out of date, run tools/gen_icons.py:", file=sys.stderr)
            for path in stale:
                print(f"  {path}", file=sys.stderr)
            return 1
        print(f"OK: {len(ASSETS)} icons match the mark")
    return 0


if __name__ == "__main__":
    sys.exit(main())
