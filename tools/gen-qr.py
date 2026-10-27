#!/usr/bin/env python3
"""Prints a QR code as the C rows app/ui.cpp draws it (kDownloadQr).

    pip install segno
    tools/gen-qr.py https://chiral-ftp.com/download

One uint32_t per row, the leftmost module in bit 0; the quiet zone around it
is the UI's to draw. The smallest symbol that holds the text, so each module
is as big as it can be -- the code is drawn small, and often on a TV across
a room -- with the most error correction that symbol has room for.
"""
import sys

import segno


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    url = sys.argv[1]
    qr = segno.make(url, micro=False, boost_error=True)
    rows = [row for row in qr.matrix]
    n = len(rows)
    if n > 32:
        sys.exit("%d modules a side: too big for a uint32_t row" % n)
    print("// %s: QR %s, %d x %d (tools/gen-qr.py)" % (url, qr.designator, n, n))
    print("const int kDownloadQrSize = %d;" % n)
    print("const std::uint32_t kDownloadQr[%d] = {" % n)
    for row in rows:
        bits = sum(1 << x for x, dark in enumerate(row) if dark)
        print("    0x%08X," % bits)
    print("};")


if __name__ == "__main__":
    main()
