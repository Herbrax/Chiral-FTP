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

"${PYTHON:-python3}" "$root/tools/release-notes.py" "$root/CHANGELOG.md" "$version" "$out/RELEASE_NOTES.md" "$out/SHA256SUMS"

ls -l "$out"
