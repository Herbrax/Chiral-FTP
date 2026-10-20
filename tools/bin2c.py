#!/usr/bin/env python3
"""Embeds a binary file as a C array: bin2c.py SYMBOL INPUT > out.c

An empty input gives a one-byte array with size 0, because `{}` is not a
valid C initialiser. The Switch build relies on this for the optional
Arabic font (see the Makefile).
"""
import sys

name, path = sys.argv[1], sys.argv[2]
data = open(path, "rb").read()
print("const unsigned char %s[] = {" % name)
if data:
    for i in range(0, len(data), 20):
        print("  " + ",".join(str(b) for b in data[i:i + 20]) + ",")
else:
    print("  0,")
print("};")
print("const unsigned long %s_size = %d;" % (name, len(data)))
