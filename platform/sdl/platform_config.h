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

/* The bottom bar draws the Switch-style A/B/X/Y glyphs (platform_common.cpp). */
#define CHIRAL_BUTTONS_PLAYSTATION 0
