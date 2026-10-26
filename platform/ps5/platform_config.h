#pragma once

/* Compile-time traits of the PS5 build. */

/* Files the app writes are open to all: the console only starts homebrew
   whose files are 777, so anything copied in has to be. */
#define CHIRAL_FILE_MODE 0777

/* The UI's own size: VideoOut gives us 1080p and the layout is 1080p. */
#define CHIRAL_RENDER_W 1920
#define CHIRAL_RENDER_H 1080

/* The app accepts connections itself and hands them to libmicrohttpd with
   MHD_add_connection, because the socket has to be set up the way a
   sandboxed title needs. */
#define CHIRAL_WEB_MHD_OWNS_LISTENER 0
#define CHIRAL_WEB_MHD_NEEDS_SOCKETPAIR 0

/* What the web app calls this machine, and the id it reports in
   /api/state. The shared web UI says "the console" rather than naming
   either one, so this is for display and for anything that has to branch. */
#define CHIRAL_PLATFORM_ID   "ps5"
#define CHIRAL_PLATFORM_NAME "PS5"

/* The console's C library heap is capped well below the memory a title may
   use, and the glyph atlas for a CJK font outgrows it, so blocks of 32 KB
   and up are mapped from the system instead (platform_common.cpp). */
#define CHIRAL_BIG_ALLOC_VIA_MMAP 1

/* The bottom bar draws PlayStation buttons: Cross, Circle, Square,
   Triangle, L1/R1/L2/R2, OPTIONS and TOUCHPAD (platform_common.cpp). */
#define CHIRAL_BUTTONS_PLAYSTATION 1
#define CHIRAL_BUTTONS_KEYBOARD 0

/* The text calls this machine "the console" (ui.cpp). */
#define CHIRAL_DEVICE_COMPUTER 0

/* FTP only: no SSH server library here (engine/sftpd.c isn't built). */
#define CHIRAL_HAS_SFTPD 0

/* The console layout, for a TV and a pad (app/ui.cpp). */
#define CHIRAL_UI_DESKTOP 0
/* Not the mobile shell (Android only: app/ui_mobile.inc). */
#define CHIRAL_UI_MOBILE 0
