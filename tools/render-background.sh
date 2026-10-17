#!/usr/bin/env bash
# Renders the dashboard background, sce_sys/pic0.dds and pic1.dds (the
# picture behind the title when it's selected, and its launch screen), from
# assets/brand/background.html: headless Chrome draws it at 3840x2160, then
# tools/bc7enc.c encodes it as the BC7 DDS the PS5 expects.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
chrome="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
work="$(mktemp -d "${TMPDIR:-/tmp}/chiral-background.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc -O2 -o "$work/bc7enc" "$root/tools/bc7enc.c" -lm
"$chrome" --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 \
  --user-data-dir="$work/profile" --allow-file-access-from-files --window-size=3840,2160 \
  --virtual-time-budget=3000 --screenshot="$work/background.png" \
  "file://$root/assets/brand/background.html" 2>/dev/null
sips -s format bmp "$work/background.png" --out "$work/background.bmp" >/dev/null
"$work/bc7enc" "$work/background.bmp" "$root/sce_sys/pic0.dds"
cp "$root/sce_sys/pic0.dds" "$root/sce_sys/pic1.dds"
cp "$work/background.png" "$root/assets/brand/background.png"  # preview
ls -l "$root/sce_sys/pic0.dds" "$root/sce_sys/pic1.dds"
