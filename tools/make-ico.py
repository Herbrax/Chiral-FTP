#!/usr/bin/env python3
"""Wraps a 256x256 PNG in a .ico, for the Windows program's icon.

An .ico may hold a PNG as it is (Windows Vista and later read it), so this
needs no imaging library: a header, one directory entry, the PNG.

    python3 tools/make-ico.py assets/brand/icon-256.png gen/chiral-ftp.ico
"""
import struct, sys

src, dst = sys.argv[1], sys.argv[2]
png = open(src, 'rb').read()
if png[:8] != b'\x89PNG\r\n\x1a\n':
    sys.exit('%s: not a PNG' % src)
w, h = struct.unpack('>II', png[16:24])
if w > 256 or h > 256:
    sys.exit('%s: %dx%d, larger than an icon can be' % (src, w, h))
with open(dst, 'wb') as f:
    f.write(struct.pack('<HHH', 0, 1, 1))
    # width and height of 256 are written as 0
    f.write(struct.pack('<BBBBHHII', w & 255, h & 255, 0, 0, 1, 32, len(png), 6 + 16))
    f.write(png)
