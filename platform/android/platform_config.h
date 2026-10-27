#pragma once

/* Compile-time traits of the Android build: phones, tablets, foldables and
   Android TV, from one APK. engine/system.h has the parts that are
   functions; these are the ones that have to be constants. */

/* Mode for files the app creates. Shared storage (FUSE) and exFAT cards
   ignore permission bits, so this only matters for uploads to a real
   server, where 0644 is what a phone's files should be. */
#define CHIRAL_FILE_MODE 0644

/* The console layout's frame, which a TV draws (a phone or tablet draws at
   its window's own pixels instead: platform_common.cpp, windowed). */
#define CHIRAL_RENDER_W 1920
#define CHIRAL_RENDER_H 1080

/* Bionic has socketpair, eventfd and epoll: libmicrohttpd binds and accepts
   for itself, as on the Switch, with nothing to stand in for. */
#define CHIRAL_WEB_MHD_OWNS_LISTENER 1
#define CHIRAL_WEB_MHD_NEEDS_SOCKETPAIR 0

#define CHIRAL_PLATFORM_ID   "android"
#define CHIRAL_PLATFORM_NAME "Android"

/* The C library's malloc draws on the whole heap. */
#define CHIRAL_BIG_ALLOC_VIA_MMAP 0

/* A TV's bottom bar draws the remote's buttons -- OK, Back, the d-pad --
   or, once a gamepad is used, that pad's A, B, X and Y (platform_common.cpp,
   draw::button). A phone or tablet has no bottom bar of buttons at all. */
#define CHIRAL_BUTTONS_PLAYSTATION 0
#define CHIRAL_BUTTONS_KEYBOARD 0
#define CHIRAL_BUTTONS_REMOTE 1

/* The text calls this machine "the console" for now (item 10 brings "this
   phone" and "the TV"). */
#define CHIRAL_DEVICE_COMPUTER 0

/* The SFTP server too: libssh on mbedTLS (tools/build-android-libssh.sh). */
#define CHIRAL_HAS_SFTPD 1

/* A phone or tablet gets the computer interface's controls and pages,
   arranged by the mobile shell (app/ui_mobile.inc) to fit whatever window
   it has: one pane, two side by side, two stacked, a cover screen. A TV,
   whose window is not handed to the layout (backend window_size), gets the
   console layout instead -- decided at run time, from one APK. */
#define CHIRAL_UI_DESKTOP 1
#define CHIRAL_UI_MOBILE 1
