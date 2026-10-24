#!/usr/bin/env python3
"""The PS5's contentVersion, from the app's version (VERSION in the Makefile).

A console finds an update by comparing contentVersion ("01.001.000"), not
the release tag, and so does the PS5 homebrew catalog, which reads it from
platform/ps5/sce_sys/param.json at the release's tag. It must therefore go
up with every release: deriving it from VERSION means it can't be forgotten.

    1.1 -> 01.001.000    1.1.2 -> 01.001.002    2.0 -> 02.000.000

    content-version.py 1.1                  print it
    content-version.py --sync 1.1 FILE      write it into FILE (a param.json)
    content-version.py --check 1.1 FILE     fail if FILE says otherwise
"""
import re, sys


def content_version(version):
    m = re.fullmatch(r'(\d+)\.(\d+)(?:\.(\d+))?', version)
    if not m or int(m[1]) > 99 or int(m[2]) > 999 or int(m[3] or 0) > 999:
        sys.exit('VERSION %r has no PS5 contentVersion: use MAJOR.MINOR or MAJOR.MINOR.PATCH '
                 '(up to 99.999.999)' % version)
    return '%02d.%03d.%03d' % (int(m[1]), int(m[2]), int(m[3] or 0))


# Only the value is touched, so the file keeps its layout and the diff is one line.
FIELD = re.compile(r'("contentVersion"\s*:\s*")([^"]*)(")')


def main(argv):
    if len(argv) == 1:
        print(content_version(argv[0]))
        return 0
    if len(argv) != 3 or argv[0] not in ('--sync', '--check'):
        sys.exit(__doc__)
    mode, want, path = argv[0], content_version(argv[1]), argv[2]
    text = open(path, encoding='utf-8').read()
    found = FIELD.search(text)
    if not found:
        sys.exit('%s has no contentVersion' % path)
    if found[2] == want:
        return 0
    if mode == '--check':
        print('%s: contentVersion is %s, but VERSION %s needs %s (make PLATFORM=ps5 updates it)'
              % (path, found[2], argv[1], want), file=sys.stderr)
        return 1
    open(path, 'w', encoding='utf-8').write(FIELD.sub(r'\g<1>%s\g<3>' % want, text, count=1))
    print('%s: contentVersion %s -> %s (commit it with the release)' % (path, found[2], want))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
