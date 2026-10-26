# Android: phones, tablets, foldables and Android TV, one APK. Needs the
# Android SDK with the NDK, CMake and JDK 17 (see tools/build-android.sh);
# the build is that script's, since an APK is not one link line: it
# compiles the native library for each ABI with ndk-build and packages it
# with Gradle. This file is here so `make PLATFORM=android` reaches it.
.DEFAULT_GOAL := all

all:
	./tools/build-android.sh

.PHONY: all
