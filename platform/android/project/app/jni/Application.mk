# The three ABIs (docs/ANDROID-ROADMAP.md, D5): 32-bit ARM for the many TVs
# with a 32-bit userland, x86_64 for the emulator and Chromebooks.
APP_ABI := arm64-v8a armeabi-v7a x86_64
APP_PLATFORM := android-26
APP_STL := c++_static
# Windows: link through response files, or the command line is too long.
APP_SHORT_COMMANDS := true
