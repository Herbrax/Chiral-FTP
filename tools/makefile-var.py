#!/usr/bin/env python3
"""Prints one variable of a Makefile that configure wrote, expanded.

    python3 tools/makefile-var.py <Makefile> <VARIABLE>

For tools/build-android-mhd.sh: which sources libmicrohttpd's configure
chose, without running make (a Windows machine has none, and the NDK's own
GNU make tries to remake the Makefile from paths it can't read). configure
has already settled the Makefile's conditionals, so plain assignments and
$(VAR) expansion are all there is to follow."""
import re
import sys


def read_vars(path):
    text = open(path, encoding="utf-8", errors="replace").read().replace("\\\n", " ")
    variables = {}
    for line in text.splitlines():
        if not line or line[0] in "#\t":
            continue
        m = re.match(r"^([A-Za-z0-9_.]+)\s*[:+]?=\s*(.*)$", line)
        if not m:
            continue
        name, value = m.group(1), m.group(2)
        if re.search(r"\+=\s*", line[: len(name) + 3]):
            variables[name] = variables.get(name, "") + " " + value
        else:
            variables[name] = value
    return variables


def expand(value, variables, depth=0):
    if depth > 20:
        return value
    return re.sub(r"\$\(([A-Za-z0-9_.]+)\)", lambda m: expand(variables.get(m.group(1), ""), variables, depth + 1), value)


variables = read_vars(sys.argv[1])
print(" ".join(expand(variables.get(sys.argv[2], ""), variables).split()))
