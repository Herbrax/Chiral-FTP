#!/usr/bin/env bash
# Prepares release/ for a GitHub release: each platform's file, their SHA-256
# and the release notes (the version's section of CHANGELOG.md, with install
# steps and checksums for every file present). Run through
# `make PLATFORM=<ps5|switch> release`; each run adds its platform's file
# and keeps the others', so one release can carry them all:
#
#   make PLATFORM=ps5 release && make PLATFORM=switch release && make PLATFORM=sdl release
#
# On a Mac, PLATFORM=sdl adds the .app (zipped); on Windows, the .exe (x64
# or arm64, whichever the shell builds for); the Linux AppImage comes from
# tools/make-appimage.sh on Linux (the GitHub workflow builds it) and is
# added with tools/make-release.sh linux once it is in dist/.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="${VERSION:?set VERSION}"
platform="${1:?usage: make-release.sh <switch|ps5|mac|linux|windows>}"
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
    # One per architecture (x86_64, aarch64), side by side in the release.
    appimages=$(ls "$root/dist/chiral-ftp-$version-"*.AppImage 2>/dev/null || true)
    [[ -n "$appimages" ]] || {
      echo "missing dist/chiral-ftp-$version-*.AppImage; run tools/make-appimage.sh on Linux" >&2; exit 1; }
    rm -f "$out"/chiral-ftp-*.AppImage
    for a in $appimages; do cp "$a" "$out/"; done
    ;;
  windows)
    # The program itself: one .exe per architecture, x64 and arm64 side by
    # side in the same release.
    exes=$(cd "$root/dist" 2>/dev/null && ls chiral-ftp-"$version"-windows-*.exe 2>/dev/null || true)
    [[ -n "$exes" ]] || {
      echo "missing dist/chiral-ftp-$version-windows-*.exe; run make PLATFORM=sdl dist in MSYS2" >&2; exit 1; }
    for exe in $exes; do cp "$root/dist/$exe" "$out/$exe"; done
    ;;
  *) echo "unknown platform: $platform" >&2; exit 2 ;;
esac

files=()
for f in "chiral-ftp-ps5-$version.zip" chiral-ftp.nro $(cd "$out" && ls chiral-ftp-"$version"-*.AppImage chiral-ftp-"$version"-windows-*.exe 2>/dev/null); do
  [[ -f "$out/$f" ]] && files+=("$f")
done
# The Mac app is a folder: its checksum is the program inside it.
[[ -f "$out/chiral-ftp.app/Contents/MacOS/chiral-ftp" ]] && files+=("chiral-ftp.app/Contents/MacOS/chiral-ftp")
# MSYS2's sha256sum marks every file binary ("<digest> *name"); the notes
# want the plain name.
sha() { if command -v sha256sum >/dev/null; then sha256sum "$@" | sed 's/ \*/  /'; else shasum -a 256 "$@"; fi; }
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
appimages = sorted(f for f in present if f.endswith(".AppImage"))
if appimages:
    # x86_64 for most PCs, aarch64 for ARM ones: say which is which when both are there.
    def kind(f):
        return "most PCs" if "x86_64" in f else "ARM computers, such as a Raspberry Pi 4 or 5 on 64-bit" if "aarch64" in f else ""
    pick = (" or ".join(f"`{f}` ({kind(f)})" for f in appimages) if len(appimages) > 1 else f"`{appimages[0]}`")
    notes += f"""
**Linux** (Ubuntu 22.04, Debian 12, Fedora 36 or newer, and others like them)

1. Download {pick} below.
2. Make it executable (`chmod +x chiral-ftp-*.AppImage`, or Properties >
   Allow executing as a program) and run it.

Updating: replace the file. Your settings stay in `~/.local/share/chiral-ftp`.
"""
exes = [f for f in present if f.endswith(".exe")]
if exes:
    names = " or ".join(f"`{f}`" for f in exes)
    # With both, say which is which; with x64 alone, that it covers ARM PCs too.
    which = ("(arm64 for a Windows on ARM PC, x64 for any other)" if len(exes) > 1
             else "(it runs on Windows on ARM PCs too, through Windows' own x64 emulation)" if "x64" in exes[0]
             else "")
    notes += f"""
**Windows** (Windows 10 or 11)

1. Download {names} below {which}.
2. Run it; nothing needs installing. The program isn't signed, so the first
   time Windows SmartScreen says it doesn't recognise it: choose
   **More info > Run anyway**. When Windows Firewall asks, allow private
   networks, so that a console or phone can reach the server.

Updating: replace the `.exe`. Your settings stay in `%APPDATA%\\chiral-ftp`.
To keep everything on a USB stick instead, settings included, make a folder
named `data` next to the `.exe`: chiral-ftp then keeps its settings there.
"""
else:
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
