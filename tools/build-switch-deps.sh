#!/usr/bin/env bash
# libmicrohttpd for the Switch, for the web app.
#
# Everything else chiral-ftp links comes from devkitPro pacman:
#
#   dkp-pacman -S switch-dev switch-libssh2 switch-mbedtls switch-zlib
#
# libmicrohttpd has no devkitPro package, so it is cross-compiled here with
# devkitA64 and installed into .deps/switch. Two Horizon-specific wrinkles:
#
#  * ITC (inter-thread communication, how MHD wakes its own polling thread)
#    normally uses a pipe or eventfd, and Horizon has neither. MHD's third
#    option is socketpair -- which libnx declares but answers with ENOSYS --
#    so the build is pointed at platform/switch/net.c's loopback version with
#    -Dsocketpair=chiral_socketpair.
#  * Dependency tracking is off: a pinned one-shot build is never rebuilt
#    incrementally, and automake's depfile bootstrap does not survive the
#    cross setup.
#  * No epoll and no sendfile: Horizon has neither. Everything else MHD
#    offers stays switched on. An earlier build also turned off basic and
#    digest auth, cookies, the post processor and thread names purely to
#    save size, and configure answered with "this will be a custom build
#    with missing symbols, do NOT use this build in a distribution" -- not
#    a trade worth making for a daemon that has to survive startup.
set -euo pipefail

MHD_VER=1.0.10
MHD_SHA256=04bfe8ef75db7d629a33de767599765cecadc56274a39822d5d081030d577685

root="$(cd "$(dirname "$0")/.." && pwd)"
prefix="$root/.deps/switch"
src="$root/.deps/src"
build="$root/.deps/build-switch"

[[ -f "$prefix/lib/libmicrohttpd.a" ]] && exit 0
: "${DEVKITPRO:?set DEVKITPRO, e.g. export DEVKITPRO=/opt/devkitpro}"
[[ -f "$DEVKITPRO/switchvars.sh" ]] || { echo "no $DEVKITPRO/switchvars.sh; install switch-dev" >&2; exit 1; }

mkdir -p "$src" "$build" "$prefix/lib" "$prefix/include"

if [[ ! -f "$src/libmicrohttpd-$MHD_VER.tar.gz" ]]; then
  curl -fsSL -o "$src/libmicrohttpd-$MHD_VER.tar.gz" \
    "https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-$MHD_VER.tar.gz"
fi
echo "$MHD_SHA256  $src/libmicrohttpd-$MHD_VER.tar.gz" | sha256sum -c - >/dev/null

rm -rf "$build/libmicrohttpd-$MHD_VER" "$build/mhd"
# Fed on stdin, not by name: an absolute Windows path makes tar think the
# "C:" is a remote host. Portable across GNU and BSD tar.
tar -xzf - -C "$build" < "$src/libmicrohttpd-$MHD_VER.tar.gz"
mkdir -p "$build/mhd"

# switchvars.sh sets CC, CXX, AR, RANLIB, CFLAGS, LDFLAGS, LIBS and
# PORTLIBS_PREFIX for the aarch64-none-elf cross toolchain. It sources
# devkita64.sh, which hardcodes DEVKITPRO=/opt/devkitpro, so put ours back
# afterwards for anyone whose devkitPro lives elsewhere.
dkp_prefix="$DEVKITPRO"
# shellcheck disable=SC1091
source "$DEVKITPRO/switchvars.sh"
if [ "$dkp_prefix" != "/opt/devkitpro" ]; then
  # devkita64.sh (sourced by switchvars.sh) hardcodes /opt/devkitpro, and the
  # flags it exports have that baked into every -I and -L. Point them back at
  # the prefix we were actually given.
  for v in CPPFLAGS CFLAGS CXXFLAGS LDFLAGS PORTLIBS_PREFIX PORTLIBS_ROOT; do
    eval "export $v=\"\${$v//\/opt\/devkitpro/$dkp_prefix}\""
  done
  export DEVKITPRO="$dkp_prefix"
  export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"
fi
# libnx's specs file supplies the linker script; without it configure's
# "can the compiler create executables" test cannot link against -lnx.
# Our own prefix goes on both paths for the compat library below.
export LDFLAGS="-specs=$DEVKITPRO/libnx/switch.specs -L$prefix/lib $LDFLAGS"
export CPPFLAGS="-I$prefix/include $CPPFLAGS"
MAKE="${MAKE:-make}"

# The socketpair shim has to be a real library before libmicrohttpd's
# configure can believe in it: configure link-tests socketpair(3), and with
# -Dsocketpair=chiral_socketpair that test needs the symbol to come from
# somewhere. platform/switch/net.c compiled on its own is that somewhere.
#
# The app itself links platform/switch/net.c directly, so this archive is only ever
# used to satisfy libmicrohttpd.
compat="$prefix/lib/libchiral-nx-compat.a"
echo "building the socketpair shim for libmicrohttpd's link tests"
"$CC" $CFLAGS $CPPFLAGS -I"$root/engine" -I"$root/platform/switch" -c "$root/platform/switch/net.c" -o "$build/nx_net.o"
rm -f "$compat"
"${AR:-aarch64-none-elf-gcc-ar}" rcs "$compat" "$build/nx_net.o"
cp "$root/platform/switch/net.h" "$prefix/include/"
export LIBS="-lchiral-nx-compat $LIBS"

(
  cd "$build/mhd"
  CFLAGS="$CFLAGS -O2 -ffunction-sections -Dsocketpair=chiral_socketpair" \
  CPPFLAGS="${CPPFLAGS:-}" \
  LDFLAGS="$LDFLAGS" \
  LIBS="$LIBS" \
  "$build/libmicrohttpd-$MHD_VER/configure" \
    --host=aarch64-none-elf \
    --prefix="$prefix" \
    --disable-shared --enable-static \
    --disable-dependency-tracking \
    --disable-https --without-gnutls --disable-curl \
    --disable-doc --disable-examples --disable-tools \
    --disable-epoll --disable-sendfile \
    --enable-itc=socketpair
)

"$MAKE" -C "$build/mhd/src/microhttpd" -j"$(nproc 2>/dev/null || echo 4)" libmicrohttpd.la
cp "$build/mhd/src/microhttpd/.libs/libmicrohttpd.a" "$prefix/lib/"
cp "$build/libmicrohttpd-$MHD_VER/src/include/microhttpd.h" "$prefix/include/"
echo "libmicrohttpd $MHD_VER built for the Switch in $prefix"
