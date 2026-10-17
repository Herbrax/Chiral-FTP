#!/usr/bin/env python3
"""Embeds a binary file as a C array: bin2c.py SYMBOL INPUT > out.c"""
import sys

name, path = sys.argv[1], sys.argv[2]
data = open(path, "rb").read()
print("const unsigned char %s[] = {" % name)
for i in range(0, len(data), 20):
    print("  " + ",".join(str(b) for b in data[i:i + 20]) + ",")
print("};")
print("const unsigned long %s_size = %d;" % (name, len(data)))
