#!/usr/bin/env bash
# Builds chiral-ftp as a native PS5 title (ShadowMountPlus):
#   compile with ps5-native-app-boilerplate's prospero-clang18 wrapper,
#   link with the full libc++ the way ProsperoEden links its app,
#   convert + sign with the boilerplate's ps5-native-tool,
#   package dist/PPSA96024 with the boilerplate's libc.prx runtime.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
bp="$root/ps5"
sdk="$bp/.deps/native/ps5-payload-sdk"
tool="$bp/build/host/ps5-native-tool"
version="${VERSION:-1.0}"
# VARIANT=app (chiral-ftp), test1 (Hello World with this link recipe) or
# test2 (platform layer + ImGui, no FTP engine): a ladder for finding what a
# console refuses to start.
variant="${VARIANT:-app}"
case "$variant" in
  app)   title=PPSA96024; app_name="Chiral-FTP"; zipname="chiral-ftp-$version.zip" ;;  # the dashboard name
  test1) title=PPSA97014; app_name="Chiral Test 1b"; zipname="Chiral-Test1b.zip" ;;
  test2) title=PPSA97012; app_name="Chiral Test 2"; zipname="Chiral-Test2.zip" ;;
  *) echo "unknown VARIANT $variant" >&2; exit 2 ;;
esac
obj="$root/.build/ps5-$variant"
out="$root/dist/$title"

if [[ "$(uname)" == Darwin ]]; then
  export PATH="$bp/.hostbin:/opt/homebrew/bin:/opt/homebrew/opt/coreutils/libexec/gnubin:/opt/homebrew/opt/gnu-sed/libexec/gnubin:/opt/homebrew/opt/llvm@18/bin:$PATH"
fi
clang18=$(command -v clang-18)
[[ -x "$tool" && -x "$sdk/bin/prospero-lld" ]] || { echo "build ps5/ (the boilerplate) first: cd ps5 && make" >&2; exit 1; }
"$root/tools/build-ps5-deps.sh"
deps="$root/.deps/ps5"

cc() { PS5_PAYLOAD_SDK="$sdk" PS5_CLANG="$clang18" sh "$bp/tooling/prospero-clang18" "$@"; }

common=(-O2 -Wall -ffunction-sections -fdata-sections -DVERSION_TAG="\"$version\"")
cflags=(-std=gnu11 "${common[@]}" -Werror -I"$root/engine" -I"$root/web" -I"$deps/include")
# No ImGui shell/file helpers: the default "open in shell" spawns a process
# (execvp/waitpid), which an app can't import.
cxxflags=(-std=c++20 "${common[@]}" -fno-exceptions -fno-rtti -I"$root/app" -I"$root/engine" -I"$root/web"
          -I"$deps/include"
          -I"$root/vendor/imgui" -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -DIMGUI_DISABLE_FILE_FUNCTIONS
          -DIMGUI_USER_CONFIG='"imgui_config.h"')

mkdir -p "$obj"
# Objects only rebuild when their sources change, so a changed flag (such as
# VERSION_TAG) must clear them all.
stamp="$(printf '%s\n' "${cflags[*]}" "${cxxflags[*]}" | shasum -a 256 | cut -c1-16)"
if [[ "$(cat "$obj/flags.stamp" 2>/dev/null)" != "$stamp" ]]; then
  find "$obj" -maxdepth 1 -name '*.o' -delete
  echo "$stamp" > "$obj/flags.stamp"
fi
objects=()
compile() {  # source flags...
  local source="$1"; shift
  local o="$obj/${source//\//_}.o"
  if [[ ! -f "$o" || "$root/$source" -nt "$o" || -n "$(find "$root/engine" "$root/app" -name '*.h' -newer "$o" 2>/dev/null | head -1)" ]]; then
    echo "  CC $source"
    cc "$@" -c "$root/$source" -o "$o"
  fi
  objects+=("$o")
}

echo "==> Compiling ($app_name)"
imgui_srcs=(vendor/imgui/imgui.cpp vendor/imgui/imgui_draw.cpp vendor/imgui/imgui_tables.cpp
            vendor/imgui/imgui_widgets.cpp)
