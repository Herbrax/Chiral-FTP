#!/usr/bin/env bash
# Prepares release/ for a GitHub release: the package zip, its SHA-256 and the
# release notes (the version's section of CHANGELOG.md, with the checksum and
# install steps). Run through `make release`.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="${VERSION:?set VERSION}"
zip="chiral-ftp-$version.zip"
out="$root/release"

[[ -f "$root/dist/$zip" ]] || { echo "missing dist/$zip; run make ps5" >&2; exit 1; }
rm -rf "$out"
mkdir -p "$out"
cp "$root/dist/$zip" "$out/"
(cd "$out" && shasum -a 256 "$zip" > SHA256SUMS)
sum="$(cut -d' ' -f1 "$out/SHA256SUMS")"

python3 - "$root/CHANGELOG.md" "$version" "$out/RELEASE_NOTES.md" "$zip" "$sum" <<'PY'
import re, sys
changelog, version, out, zipname, digest = sys.argv[1:]
text = open(changelog, encoding="utf-8").read()
m = re.search(r"^## " + re.escape(version) + r"\n(.*?)(?=^## |\Z)", text, re.S | re.M)
if not m:
    sys.exit("CHANGELOG.md has no section for " + version)
notes = m.group(1).strip()
notes += f"""

### Install

1. Download `{zipname}` below and unpack it: it contains the folder `PPSA96024`.
2. Copy that folder to `/data/homebrew/PPSA96024` on the PS5.
3. Make sure the folder and every file in it are 777 (the zip stores them that
   way, but some FTP clients don't keep it), then start Chiral-FTP from the
   dashboard.

Updating: close chiral-ftp first, then replace the folder. Your settings and
saved sites stay in `/data/chiral-ftp`.

### Checksum

```
{digest}  {zipname}
```

Made by Herbrax (Simo Hakim). Bugs and pull requests are welcome:
https://github.com/Herbrax/Chiral-FTP-PS5/issues
"""
repo = "Herbrax/Chiral-FTP-PS5"
badges = (f"[![Downloads](https://img.shields.io/github/downloads/{repo}/v{version}/total?color=D9B65E&label=downloads)]"
          f"(https://github.com/{repo}/releases/tag/v{version}) "
          f"[![Stars](https://img.shields.io/github/stars/{repo}?color=D9B65E&label=stars)]"
          f"(https://github.com/{repo}/stargazers)")
open(out, "w", encoding="utf-8").write(f"# chiral-ftp {version}\n\n{badges}\n\n" + notes + "\n")
PY

ls -l "$out"
