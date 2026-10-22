#!/usr/bin/env bash
# dist/chiral-ftp-<version>-<arch>.AppImage: the desktop build as one file
# for any Linux distribution. Runs on Linux only (linuxdeploy bundles the
# shared libraries the program links from the build machine, so build on the
# oldest distribution you want to support -- the GitHub workflow uses Ubuntu
# 22.04). Settings go to ~/.local/share/chiral-ftp (platform/sdl/system.c).
#
#   VERSION=1.1 tools/make-appimage.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="${VERSION:?set VERSION}"
arch="$(uname -m)"
[[ "$(uname -s)" == Linux ]] || { echo "make-appimage.sh runs on Linux" >&2; exit 1; }
[[ -x "$root/chiral-ftp" ]] || make -C "$root" PLATFORM=sdl VERSION="$version"

tools="$root/.deps/appimage"
linuxdeploy="$tools/linuxdeploy-$arch.AppImage"
if [[ ! -x "$linuxdeploy" ]]; then
  mkdir -p "$tools"
  curl -fsSL -o "$linuxdeploy" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$arch.AppImage"
  chmod +x "$linuxdeploy"
  # An AppImage marks itself with "AI\x02" in its ELF header's padding, and
  # emulated runs (qemu, Rosetta: an x86_64 build on an Apple silicon Mac)
  # refuse such a header with "Exec format error". Blank the mark in this copy
  # of the tool; on real hardware it makes no difference.
  printf '\0\0\0' | dd of="$linuxdeploy" bs=1 seek=8 count=3 conv=notrunc 2>/dev/null
fi
# Its own pieces (the AppImage output plugin) are AppImages too: unpacked
# rather than run as they are, for the same reason and where FUSE is missing.
export APPIMAGE_EXTRACT_AND_RUN=1

appdir="$root/.build/AppDir"
rm -rf "$appdir"
mkdir -p "$appdir/usr/share/applications" "$appdir/usr/share/icons/hicolor/256x256/apps"
cat > "$appdir/usr/share/applications/chiral-ftp.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=chiral-ftp
Comment=FTP / SFTP server, web app and Quick connect for your consoles and computers
Exec=chiral-ftp
Icon=chiral-ftp
Categories=Network;FileTransfer;
Terminal=false
DESKTOP
cp "$root/assets/brand/icon-256.png" "$appdir/usr/share/icons/hicolor/256x256/apps/chiral-ftp.png"

mkdir -p "$root/dist"
cd "$root/dist"
OUTPUT="chiral-ftp-$version-$arch.AppImage" "$linuxdeploy" --appimage-extract-and-run \
  --appdir "$appdir" \
  --executable "$root/chiral-ftp" \
  --desktop-file "$appdir/usr/share/applications/chiral-ftp.desktop" \
  --icon-file "$appdir/usr/share/icons/hicolor/256x256/apps/chiral-ftp.png" \
  --output appimage
echo "built dist/chiral-ftp-$version-$arch.AppImage"
