#!/usr/bin/env python3
"""Renders assets/brand/icon.jpg, the icon hbmenu shows, from the geometry of
assets/brand/chiral-ftp-icon.svg.

The NACP format is strict: exactly 256x256, baseline JPEG, no alpha. A .nro
whose icon isn't that silently falls back to libnx's default, which is what
happened before this existed.

The polygons are copied from the SVG rather than parsed out of it -- the file
is eight hand-written polygons and a background, and a dependency on an SVG
renderer (the old shell script wanted headless Chrome *and* ImageMagick) is
not worth it for that. If the SVG changes, change these to match; BRAND.md
describes the mark.

    python3 tools/render-icon.py [output.jpg]
"""
import sys
from PIL import Image, ImageDraw

BACKGROUND = (0x0E, 0x0F, 0x11)
LIGHT = (0xF2, 0xD9, 0x8A)
DARK = (0xC7, 0x9F, 0x43)

# One stepped arrow, in the SVG's 200-unit arrow space. Light and dark facets
# alternate, exactly as chiral-ftp-icon.svg lists them.
ARROW = [
    (LIGHT, [(138, 34), (104, 84), (138, 84)]),
    (DARK, [(138, 34), (172, 84), (138, 84)]),
    (LIGHT, [(126, 84), (138, 84), (138, 110), (126, 110)]),
    (DARK, [(138, 84), (150, 84), (150, 110), (138, 110)]),
    (LIGHT, [(126, 110), (138, 110), (122, 126), (110, 126)]),
    (DARK, [(138, 110), (150, 110), (134, 126), (122, 126)]),
    (LIGHT, [(110, 126), (122, 126), (122, 164), (110, 152)]),
    (DARK, [(122, 126), (134, 126), (134, 152), (122, 164)]),
]

SIZE = 256
SUPERSAMPLE = 4  # drawn large and shrunk, so the diagonals are not jagged


def render(size):
    """The icon at size x size, as an RGB image."""
    # The SVG is 512 wide and puts the arrows at translate(56 56) scale(2),
    # so an arrow-space point maps to (56 + 2x, 56 + 2y) at 512.
    scale = size / 512.0
    img = Image.new('RGB', (size, size), BACKGROUND)
    draw = ImageDraw.Draw(img)

    def place(point, turned):
        x, y = point
        if turned:  # the second arrow: rotate(180 100 100)
            x, y = 200 - x, 200 - y
        return ((56 + 2 * x) * scale, (56 + 2 * y) * scale)

    for turned in (False, True):
        for colour, points in ARROW:
            draw.polygon([place(p, turned) for p in points], fill=colour)
    return img


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'assets/brand/icon.jpg'
    big = render(SIZE * SUPERSAMPLE)
    icon = big.resize((SIZE, SIZE), Image.LANCZOS)
    # Baseline (not progressive), 4:2:0, no alpha: what the NACP tool wants.
    icon.save(out, 'JPEG', quality=92, optimize=True, progressive=False,
              subsampling='4:2:0')
    print('%s: %dx%d baseline JPEG' % (out, icon.width, icon.height))


if __name__ == '__main__':
    main()
