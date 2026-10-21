#!/usr/bin/env bash
# Prepares release/ for a GitHub release: each platform's file, their SHA-256
# and the release notes (the version's section of CHANGELOG.md, with install
# steps and checksums for every file present). Run through
# `make PLATFORM=<ps5|switch> release`; each run adds its platform's file
# and keeps the others', so one release can carry them all:
#
#   make PLATFORM=ps5 release && make PLATFORM=switch release && make PLATFORM=sdl release
#
# On a Mac, PLATFORM=sdl adds the .app (zipped); the Linux AppImage comes
# from tools/make-appimage.sh on Linux (the GitHub workflow builds it) and is
# added with tools/make-release.sh linux once it is in dist/.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="${VERSION:?set VERSION}"
platform="${1:?usage: make-release.sh <switch|ps5|mac|linux>}"
out="$root/release"
mkdir -p "$out"

case "$platform" in
  switch)
    # The .nro itself: hbmenu lists it from /switch. Not versioned, so an
    # update replaces the old file rather than sitting next to it.
    [[ -f "$root/dist/chiral-ftp/chiral-ftp.nro" ]] || {
      echo "missing dist/chiral-ftp/chiral-ftp.nro; run make PLATFORM=switch" >&2; exit 1; }
    cp "$root/dist/chiral-ftp/chiral-ftp.nro" "$out/chiral-ftp.nro"
    ;;
  ps5)
    # The zip the PS5 build made: it stores every mode as 777, which the
    # console needs to start the title.
    [[ -f "$root/dist/chiral-ftp-$version.zip" ]] || {
      echo "missing dist/chiral-ftp-$version.zip; run make PLATFORM=ps5" >&2; exit 1; }
    rm -f "$out"/chiral-ftp-ps5-*.zip
    cp "$root/dist/chiral-ftp-$version.zip" "$out/chiral-ftp-ps5-$version.zip"
    ;;
  mac)
    # The .app itself, not zipped (the maintainer's choice), copied with
    # ditto so its symlinks and code signature survive. A .app is a folder:
    # a GitHub release needs it wrapped in a single file (a .dmg) to upload.
    [[ -d "$root/dist/chiral-ftp.app" ]] || {
      echo "missing dist/chiral-ftp.app; run make PLATFORM=sdl app" >&2; exit 1; }
    rm -rf "$out"/chiral-ftp-mac-*.zip "$out/chiral-ftp.app"
    ditto "$root/dist/chiral-ftp.app" "$out/chiral-ftp.app"
    ;;
  linux)
    appimage=$(ls "$root/dist/chiral-ftp-$version-"*.AppImage 2>/dev/null | head -1)
    [[ -n "$appimage" ]] || {
      echo "missing dist/chiral-ftp-$version-*.AppImage; run tools/make-appimage.sh on Linux" >&2; exit 1; }
    rm -f "$out"/chiral-ftp-*.AppImage
    cp "$appimage" "$out/"
    ;;
  *) echo "unknown platform: $platform" >&2; exit 2 ;;
esac

files=()
for f in "chiral-ftp-ps5-$version.zip" chiral-ftp.nro $(cd "$out" && ls chiral-ftp-"$version"-*.AppImage 2>/dev/null); do
  [[ -f "$out/$f" ]] && files+=("$f")
done
# The Mac app is a folder: its checksum is the program inside it.
[[ -f "$out/chiral-ftp.app/Contents/MacOS/chiral-ftp" ]] && files+=("chiral-ftp.app/Contents/MacOS/chiral-ftp")
sha() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
(cd "$out" && sha "${files[@]}" > SHA256SUMS)

"${PYTHON:-python3}" - "$root/CHANGELOG.md" "$version" "$out/RELEASE_NOTES.md" "$out/SHA256SUMS" <<'PY'
import re, sys
changelog, version, out, sums = sys.argv[1:]
text = open(changelog, encoding="utf-8").read()
m = re.search(r"^## " + re.escape(version) + r"\n(.*?)(?=^## |\Z)", text, re.S | re.M)
if not m:
    sys.exit("CHANGELOG.md has no section for " + version)
digests = open(sums, encoding="utf-8").read().strip()
present = digests.split()
# Pictures in the changelog point into the repository; a release page needs the full address.
body = m.group(1).strip().replace("](docs/", "](https://raw.githubusercontent.com/Herbrax/chiral-ftp/main/docs/")
notes = body + "\n\n### Install\n"
if f"chiral-ftp-ps5-{version}.zip" in present:
    notes += f"""
**PS5**

1. Download `chiral-ftp-ps5-{version}.zip` below and unpack it: it contains
   the folder `PPSA96024`.
2. Copy that folder to `/data/homebrew/PPSA96024` on the PS5.
3. Make sure the folder and every file in it are 777 (the zip stores them
   that way, but some FTP clients don't keep it), then start Chiral-FTP from
   the dashboard.

Updating: close chiral-ftp first, then replace the folder. Your settings and
saved sites stay in `/data/chiral-ftp`.
"""
if "chiral-ftp.nro" in present:
    notes += """
**Nintendo Switch**

1. Download `chiral-ftp.nro` below.
2. Copy it into `/switch` on your SD card.
3. Start it from hbmenu. For the whole console's memory, hold **R** while
   launching a game and pick chiral-ftp from the menu that opens; starting it
   from the album works too, with less memory.

Updating: replace the `.nro`. Your settings and saved sites stay in
`/switch/chiral-ftp`.
"""
if "chiral-ftp.app/Contents/MacOS/chiral-ftp" in present:
    notes += f"""
**Mac** (macOS 27 or later, Apple silicon)

1. Download `chiral-ftp.app` below.
2. Drag it to Applications.
3. The app isn't notarized by Apple, so the first time macOS says it can't
   check it: open **System Settings > Privacy & Security** and choose
   **Open Anyway** (or run `xattr -dr com.apple.quarantine
   /Applications/chiral-ftp.app` once).

Updating: replace the app. Your settings stay in
`~/Library/Application Support/chiral-ftp`.
"""
appimages = [f for f in present if f.endswith(".AppImage")]
if appimages:
    notes += f"""
**Linux**

1. Download `{appimages[0]}` below.
2. Make it executable (`chmod +x {appimages[0]}`) and run it.

Updating: replace the file. Your settings stay in `~/.local/share/chiral-ftp`.
"""
notes += """
**Windows** is coming soon.
"""
notes += f"""
### Checksums

```
{digests}
```

Made by Herbrax (Simo Hakim). Bugs and pull requests are welcome:
https://github.com/Herbrax/chiral-ftp/issues
"""
repo = "Herbrax/chiral-ftp"
badges = (f"[![Downloads](https://img.shields.io/github/downloads/{repo}/v{version}/total?color=D9B65E&label=downloads)]"
          f"(https://github.com/{repo}/releases/tag/v{version}) "
          f"[![Stars](https://img.shields.io/github/stars/{repo}?color=D9B65E&label=stars)]"
          f"(https://github.com/{repo}/stargazers)")
open(out, "w", encoding="utf-8").write(f"# chiral-ftp {version}\n\n{badges}\n\n" + notes)
PY

ls -l "$out"
