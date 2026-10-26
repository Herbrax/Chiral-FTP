#!/usr/bin/env bash
# Builds the Android dependencies for each ABI: mbedTLS (crypto) and libssh2
# (SSH/SFTP), the same versions and checksums as tools/build-sftp-deps.sh.
#
#   tools/build-android-deps.sh     install into .deps/android/<abi>
#
# Needs the NDK and the SDK's CMake (which brings Ninja):
#   sdkmanager "ndk;28.2.13676358" "cmake;3.31.6"
# ANDROID_HOME defaults to where Android Studio puts the SDK on each system.
#
# libmicrohttpd (the web app) isn't here yet: it builds with autotools.
#
# Both are static, permissively licensed (Apache-2.0 / BSD-3-Clause) and
# compatible with this project's GPLv3.
set -euo pipefail

MBEDTLS_VER=3.6.7
LIBSSH2_VER=1.11.1
LIBSSH2_SHA256=d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7
NDK_VER=28.2.13676358
CMAKE_VER=3.31.6
API=26
ABIS=(arm64-v8a armeabi-v7a x86_64)

here="$(cd "$(dirname "$0")/.." && pwd)"
work="$here/.deps/src"
mkdir -p "$work"

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
for need in "$ndk/build/cmake/android.toolchain.cmake" "$cmake_bin" "$cmake_dir/ninja$exe"; do
  [[ -e "$need" ]] || { echo "missing $need (see the top of $0)" >&2; exit 1; }
done

# macOS has shasum; Git Bash and MSYS2 on Windows have only sha256sum.
sha256() {
  if command -v shasum >/dev/null; then shasum -a 256 "$@"; else sha256sum "$@"; fi
}

fetch() {  # url file sha256
  if [[ ! -f "$work/$2" ]]; then
    curl -fsSL -o "$work/$2.part" "$1"
    mv "$work/$2.part" "$work/$2"
  fi
  echo "$3  $work/$2" | sha256 -c - >/dev/null
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

src="$here/.deps/build-android/src"
rm -rf "$src"
mkdir -p "$src"
tar -xjf "$work/$mbedtls_tar" -C "$src"
tar -xzf "$work/libssh2-$LIBSSH2_VER.tar.gz" -C "$src"

for abi in "${ABIS[@]}"; do
  prefix="$here/.deps/android/$abi"
  build="$here/.deps/build-android/$abi"
  rm -rf "$build" "$prefix"
  mkdir -p "$build"
  common=(-G Ninja -DCMAKE_MAKE_PROGRAM="$cmake_dir/ninja$exe"
          -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake"
          -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$API"
          -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix"
          -DCMAKE_POSITION_INDEPENDENT_CODE=ON)

  # mbedTLS: the stock config; bionic gives it getrandom() for entropy.
  "$cmake_bin" -S "$src/mbedtls-$MBEDTLS_VER" -B "$build/mbedtls" "${common[@]}" \
    -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DMBEDTLS_FATAL_WARNINGS=OFF >/dev/null
  "$cmake_bin" --build "$build/mbedtls" >/dev/null
  "$cmake_bin" --install "$build/mbedtls" >/dev/null

  # As in build-sftp-deps.sh: mbedTLS's package config doesn't set what
  # libssh2's FindMbedTLS checks, so remove it and let libssh2's finder run.
  rm -rf "$prefix/lib/cmake/MbedTLS"
  "$cmake_bin" -S "$src/libssh2-$LIBSSH2_VER" -B "$build/libssh2" "${common[@]}" \
    -DCRYPTO_BACKEND=mbedTLS -DMBEDTLS_INCLUDE_DIR="$prefix/include" \
    -DMBEDCRYPTO_LIBRARY="$prefix/lib/libmbedcrypto.a" \
    -DMBEDTLS_LIBRARY="$prefix/lib/libmbedtls.a" \
    -DMBEDX509_LIBRARY="$prefix/lib/libmbedx509.a" \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
    -DENABLE_ZLIB_COMPRESSION=OFF >/dev/null
  "$cmake_bin" --build "$build/libssh2" >/dev/null
  "$cmake_bin" --install "$build/libssh2" >/dev/null
  echo "$abi: mbedTLS $MBEDTLS_VER and libssh2 $LIBSSH2_VER in $prefix"
done
