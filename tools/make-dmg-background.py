#!/usr/bin/env python3
"""The Mac disk image's window picture, docs/screenshots/Mockups/mac_dmg_background.png:
the PS5's launch picture (ps5_background.png, 3840x2160) with the Mac's
tagline, and chevrons in the mark's two golds leading from where
tools/make-dmg.sh puts chiral-ftp.app down to where it puts Applications.
Needs Pillow.

    tools/make-dmg-background.py

The window is 960x540 points, so the picture is it at 4x. The places below
are the ones in tools/make-dmg.sh: change them together.
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "docs", "screenshots", "Mockups", "ps5_background.png")
OUT = os.path.join(ROOT, "docs", "screenshots", "Mockups", "mac_dmg_background.png")
FONT = os.path.join(ROOT, "assets", "fonts", "IBMPlexMono-Medium.ttf")

SCALE = 4                  # pixels per window point
COLUMN_X = 160             # both icons' centre, in points
APP_Y, APPLICATIONS_Y = 120, 390
ICON = 128                 # icon size in points
LABEL = 28                 # the name under an icon, with its gap

# The mark's faces (assets/brand/chiral-ftp-mark.svg). Its down arrow has the
# deep one on the left, so the chevrons do too.
LIGHT, DARK = (0xF2, 0xD9, 0x8A), (0xC7, 0x9F, 0x43)

# The PS5 picture's tagline, in pixels: IBM Plex Mono Medium at 55 px, this
# grey, its ink in this box (measured from ps5_background.png).
TAGLINE_BOX = (2494, 1660, 3347, 1714)
TAGLINE_GREY = (142, 138, 128)
TAGLINE_SIZE = 55
TAGLINE = "Ultimate FTP Client"


def retitle(picture):
    """The PS5's tagline out, the Mac's in, at the same size, centre and line."""
    px = picture.load()
    x0, y0, x1, y1 = TAGLINE_BOX
    x0, y0, x1, y1 = x0 - 12, y0 - 10, x1 + 12, y1 + 10
    # The background is a smooth gradient: each column, from the row above
    # the text to the row below it.
    for x in range(x0, x1 + 1):
        top, bottom = px[x, y0 - 1], px[x, y1 + 1]
        for y in range(y0, y1 + 1):
            t = (y - y0 + 1) / (y1 - y0 + 2)
            px[x, y] = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))
    font = ImageFont.truetype(FONT, TAGLINE_SIZE)
    # The old line's ink began at its top; the same font puts a capital's top
    # this far below the drawing point.
    old = font.getbbox("file manager & FTP for PS5")
    new = font.getbbox(TAGLINE)
    centre = (TAGLINE_BOX[0] + TAGLINE_BOX[2]) / 2
    origin = (round(centre - (new[0] + new[2]) / 2), TAGLINE_BOX[1] - old[1])
    ImageDraw.Draw(picture).text(origin, TAGLINE, font=font, fill=TAGLINE_GREY + (255,))


def chevron(cx, top, half, drop, thick):
    """A V pointing down, as two faces: left arm, right arm (points)."""
    left = [(cx - half, top), (cx, top + drop), (cx, top + drop + thick), (cx - half, top + thick)]
    right = [(cx, top + drop), (cx + half, top), (cx + half, top + thick), (cx, top + drop + thick)]
    return left, right


def chevrons(picture):
    """Three, faint to full, filling the gap between the app's name and the
    Applications icon: the way to drag it."""
    top = APP_Y + ICON / 2 + LABEL + 12
    bottom = APPLICATIONS_Y - ICON / 2 - 12
    half, drop, thick, step = 27, 17, 10, 27  # points
    count = 3
    span = (count - 1) * step + drop + thick
    first = top + (bottom - top - span) / 2
    # Drawn at 4x again and scaled down: Pillow's polygons have no smoothing.
    over = 4
    k = SCALE * over
    w = int(2 * half * k) + 2 * over
    h = int(span * k) + 2 * over
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for i, alpha in enumerate((90, 165, 255)):
        part = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        draw = ImageDraw.Draw(part)
        left, right = chevron(half, i * step, half, drop, thick)
        for face, colour in ((left, DARK), (right, LIGHT)):
            draw.polygon([(x * k + over, y * k + over) for x, y in face], fill=colour + (alpha,))
        layer.alpha_composite(part)
    layer = layer.resize((w // over, h // over), Image.LANCZOS)
    picture.alpha_composite(layer, (round((COLUMN_X - half) * SCALE) - 1, round(first * SCALE) - 1))


def main():
    picture = Image.open(SRC).convert("RGBA")
    if picture.size != (960 * SCALE, 540 * SCALE):
        raise SystemExit(f"{SRC} is {picture.size}, not 3840x2160")
    retitle(picture)
    chevrons(picture)
    picture.convert("RGB").save(OUT, optimize=True)
    print(OUT)


if __name__ == "__main__":
    main()
