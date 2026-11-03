#!/usr/bin/env bash
# The Mac's disk image, chiral-ftp-<version>.dmg: chiral-ftp.app beside a link
# to Applications below it, on docs/screenshots/Mockups/mac_dmg_background.png
# (tools/make-dmg-background.py: the PS5's launch picture with chevrons from
# the one to the other). The updater opens it the same way
# (platform/sdl/update.cpp) and copies the only .app it finds.
#
#     tools/make-dmg.sh <chiral-ftp.app> <out.dmg>
#
# macOS only: hdiutil, sips, tiffutil and Finder (through osascript) for the
# window.
# When Finder doesn't answer, as on a runner now and then, the image is made
# without the window's layout rather than not at all.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
app="$1" out="$2"
name=chiral-ftp
work="$(mktemp -d "${TMPDIR:-/tmp}/chiral-dmg.XXXXXX")"
mnt=""
cleanup() {
  [ -z "$mnt" ] || hdiutil detach -force "$mnt" > /dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT

mkdir -p "$work/src/.background"
ditto "$app" "$work/src/$name.app"
ln -s /Applications "$work/src/Applications"
# The window's size and twice it, in one file: Finder picks the 2x on a
# Retina screen.
background="$root/docs/screenshots/Mockups/mac_dmg_background.png"
sips -z 540 960 "$background" --out "$work/background.png" > /dev/null
sips -z 1080 1920 "$background" --out "$work/background@2x.png" > /dev/null
tiffutil -cathidpicheck "$work/background.png" "$work/background@2x.png" \
  -out "$work/src/.background/background.tiff"

# Writable first, for Finder's .DS_Store; room for it on top of the app.
size=$(( $(du -sm "$work/src" | cut -f1) + 20 ))
# hdiutil sometimes finds the runner's disk busy: three tries.
for attempt in 1 2 3; do
  hdiutil create -volname "$name" -srcfolder "$work/src" -fs HFS+ -format UDRW -size "${size}m" -ov \
    "$work/rw.dmg" > /dev/null && break
  [ "$attempt" = 3 ] && exit 1
  sleep 10
done
mnt="$(hdiutil attach -readwrite -noverify -noautoopen "$work/rw.dmg" | grep -o '/Volumes/.*$' | tail -1)"
[ -d "$mnt" ] || { echo "the disk image didn't mount" >&2; exit 1; }
disk="$name"  # Finder knows a disk by its volume's name

# 960x540, the background's size: the app at the top left, Applications below
# it, the picture's chevrons between them. The places and the icon size are
# tools/make-dmg-background.py's: change them together.
layout() {
  osascript <<EOF
tell application "Finder"
  tell disk "$disk"
    open
    set current view of container window to icon view
    set toolbar visible of container window to false
    set statusbar visible of container window to false
    set the bounds of container window to {200, 120, 1160, 660}
    set opts to the icon view options of container window
    set arrangement of opts to not arranged
    set icon size of opts to 128
    set text size of opts to 14
    set background picture of opts to file ".background:background.tiff"
    set position of item "$name.app" of container window to {160, 120}
    set position of item "Applications" of container window to {160, 390}
    update without registering applications
    delay 2
    close
  end tell
end tell
EOF
}
laid_out=no
for attempt in 1 2 3; do
  if layout; then laid_out=yes; break; fi
  sleep 5
done
if [ "$laid_out" = yes ]; then
  # Finder writes .DS_Store a moment after the window closes.
  for _ in $(seq 1 10); do [ -f "$mnt/.DS_Store" ] && break; sleep 1; done
else
  echo "::warning::Finder didn't lay out the disk image's window; it opens as a plain list"
fi
rm -rf "$mnt/.fseventsd"
sync
hdiutil detach "$mnt" > /dev/null || hdiutil detach -force "$mnt" > /dev/null
mnt=""

hdiutil convert "$work/rw.dmg" -format UDZO -imagekey zlib-level=9 -ov -o "$out" > /dev/null
hdiutil verify "$out"
