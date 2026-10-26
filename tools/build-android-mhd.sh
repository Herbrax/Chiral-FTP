#!/usr/bin/env bash
# libmicrohttpd for Android, for the web app: the same version and checksum
# as the Switch (tools/build-switch-deps.sh), built for each ABI into
# .deps/android/<abi>.
#
#   tools/build-android-mhd.sh      (tools/build-android.sh runs it when needed)
#
# Its configure script is run as it is, once per ABI, with the NDK's clang:
# that is what finds out what bionic has (epoll, eventfd, sendfile all do
# exist here, unlike on Horizon). Then, rather than needing make and
# libtool -- a Windows machine has neither -- the sources configure chose
# are compiled straight into a static library. Which sources those are is
# read from the Makefile configure wrote (tools/makefile-var.py).
set -euo pipefail

MHD_VER=1.0.10
MHD_SHA256=04bfe8ef75db7d629a33de767599765cecadc56274a39822d5d081030d577685
NDK_VER=28.2.13676358
API=26

here="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -z "${ANDROID_HOME:-}" ]]; then
  case "$(uname -s)" in
    Darwin) ANDROID_HOME="$HOME/Library/Android/sdk" ;;
    MINGW*|MSYS*|CYGWIN*) ANDROID_HOME="$(cygpath -u "$LOCALAPPDATA")/Android/Sdk" ;;
    *) ANDROID_HOME="$HOME/Android/Sdk" ;;
  esac
fi
ndk="$ANDROID_HOME/ndk/$NDK_VER"
bin="$(ls -d "$ndk"/toolchains/llvm/prebuilt/*/bin | head -1)"
PYTHON="${PYTHON:-$(command -v python3 || command -v python)}"
exe=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) exe=".exe" ;; esac

sha256() {
  if command -v shasum >/dev/null; then shasum -a 256 "$@"; else sha256sum "$@"; fi
}

src="$here/.deps/src"
mkdir -p "$src"
tarball="$src/libmicrohttpd-$MHD_VER.tar.gz"
if [[ ! -f "$tarball" ]]; then
  curl -fsSL -o "$tarball.part" "https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-$MHD_VER.tar.gz"
  mv "$tarball.part" "$tarball"
fi
echo "$MHD_SHA256  $tarball" | sha256 -c - >/dev/null
work="$here/.deps/build-android/mhd"
rm -rf "$work"
mkdir -p "$work"
# On stdin, not by name: tar reads the "C:" of a Windows path as a host.
tar -xzf - -C "$work" < "$tarball"
mhd="$work/libmicrohttpd-$MHD_VER"

# configure insists on pkg-config even with HTTPS off, only to ask about
# GnuTLS, which this build leaves out. A stand-in that exists and finds
# nothing is all it needs (a Windows machine usually has no pkg-config).
fake_pc="$work/pkg-config"
printf '#!/bin/sh\ncase "$1" in\n  --version) echo 0.29.2 ;;\n  --atleast-pkgconfig-version) ;;\n  *) exit 1 ;;\nesac\n' > "$fake_pc"
chmod +x "$fake_pc"

for abi in arm64-v8a armeabi-v7a x86_64; do
  case "$abi" in
    arm64-v8a) triple=aarch64-linux-android ;;
    armeabi-v7a) triple=armv7a-linux-androideabi ;;
    x86_64) triple=x86_64-linux-android ;;
  esac
  b="$work/$abi"
  mkdir -p "$b"
  cc="$bin/clang$exe --target=$triple$API"
  # _GNU_SOURCE for configure and the compile alike: bionic declares
  # pipe2() and eventfd() only with it, and configure must see what the
  # compile will.
  ( cd "$b" && CC="$cc" AR="$bin/llvm-ar$exe" RANLIB="$bin/llvm-ranlib$exe" CFLAGS="-O2 -fPIC -ffunction-sections -D_GNU_SOURCE" \
      PKG_CONFIG="$fake_pc" \
      "$mhd/configure" --host="$triple" \
        --disable-shared --enable-static --disable-dependency-tracking \
        --disable-https --without-gnutls --disable-curl \
        --disable-doc --disable-examples --disable-tools >"$b/configure.log" 2>&1 ) ||
    { tail -30 "$b/configure.log" >&2; exit 1; }

  # The objects the library is made of, by the Makefile's own reckoning.
  objs="$("$PYTHON" "$here/tools/makefile-var.py" "$b/src/microhttpd/Makefile" libmicrohttpd_la_OBJECTS | tr -d '\r')"
  [[ -n "$objs" ]] || { echo "no objects for $abi" >&2; exit 1; }

  out="$here/.deps/android/$abi"
  mkdir -p "$out/lib" "$out/include" "$b/obj"
  rm -f "$out/lib/libmicrohttpd.a"
  list=()
  for o in $objs; do
    name="${o#libmicrohttpd_la-}"
    name="${name%.lo}"
    $cc -O2 -fPIC -ffunction-sections -D_GNU_SOURCE -DHAVE_CONFIG_H -I"$b" -I"$mhd/src/include" -I"$mhd/src/microhttpd" \
        -c "$mhd/src/microhttpd/$name.c" -o "$b/obj/$name.o"
    list+=("$b/obj/$name.o")
  done
  "$bin/llvm-ar$exe" rcs "$out/lib/libmicrohttpd.a" "${list[@]}"
  cp "$mhd/src/include/microhttpd.h" "$out/include/"
  echo "$abi: libmicrohttpd $MHD_VER (${#list[@]} sources) in $out"
done
