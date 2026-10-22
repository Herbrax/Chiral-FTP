#!/usr/bin/env bash
# libusbhsfs (DarkMatterCore) for the Switch build: USB drives, as Goldleaf
# reads them. Same commit as Goldleaf's submodule (b1ff881), same GPL build
# (NTFS through ntfs-3g, EXT2/3/4 through lwext4), installed into
# .deps/switch like the other dependencies. Needs the two portlibs once:
#   sudo dkp-pacman -S switch-ntfs-3g switch-lwext4
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
src="$root/.deps/build-switch/libusbhsfs"
out="$root/.deps/switch"
commit=b1ff8811a762adf1b585b762b21308d458d43bfa
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
for lib in libntfs-3g.a liblwext4.a; do
  [[ -f "$DEVKITPRO/portlibs/switch/lib/$lib" ]] || { echo "missing $lib: sudo dkp-pacman -S switch-ntfs-3g switch-lwext4" >&2; exit 1; }
done
if [[ ! -d "$src/.git" ]]; then
  mkdir -p "$(dirname "$src")"
  git clone -q https://github.com/DarkMatterCore/libusbhsfs.git "$src"
fi
cd "$src"
git checkout -q -- Makefile
git checkout -q "$commit"
# This devkitA64's GCC is newer than the 2024 code and turns a variable that
# is set but never read into an error under -Werror; leave it a warning.
sed -i.orig 's/BUILD_CFLAGS="-DNDEBUG=1 -O2"/BUILD_CFLAGS="-DNDEBUG=1 -O2 -Wno-error=unused-but-set-variable"/' Makefile
make BUILD_TYPE=GPL -j8 >/dev/null
mkdir -p "$out/lib" "$out/include"
cp lib/libusbhsfs.a "$out/lib/"
cp include/usbhsfs.h "$out/include/"
cp LICENSE_GPLv2+.md "$out/libusbhsfs-LICENSE.md"
echo "libusbhsfs ${commit:0:7} (GPL) in $out"
