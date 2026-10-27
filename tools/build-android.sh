#!/usr/bin/env bash
# Builds chiral-ftp for Android: one APK for phones, tablets, foldables and
# TVs, with the native library for arm64-v8a, armeabi-v7a and x86_64.
#
#   tools/build-android.sh          dist/chiral-ftp-<version>-android.apk (debug-signed)
#
# Needs the Android SDK with the NDK and CMake, JDK 17, Python 3 and curl:
#   sdkmanager "ndk;28.2.13676358" "cmake;3.31.6" "platforms;android-35" "build-tools;35.0.1"
# ANDROID_HOME and JAVA_HOME default to the usual places. Runs on Linux,
# macOS and Windows (Git Bash or MSYS2).
#
# Not built in yet: the SFTP server (libssh).
set -euo pipefail

SDL_VER=2.32.10
SDL_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
NDK_VER=28.2.13676358
BUILD_TOOLS=35.0.1

here="$(cd "$(dirname "$0")/.." && pwd)"
version="${CHIRAL_VERSION:-$(sed -n 's/^VERSION := //p' "$here/Makefile")}"
# Android installs an APK over another only when its versionCode is no lower,
# so it follows VERSION the way the PS5's contentVersion does:
# 1.2 -> 1002000, 1.2.1 -> 1002001.
version_code="$(echo "$version" | awk -F. '{print $1 * 1000000 + $2 * 1000 + ($3 == "" ? 0 : $3)}')"
windows=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) windows=1 ;; esac
native() { if [[ $windows == 1 ]]; then cygpath -m "$1"; else echo "$1"; fi; }

if [[ -z "${ANDROID_HOME:-}" ]]; then
  case "$(uname -s)" in
    Darwin) ANDROID_HOME="$HOME/Library/Android/sdk" ;;
    MINGW*|MSYS*|CYGWIN*) ANDROID_HOME="$(cygpath -u "$LOCALAPPDATA")/Android/Sdk" ;;
    *) ANDROID_HOME="$HOME/Android/Sdk" ;;
  esac
fi
ndk="$ANDROID_HOME/ndk/$NDK_VER"
[[ -d "$ndk" ]] || { echo "missing the NDK at $ndk (see the top of $0)" >&2; exit 1; }
if [[ -z "${JAVA_HOME:-}" || ! -x "$JAVA_HOME/bin/java" && ! -x "$JAVA_HOME/bin/java.exe" ]]; then
  for j in "/c/Program Files/Java/jdk-17" /usr/lib/jvm/java-17-openjdk-amd64 /usr/lib/jvm/java-17-openjdk \
           "$(/usr/libexec/java_home -v 17 2>/dev/null || true)"; do
    if [[ -n "$j" && ( -x "$j/bin/java" || -x "$j/bin/java.exe" ) ]]; then JAVA_HOME="$j"; break; fi
  done
fi
[[ -n "${JAVA_HOME:-}" ]] || { echo "set JAVA_HOME to a JDK 17" >&2; exit 1; }
export JAVA_HOME="$(native "$JAVA_HOME")"
PYTHON="${PYTHON:-$(command -v python3 || command -v python)}"

sha256() {
  if command -v shasum >/dev/null; then shasum -a 256 "$@"; else sha256sum "$@"; fi
}

# 1. The libraries, once.
for abi in arm64-v8a armeabi-v7a x86_64; do
  if [[ ! -f "$here/.deps/android/$abi/lib/libssh2.a" ]]; then
    "$here/tools/build-android-deps.sh"
    break
  fi
done
for abi in arm64-v8a armeabi-v7a x86_64; do
  if [[ ! -f "$here/.deps/android/$abi/lib/libssh.a" ]]; then
    "$here/tools/build-android-libssh.sh"
    break
  fi
done
for abi in arm64-v8a armeabi-v7a x86_64; do
  if [[ ! -f "$here/.deps/android/$abi/lib/libmicrohttpd.a" ]]; then
    "$here/tools/build-android-mhd.sh"
    break
  fi
done

# 2. SDL, its source pinned by checksum.
work="$here/.deps/src"
mkdir -p "$work"
sdl_tar="$work/SDL2-$SDL_VER.tar.gz"
if [[ ! -f "$sdl_tar" ]]; then
  curl -fsSL -o "$sdl_tar.part" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz"
  mv "$sdl_tar.part" "$sdl_tar"
fi
echo "$SDL_SHA256  $sdl_tar" | sha256 -c - >/dev/null
sdl="$work/SDL2-$SDL_VER"
[[ -d "$sdl" ]] || tar -xzf "$sdl_tar" -C "$work"

