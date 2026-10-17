#!/usr/bin/env bash
# PS5 (native title) builds of mbedTLS and libssh2 for SFTP and libmicrohttpd
# for the web app, compiled with the
# boilerplate's prospero-clang18 wrapper (target x86_64-sie-ps5) against the
# boilerplate's pinned Payload SDK. Installs into .deps/ps5.
set -euo pipefail

MBEDTLS_VER=3.6.7
LIBSSH2_VER=1.11.1
LIBSSH2_SHA256=d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7
MHD_VER=1.0.10
MHD_SHA256=04bfe8ef75db7d629a33de767599765cecadc56274a39822d5d081030d577685

root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="$root/ps5/.deps/native/ps5-payload-sdk"
prefix="$root/.deps/ps5"
src="$root/.deps/src"
build="$root/.deps/build-ps5"
[[ -f "$prefix/lib/libssh2.a" && -f "$prefix/lib/libmbedcrypto.a" && -f "$prefix/lib/libmicrohttpd.a" ]] && exit 0
[[ -x "$sdk/bin/prospero-lld" ]] || { echo "missing $sdk; build ps5/ first (see README)" >&2; exit 1; }
mkdir -p "$src" "$build" "$prefix/bin"

# Compiler wrappers CMake can call.
clang18=$(command -v clang-18 || echo /opt/homebrew/opt/llvm@18/bin/clang-18)
cat > "$prefix/bin/ps5-cc" <<EOF
#!/bin/sh
PS5_PAYLOAD_SDK="$sdk" PS5_CLANG="$clang18" exec sh "$root/ps5/tooling/prospero-clang18" "\$@"
EOF
chmod +x "$prefix/bin/ps5-cc"
cat > "$build/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "$prefix/bin/ps5-cc")
set(CMAKE_AR "$(command -v llvm-ar || echo /opt/homebrew/opt/llvm@18/bin/llvm-ar)")
set(CMAKE_RANLIB "$(command -v llvm-ranlib || echo /opt/homebrew/opt/llvm@18/bin/llvm-ranlib)")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_FLAGS_INIT "-O2 -ffunction-sections -fdata-sections")
set(CMAKE_FIND_ROOT_PATH "$prefix")
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

fetch() {  # url file sha256
  [[ -f "$src/$2" ]] || curl -fsSL -o "$src/$2" "$1"
  echo "$3  $src/$2" | shasum -a 256 -c - >/dev/null
}