case "$variant" in
  app)
    c_srcs=(engine/ftp.c engine/listparse.c engine/session.c engine/queue.c engine/localfs.c
            engine/settings.c engine/log.c engine/strbuf.c engine/form.c engine/notify.c
            engine/sftp.c engine/remote_ops.c engine/ps5_net.c engine/ftpd.c engine/fsjobs.c app/ps5_runtime.c
            web/websrv.c web/api.c web/asset.c gen/font_regular.c gen/font_bold.c gen/font_mono.c gen/index.html.c
            gen/app.css.c gen/app.js.c gen/icon0.png.c gen/files.html.c gen/files.css.c
            gen/files.js.c gen/mark.svg.c gen/mark-light.svg.c gen/plexmono.ttf.c gen/i18n.c)
    cxx_srcs=(app/main.cpp app/ui.cpp app/json.cpp app/platform_common.cpp app/raster.cpp
              app/backend_ps5.cpp app/elevation/elevation.cpp app/i18n.cpp "${imgui_srcs[@]}")
    extra_libs=("$deps/lib/libssh2.a" "$deps/lib/libmbedcrypto.a" "$deps/lib/libmicrohttpd.a")
    ;;
  test1)
    c_srcs=(app/ps5_runtime.c)
    cxx_srcs=(tests/ps5_hello_main.cpp tests/ps5_hello_renderer.cpp tests/ps5_hello_shim.cpp)
    cxxflags+=(-I"$root/tests")
    extra_libs=()
    ;;
  test2)
    c_srcs=(app/ps5_runtime.c gen/font_regular.c gen/font_bold.c gen/font_mono.c)
    cxx_srcs=(tests/ps5_platform_test.cpp app/platform_common.cpp app/raster.cpp app/backend_ps5.cpp
              "${imgui_srcs[@]}")
    extra_libs=()
    ;;
esac
for s in "${c_srcs[@]}"; do compile "$s" "${cflags[@]}"; done
for s in "${cxx_srcs[@]}"; do compile "$s" "${cxxflags[@]}"; done
# The boilerplate's program entry (_start, init/fini arrays), as ProsperoEden uses it.
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$bp/tooling/native/app_crt.cpp" -o "$obj/app_crt.o"

# Link-time facade for libSceCommonDialog (no SDK stub), as in ProsperoEden.
stub="$obj/stubs/libSceCommonDialog.so"
if [[ ! -f "$stub" ]]; then
  mkdir -p "$obj/stubs"
  echo 'int sceCommonDialogInitialize(void) { return -1; }' > "$obj/stubs/common_dialog.c"
  PS5_PAYLOAD_SDK="$sdk" "$sdk/bin/prospero-clang" -fPIC -c "$obj/stubs/common_dialog.c" -o "$obj/stubs/common_dialog.o"
  "$sdk/bin/prospero-lld" --shared -soname libSceCommonDialog.sprx -o "$stub" "$obj/stubs/common_dialog.o"
fi

cat > "$obj/unwind.ld" <<'EOF'
PROVIDE(__eh_frame_start = ADDR(.eh_frame));
PROVIDE(__eh_frame_end = ADDR(.eh_frame) + SIZEOF(.eh_frame));
PROVIDE(__eh_frame_hdr_start = ADDR(.eh_frame_hdr));
PROVIDE(__eh_frame_hdr_end = ADDR(.eh_frame_hdr) + SIZEOF(.eh_frame_hdr));
EOF

echo "==> Linking"
lib="$sdk/target/lib"
"$sdk/bin/prospero-lld" \
  --defsym=__cxa_thread_atexit_impl=0 -L "$lib" \
  --defsym=__dlopen=0 --defsym=__dlsym=0 --defsym=__dladdr=0 --defsym=__dlclose=0 --defsym=__dlerror=0 \
  -T "$bp/tooling/native/ps5-pie.ld" -T "$obj/unwind.ld" \
  --eh-frame-hdr --gc-sections --version-script "$bp/tooling/native/app-symbols.map" -e _start \
  --wrap=fcntl --wrap=socket --wrap=accept --wrap=lstat \
  --no-dynamic-linker -Map="$obj/link.map" \
  -o "$obj/llvm-pie.elf" "$obj/app_crt.o" "${objects[@]}" \
  "${extra_libs[@]}" \
  --start-group "$lib/libc++.a" "$lib/libc++abi.a" "$lib/libunwind.a" --end-group \
  --as-needed "$lib/libSceLibcInternal.so" "$lib/libkernel.so" "$lib/libc.a" \
  "$lib/libSceNet.so" "$lib/libSceVideoOut.so" "$lib/libScePad.so" "$lib/libSceUserService.so" \
  "$lib/libSceSystemService.so" "$lib/libSceImeDialog.so" "$lib/libSceSysmodule.so" "$stub"

