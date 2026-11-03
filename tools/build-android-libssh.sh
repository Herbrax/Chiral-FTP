#!/usr/bin/env bash
# libssh for Android, for the SFTP server (engine/sftpd.c): built for each
# ABI into .deps/android/<abi>, on the mbedTLS that tools/build-android-deps.sh
# put there first. The computer build uses the system's libssh on OpenSSL;
# here mbedTLS is already in the app for libssh2, so libssh uses it too.
#
#   tools/build-android-libssh.sh   (tools/build-android.sh runs it when needed)
#
# libssh is LGPL-2.1-or-later, which a GPLv3 program may link statically.
# The tarball is checked against the hash below, taken from the release
# whose signature (libssh-0.11.5.tar.xz.asc) verified with the libssh
# release key 88A2 28D8 9B07 C2C7 7D0C 7809 03D5 DF8C FDD3 E8E7.
set -euo pipefail

LIBSSH_VER=0.11.5
LIBSSH_SHA256=6898ba9dd836d618b71dc7a4bb786a502c173cef5cafbf20fe5e0567ba4ea30c
NDK_VER=28.2.13676358
CMAKE_VER=3.31.6
API=26
ABIS=(arm64-v8a armeabi-v7a x86_64)

here="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -z "${ANDROID_HOME:-}" ]]; then
  case "$(uname -s)" in
    Darwin) ANDROID_HOME="$HOME/Library/Android/sdk" ;;
    MINGW*|MSYS*|CYGWIN*) ANDROID_HOME="$(cygpath -u "$LOCALAPPDATA")/Android/Sdk" ;;
    *) ANDROID_HOME="$HOME/Android/Sdk" ;;
  esac
fi
ndk="$ANDROID_HOME/ndk/$NDK_VER"
cmake_dir="$ANDROID_HOME/cmake/$CMAKE_VER/bin"
exe=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) exe=".exe" ;; esac
cmake_bin="$cmake_dir/cmake$exe"

sha256() {
  if command -v shasum >/dev/null; then shasum -a 256 "$@"; else sha256sum "$@"; fi
}

work="$here/.deps/src"
mkdir -p "$work"
tarball="$work/libssh-$LIBSSH_VER.tar.xz"
if [[ ! -f "$tarball" ]]; then
  curl -fsSL -o "$tarball.part" "https://www.libssh.org/files/${LIBSSH_VER%.*}/libssh-$LIBSSH_VER.tar.xz"
  mv "$tarball.part" "$tarball"
fi
echo "$LIBSSH_SHA256  $tarball" | sha256 -c - >/dev/null

src="$here/.deps/build-android/libssh-src"
rm -rf "$src"
mkdir -p "$src"
# On stdin, not by name: tar reads the "C:" of a Windows path as a host.
xz -dc < "$tarball" | tar -xf - -C "$src"

for abi in "${ABIS[@]}"; do
  prefix="$here/.deps/android/$abi"
  [[ -f "$prefix/lib/libmbedcrypto.a" ]] || { echo "no mbedTLS for $abi: run tools/build-android-deps.sh" >&2; exit 1; }
  build="$here/.deps/build-android/$abi/libssh"
  rm -rf "$build"
  # S_IWRITE: the old BSD name for S_IWUSR, which misc.c uses and bionic lacks.
  "$cmake_bin" -S "$src/libssh-$LIBSSH_VER" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$cmake_dir/ninja$exe" \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_C_FLAGS="-DS_IWRITE=S_IWUSR" -DCMAKE_FIND_ROOT_PATH="$prefix" -DMBEDTLS_ROOT_DIR="$prefix" \
    -DWITH_MBEDTLS=ON -DWITH_GCRYPT=OFF -DWITH_ZLIB=OFF -DWITH_GSSAPI=OFF -DWITH_SERVER=ON -DWITH_SFTP=ON \
    -DWITH_EXAMPLES=OFF -DUNIT_TESTING=OFF -DCLIENT_TESTING=OFF -DSERVER_TESTING=OFF \
    -DBUILD_SHARED_LIBS=OFF -DWITH_SYMBOL_VERSIONING=OFF >"$build.log" 2>&1 ||
    { tail -30 "$build.log" >&2; exit 1; }
  "$cmake_bin" --build "$build" >>"$build.log" 2>&1 || { tail -30 "$build.log" >&2; exit 1; }
  "$cmake_bin" --install "$build" >>"$build.log" 2>&1 || { tail -30 "$build.log" >&2; exit 1; }
  echo "$abi: libssh $LIBSSH_VER in $prefix"
done