build_ssh() {
curl -fsSL -o "$src/mbedtls-sha256sum.txt" \
  "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VER/mbedtls-$MBEDTLS_VER-sha256sum.txt"
mbed_sha=$(awk -v f="mbedtls-$MBEDTLS_VER.tar.bz2" '$2 == f || $2 == "*"f {print $1}' "$src/mbedtls-sha256sum.txt")
fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VER/mbedtls-$MBEDTLS_VER.tar.bz2" \
      "mbedtls-$MBEDTLS_VER.tar.bz2" "$mbed_sha"
fetch "https://github.com/libssh2/libssh2/releases/download/libssh2-$LIBSSH2_VER/libssh2-$LIBSSH2_VER.tar.gz" \
      "libssh2-$LIBSSH2_VER.tar.gz" "$LIBSSH2_SHA256"

rm -rf "$build/mbedtls-$MBEDTLS_VER" "$build/libssh2-$LIBSSH2_VER" "$build/mb" "$build/ssh"
tar -xjf "$src/mbedtls-$MBEDTLS_VER.tar.bz2" -C "$build"
tar -xzf "$src/libssh2-$LIBSSH2_VER.tar.gz" -C "$build"
mbed="$build/mbedtls-$MBEDTLS_VER"
cfg="$mbed/include/mbedtls/mbedtls_config.h"
# Entropy comes from the app (mbedtls_hardware_poll in engine/sftp.c).
python3 "$mbed/scripts/config.py" -f "$cfg" set MBEDTLS_NO_PLATFORM_ENTROPY
python3 "$mbed/scripts/config.py" -f "$cfg" set MBEDTLS_ENTROPY_HARDWARE_ALT
python3 "$mbed/scripts/config.py" -f "$cfg" unset MBEDTLS_TIMING_C
python3 "$mbed/scripts/config.py" -f "$cfg" unset MBEDTLS_NET_C
# No 128-bit division helper (__udivti3) in the console's libraries.
python3 "$mbed/scripts/config.py" -f "$cfg" set MBEDTLS_NO_UDBL_DIVISION

cmake -S "$mbed" -B "$build/mb" -DCMAKE_TOOLCHAIN_FILE="$build/toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DENABLE_PROGRAMS=OFF \
  -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DMBEDTLS_FATAL_WARNINGS=OFF >/dev/null
cmake --build "$build/mb" --parallel 8 >/dev/null
cmake --install "$build/mb" >/dev/null
rm -rf "$prefix/lib/cmake/MbedTLS"

cmake -S "$build/libssh2-$LIBSSH2_VER" -B "$build/ssh" -DCMAKE_TOOLCHAIN_FILE="$build/toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCRYPTO_BACKEND=mbedTLS \
  -DMBEDTLS_INCLUDE_DIR="$prefix/include" -DMBEDCRYPTO_LIBRARY="$prefix/lib/libmbedcrypto.a" \
  -DMBEDTLS_LIBRARY="$prefix/lib/libmbedtls.a" -DMBEDX509_LIBRARY="$prefix/lib/libmbedx509.a" \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
  -DENABLE_ZLIB_COMPRESSION=OFF >/dev/null
cmake --build "$build/ssh" --parallel 8 >/dev/null
cmake --install "$build/ssh" >/dev/null
echo "mbedTLS $MBEDTLS_VER and libssh2 $LIBSSH2_VER built for the PS5 in $prefix"
}

# libmicrohttpd: configure runs its link tests with the payload SDK's compiler
# (which can link test programs); the library itself is compiled with the
# native-title wrapper. Only what api.c uses: no HTTPS, auth, cookies or
# post processor; no sendfile (responses come from memory).
build_mhd() {
fetch "https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-$MHD_VER.tar.gz" "libmicrohttpd-$MHD_VER.tar.gz" "$MHD_SHA256"
rm -rf "$build/libmicrohttpd-$MHD_VER" "$build/mhd"
tar -xzf "$src/libmicrohttpd-$MHD_VER.tar.gz" -C "$build"
mkdir -p "$build/mhd"
(cd "$build/mhd" && PS5_PAYLOAD_SDK="$sdk" CC="$sdk/bin/prospero-clang" CFLAGS="-O2" \
  AR="$(command -v llvm-ar)" RANLIB="$(command -v llvm-ranlib)" \
  "$build/libmicrohttpd-$MHD_VER/configure" --host=x86_64-pc-freebsd --prefix="$prefix" \
  --disable-shared --enable-static --disable-https --without-gnutls --disable-curl --disable-doc \
  --disable-examples --disable-tools --disable-bauth --disable-dauth --disable-cookie \
  --disable-postprocessor --disable-sendfile --disable-thread-names --disable-epoll \
  --enable-itc=pipe >/dev/null)
make -C "$build/mhd/src/microhttpd" -j8 CC="$prefix/bin/ps5-cc" \
  CFLAGS="-O2 -ffunction-sections -fdata-sections" libmicrohttpd.la >/dev/null
mkdir -p "$prefix/lib" "$prefix/include"
cp "$build/mhd/src/microhttpd/.libs/libmicrohttpd.a" "$prefix/lib/"
cp "$build/libmicrohttpd-$MHD_VER/src/include/microhttpd.h" "$prefix/include/"
echo "libmicrohttpd $MHD_VER built for the PS5 in $prefix"
}

[[ -f "$prefix/lib/libssh2.a" && -f "$prefix/lib/libmbedcrypto.a" ]] || build_ssh
[[ -f "$prefix/lib/libmicrohttpd.a" ]] || build_mhd