# A title may only make system calls through libkernel: anything taken from
# the SDK's raw syscall stubs (libc.a syscalls.o) is killed at run time with
# SYSTEM_ILLEGAL_FUNCTION_CALL. Refuse to package such a build.
if raw=$(grep -oE "libc\.a\(syscalls\.o\):\(\.text\.[A-Za-z0-9_]+\)" "$obj/link.map" | sed 's/.*\.text\.//;s/)//' | sort -u) && [[ -n "$raw" ]]; then
  echo "error: raw system calls linked from libc.a: $raw" >&2
  echo "       use the libkernel function, or implement it in app/ps5_runtime.c" >&2
  exit 1
fi

echo "==> Converting and signing"
"$tool" link --in "$obj/llvm-pie.elf" --out "$obj/eboot.elf" --stub-dir "$lib" --stub "$stub" \
  --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
rm -rf "$out"
mkdir -p "$out/sce_sys" "$out/sce_module"
"$tool" self --sign --in "$obj/eboot.elf" --out "$out/eboot.bin" --magic 0x1D3D154F
(cd "$bp/runtime" && sha256sum --check --strict libc.prx.sha256 >/dev/null)
cp "$bp/runtime/libc.prx" "$out/sce_module/libc.prx"
python3 - "$root/sce_sys/param.json" "$out/sce_sys/param.json" "$title" "$app_name" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))
title, name = sys.argv[3], sys.argv[4]
p["titleId"] = title
p["conceptId"] = title[4:]
p["contentId"] = "UP9000-%s_00-%s" % (title, (name.upper().replace(" ", "").replace("-", "") + "0" * 16)[:16])
p["localizedParameters"] = {"defaultLanguage": "en-US", "en-US": {"titleName": name}}
json.dump(p, open(sys.argv[2], "w"), indent=2)
PY
cp "$root/sce_sys/icon0.png" "$out/sce_sys/"
# The picture behind the title on the home screen, and its launch screen
# (tools/render-background.sh makes them).
[[ "$variant" != app ]] || cp "$root/sce_sys/pic0.dds" "$root/sce_sys/pic1.dds" "$out/sce_sys/"
[[ "$variant" != test1 ]] || cp -R "$root/tests/assets" "$out/assets"
mkdir -p "$out/licenses"
cp "$root/LICENSE" "$out/licenses/chiral-ftp-GPL-3.0.txt"
cp "$root/assets/fonts/OFL.txt" "$out/licenses/NotoSans-OFL.txt"
cp "$root/assets/fonts/IBMPlexMono-OFL.txt" "$out/licenses/IBMPlexMono-OFL.txt"
cp "$root/vendor/imgui/LICENSE.txt" "$out/licenses/DearImGui-MIT.txt"
if [[ "$variant" == app ]]; then
  # Full storage access: upstream Lapy's one-shot helper, built for this exact
  # title by the boilerplate's pinned builder (it verifies the manifest).
  python3 "$bp/tools/build-lapy-helper.py" "$title" "$obj/lapy"
  cp "$obj/lapy/lapy.elf" "$obj/lapy/lapy-manifest.json" "$out/"
  cp "$obj/lapy/Lapy-MIT.txt" "$out/licenses/"
  # Bundled libraries (sources fetched by tools/build-ps5-deps.sh).
  cp "$root/.deps/build-ps5/libmicrohttpd-"*/COPYING "$out/licenses/libmicrohttpd-LGPL-2.1.txt"
  cp "$root/.deps/build-ps5/libssh2-"*/COPYING "$out/licenses/libssh2-BSD.txt"
  cp "$root/.deps/build-ps5/mbedtls-"*/LICENSE "$out/licenses/mbedTLS-Apache-2.0.txt"
fi
"$tool" self --inspect --file "$out/eboot.bin" | tail -3

echo "==> Packaging"
# The console refuses to start a title whose folder or files aren't 777.
chmod -R 777 "$out"
zip="$zipname"
(cd "$root/dist" && rm -f "$zip" && python3 -m zipfile -c "$zip" "$title")
# The console only starts an app whose files are open to all (boilerplate note).
python3 "$bp/tools/zip-open-modes.py" "$root/dist/$zip"
echo "Built dist/$title and dist/$zip"
