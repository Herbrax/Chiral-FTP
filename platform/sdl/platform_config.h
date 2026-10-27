#pragma once

/* Compile-time traits of the desktop build (Linux, macOS, Windows under
   MSYS2) -- the one most of the UI is developed against. */

#define CHIRAL_FILE_MODE 0644

#define CHIRAL_RENDER_W 1920
#define CHIRAL_RENDER_H 1080

/* The system libmicrohttpd, used the ordinary way. */
#define CHIRAL_WEB_MHD_OWNS_LISTENER 0
#define CHIRAL_WEB_MHD_NEEDS_SOCKETPAIR 0

/* What the web app calls this machine, and the id it reports in
   /api/state. The shared web UI says "the console" rather than naming
   either one, so this is for display and for anything that has to branch. */
#define CHIRAL_PLATFORM_ID   "host"
#define CHIRAL_PLATFORM_NAME "this computer"

/* An ordinary hosted malloc; nothing to work around. */
#define CHIRAL_BIG_ALLOC_VIA_MMAP 0

/* The bottom bar draws the keys backend.cpp maps -- Enter, Esc, Space, T,
   Q/E, PgUp/PgDn, M, G and the arrows -- not a console's buttons
   (platform_common.cpp). */
#define CHIRAL_BUTTONS_PLAYSTATION 0
#define CHIRAL_BUTTONS_KEYBOARD 1
#define CHIRAL_BUTTONS_REMOTE 0  /* a TV remote: Android only */

/* The text calls this machine "this computer", not "the console" (ui.cpp). */
#define CHIRAL_DEVICE_COMPUTER 1

/* The server screen offers SFTP as well as FTP (engine/sftpd.c, on libssh,
   which only this build links). */
#define CHIRAL_HAS_SFTPD 1

/* The computer interface -- a sidebar of pages with switches, fields and
   buttons, mouse first (app/ui_desktop.inc) -- on every computer: macOS,
   Windows and Linux alike. (The console layout is the consoles'; Android
   will have a third, its own.) */
#define CHIRAL_UI_DESKTOP 1
/* Not the mobile shell (Android only: app/ui_mobile.inc). */
#define CHIRAL_UI_MOBILE 0
