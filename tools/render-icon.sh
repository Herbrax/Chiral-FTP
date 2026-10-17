#!/usr/bin/env bash
# Renders sce_sys/icon0.png (512x512 RGB, what the PS5 dashboard shows) from
# assets/brand/chiral-ftp-icon.svg with headless Chrome, flattened onto the
# icon's own background colour (#0E0F11).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
chrome="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
work="$(mktemp -d "${TMPDIR:-/tmp}/chiral-icon.XXXXXX")"
trap 'rm -rf "$work"' EXIT
cat > "$work/icon.html" <<HTML
<!doctype html><html><head><style>html,body{margin:0;width:512px;height:512px;background:#0E0F11;overflow:hidden}
img{display:block;width:512px;height:512px}</style></head>
<body><img src="file://$root/assets/brand/chiral-ftp-icon.svg"></body></html>
HTML
"$chrome" --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 \
  --user-data-dir="$work/profile" --allow-file-access-from-files --window-size=512,512 \
  --screenshot="$root/sce_sys/icon0.png" "file://$work/icon.html" 2>/dev/null
file "$root/sce_sys/icon0.png"
