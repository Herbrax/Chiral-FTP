#!/usr/bin/env bash
# Builds the SFTP dependencies: mbedTLS (crypto) and libssh2 (SSH/SFTP).
#
#   tools/build-sftp-deps.sh host   install into .deps/host, for the computer
#                                   build (make -f Makefile.host)
#
# The Switch build doesn't use this: devkitPro has switch-mbedtls and
# switch-libssh2 as packages (dkp-pacman -S switch-mbedtls switch-libssh2).
#
# Both are static, permissively licensed (Apache-2.0 / BSD-3-Clause) and
# compatible with this project's GPLv3.
set -euo pipefail

MBEDTLS_VER=3.6.7
LIBSSH2_VER=1.11.1
LIBSSH2_SHA256=d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7

target="${1:-}"
here="$(cd "$(dirname "$0")/.." && pwd)"
work="$here/.deps/src"
mkdir -p "$work"

case "$target" in
  host)
    prefix="$here/.deps/host"
    cmake_bin=cmake
    ;;
  *)
    echo "usage: $0 host" >&2
    exit 1
    ;;
esac

fetch() {  # url file sha256
  if [[ ! -f "$work/$2" ]]; then
    curl -fsSL -o "$work/$2.part" "$1"
    mv "$work/$2.part" "$work/$2"
  fi
  echo "$3  $work/$2" | sha256 -c - >/dev/null
}

# macOS has shasum; MSYS2 on Windows has only coreutils' sha256sum.
sha256() {
  if command -v shasum >/dev/null; then shasum -a 256 "$@"; else sha256sum "$@"; fi
}

mbedtls_tar="mbedtls-$MBEDTLS_VER.tar.bz2"
if [[ ! -f "$work/$mbedtls_tar" ]]; then
  curl -fsSL -o "$work/mbedtls-sha256sum.txt" \
    "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VER/mbedtls-$MBEDTLS_VER-sha256sum.txt"
fi
mbedtls_sha="$(awk -v f="$mbedtls_tar" '$2 == f || $2 == "*"f {print $1}' "$work/mbedtls-sha256sum.txt")"
fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VER/$mbedtls_tar" \
      "$mbedtls_tar" "$mbedtls_sha"
fetch "https://github.com/libssh2/libssh2/releases/download/libssh2-$LIBSSH2_VER/libssh2-$LIBSSH2_VER.tar.gz" \
      "libssh2-$LIBSSH2_VER.tar.gz" "$LIBSSH2_SHA256"

build="$here/.deps/build-$target"
rm -rf "$build"
mkdir -p "$build"
tar -xjf "$work/$mbedtls_tar" -C "$build"
tar -xzf "$work/libssh2-$LIBSSH2_VER.tar.gz" -C "$build"

# mbedTLS: only the crypto pieces libssh2 needs. The computer build takes
# entropy from the OS, so the stock config is right as it stands.
mbed="$build/mbedtls-$MBEDTLS_VER"
"$cmake_bin" -S "$mbed" -B "$build/mbedtls-build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
  -DMBEDTLS_FATAL_WARNINGS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON >/dev/null
"$cmake_bin" --build "$build/mbedtls-build" --parallel 8 >/dev/null
"$cmake_bin" --install "$build/mbedtls-build" >/dev/null

# mbedTLS's CMake package config doesn't set what libssh2's FindMbedTLS
# checks. Remove it so libssh2's own finder runs instead.
rm -rf "$prefix/lib/cmake/MbedTLS"
"$cmake_bin" -S "$build/libssh2-$LIBSSH2_VER" -B "$build/libssh2-build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCRYPTO_BACKEND=mbedTLS -DMBEDTLS_INCLUDE_DIR="$prefix/include" \
  -DMBEDCRYPTO_LIBRARY="$prefix/lib/libmbedcrypto.a" \
  -DMBEDTLS_LIBRARY="$prefix/lib/libmbedtls.a" \
  -DMBEDX509_LIBRARY="$prefix/lib/libmbedx509.a" \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
  -DENABLE_ZLIB_COMPRESSION=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON >/dev/null
"$cmake_bin" --build "$build/libssh2-build" --parallel 8 >/dev/null
"$cmake_bin" --install "$build/libssh2-build" >/dev/null

echo "mbedTLS $MBEDTLS_VER and libssh2 $LIBSSH2_VER installed in $prefix"
