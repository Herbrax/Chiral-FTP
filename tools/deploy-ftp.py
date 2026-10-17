#!/usr/bin/env python3
"""Copies a built title folder (dist/PPSA96024) to the PS5 over FTP.

  make deploy PS5_HOST=192.168.18.18 [PS5_PORT=2121]

Use an FTP server that runs outside the app's sandbox (ftpsrv on 2121).
Only changed files are sent. Each goes up as "<name>.upload", is read back and
compared, then renamed over the old file: the console refuses an in-place
overwrite of an eboot.bin it considers in use, but not a rename. The old
eboot.bin is kept as eboot.bin.old. Everything ends up 777.
"""

import argparse
import ftplib
import hashlib
import io
import os
import posixpath
import sys


def sha(data):
    return hashlib.sha256(data).hexdigest()


def remote_read(ftp, path):
    buf = io.BytesIO()
    try:
        ftp.retrbinary("RETR " + path, buf.write)
    except ftplib.error_perm:
        return None
    return buf.getvalue()


def chmod777(ftp, path):
    for cmd in ("SITE CHMOD 777 " + path, "CHMOD 777 " + path):
        try:
            ftp.sendcmd(cmd)
            return
        except ftplib.error_perm:
            continue
    print("  warning: could not set 777 on " + path)


def ensure_dir(ftp, path):
    try:
        ftp.mkd(path)
    except ftplib.error_perm:
        pass  # already there
    chmod777(ftp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", help="the built title folder, e.g. dist/PPSA96024")
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=2121)
    ap.add_argument("--target", default="/data/homebrew")
    args = ap.parse_args()

    title = os.path.basename(os.path.normpath(args.folder))
    dest = posixpath.join(args.target, title)
    ftp = ftplib.FTP()
    ftp.connect(args.host, args.port, timeout=30)
    ftp.login()
    print("Deploying %s to ftp://%s:%d%s" % (title, args.host, args.port, dest))
    ensure_dir(ftp, dest)
    sent = 0
    for root, dirs, files in os.walk(args.folder):
        dirs.sort()
        rel = os.path.relpath(root, args.folder)
        rdir = dest if rel == "." else posixpath.join(dest, rel.replace(os.sep, "/"))
        if rel != ".":
            ensure_dir(ftp, rdir)
        for name in sorted(files):
            local = open(os.path.join(root, name), "rb").read()
            rpath = posixpath.join(rdir, name)
            old = remote_read(ftp, rpath)
            if old is not None and sha(old) == sha(local):
                continue
            tmp = rpath + ".upload"
            ftp.storbinary("STOR " + tmp, io.BytesIO(local))
            back = remote_read(ftp, tmp)
            if back is None or sha(back) != sha(local):
                sys.exit("error: %s did not arrive intact; nothing replaced" % rpath)
            if old is not None:
                keep = rpath + ".old" if name == "eboot.bin" else None
                if keep:
                    try:
                        ftp.delete(keep)
                    except ftplib.error_perm:
                        pass
                    ftp.rename(rpath, keep)
                else:
                    ftp.delete(rpath)
            ftp.rename(tmp, rpath)
            chmod777(ftp, rpath)
            sent += 1
            print("  updated " + posixpath.relpath(rpath, dest))
    ftp.quit()
    print("Done: %d file%s updated. Close chiral-ftp on the PS5 and start it again." % (sent, "" if sent == 1 else "s")
          if sent else "Already up to date.")


if __name__ == "__main__":
    main()