# 3. The generated sources, as the Makefile makes them.
mkdir -p "$here/gen"
"$PYTHON" "$here/tools/bin2c.py" font_regular_ttf "$here/assets/fonts/NotoSans-Regular.ttf" > "$here/gen/font_regular.c"
"$PYTHON" "$here/tools/bin2c.py" font_bold_ttf "$here/assets/fonts/NotoSans-SemiBold.ttf" > "$here/gen/font_bold.c"
"$PYTHON" "$here/tools/bin2c.py" font_mono_ttf "$here/assets/fonts/IBMPlexMono-Medium.ttf" > "$here/gen/font_mono.c"
for page in index.html app.css app.js files.html files.css files.js; do
  "$PYTHON" "$here/tools/gen-asset-module.py" --path "$page" "$here/web/assets/$page" > "$here/gen/$page.c"
done
"$PYTHON" "$here/tools/gen-asset-module.py" --path mark.svg "$here/assets/brand/chiral-ftp-mark.svg" > "$here/gen/mark.svg.c"
"$PYTHON" "$here/tools/gen-asset-module.py" --path mark-light.svg "$here/assets/brand/chiral-ftp-mark-light.svg" > "$here/gen/mark-light.svg.c"
"$PYTHON" "$here/tools/gen-asset-module.py" --path fonts/IBMPlexMono-Medium.ttf "$here/assets/fonts/IBMPlexMono-Medium.ttf" > "$here/gen/plexmono.ttf.c"
( cd "$here" && "$PYTHON" tools/gen-i18n.py i18n/translations.json gen/i18n_table.c \
    app/main.cpp app/ui.cpp app/json.cpp app/platform_common.cpp app/raster.cpp app/i18n.cpp \
    platform/*/*.cpp platform/sdl/macos.mm engine/ftpd.c engine/sftpd.c engine/fsjobs.c \
    app/ui_desktop.inc app/ui_mobile.inc )

# 4. The project: SDL's Java side and Gradle wrapper, then ours over it.
proj="$here/.build/android"
mkdir -p "$proj/app/src/main/java/org/libsdl" "$proj/app/jni/SDL" "$proj/gradle/wrapper" \
         "$proj/app/src/main/res/mipmap-xxxhdpi"
cp -r "$sdl/android-project/app/src/main/java/org/libsdl/app" "$proj/app/src/main/java/org/libsdl/"
cp "$sdl/android-project/gradlew" "$sdl/android-project/gradlew.bat" "$proj/"
cp "$sdl/android-project/gradle/wrapper/gradle-wrapper.jar" "$proj/gradle/wrapper/"
if [[ ! -f "$proj/app/jni/SDL/Android.mk" ]]; then
  cp -r "$sdl/src" "$sdl/include" "$sdl/Android.mk" "$proj/app/jni/SDL/"
fi
cp -r "$here/platform/android/project/." "$proj/"
cp "$here/assets/brand/icon-256.png" "$proj/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png"
printf 'sdk.dir=%s\n' "$(native "$ANDROID_HOME" | sed 's/:/\\:/')" > "$proj/local.properties"

# 5. The native library, for each ABI.
ndk_build="$ndk/ndk-build"
[[ $windows == 1 ]] && ndk_build="$ndk/ndk-build.cmd"
( cd "$proj/app" && "$ndk_build" NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk \
    NDK_APPLICATION_MK=jni/Application.mk NDK_OUT="$(native "$proj/obj")" NDK_LIBS_OUT="$(native "$proj/app/libs")" \
    CHIRAL_ROOT="$(native "$here")" CHIRAL_VERSION="$version" -j8 )

# 6. The APK.
( cd "$proj" && sh ./gradlew assembleDebug --console=plain -q -PchiralVersion="$version" -PchiralVersionCode="$version_code" )
mkdir -p "$here/dist"
apk="$here/dist/chiral-ftp-$version-android.apk"
cp "$proj/app/build/outputs/apk/debug/app-debug.apk" "$apk"

# 7. Linked what we think, and aligned for 16 KB pages (agent notes §5).
nm="$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/llvm-nm* | head -1)"
symbols="$("$nm" -D --defined-only "$proj/obj/local/arm64-v8a/libmain.so" | tr -d '\r')"
for s in main ftpd_start discovery_start libssh2_session_init_ex mbedtls_sha256_starts MHD_start_daemon; do
  grep -q " $s\$" <<< "$symbols" || { echo "libmain.so lacks $s" >&2; exit 1; }
done
zipalign="$(ls "$ANDROID_HOME/build-tools/$BUILD_TOOLS"/zipalign* | head -1)"
"$zipalign" -c -P 16 4 "$apk" >/dev/null || { echo "$apk is not 16 KB aligned" >&2; exit 1; }
echo "built $apk"
