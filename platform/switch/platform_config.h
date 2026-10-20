#pragma once

/* Compile-time traits of the Switch build. engine/system.h has the parts
   that are functions; these are the ones that have to be constants. */

/* Mode for files the app creates. The SD card is FAT32 or exFAT and has no
   permission bits, so this only really matters for files uploaded to a real
   server (engine/queue.c sets it there too). */
#define CHIRAL_FILE_MODE 0777

/* What the software renderer draws into, in pixels. The UI is laid out in a
   1920x1080 space whatever this is; platform_common scales the draw data if
   they differ. Full size here: the rasteriser is fast enough, and matching
   the layout means text is drawn rather than resampled. */
#define CHIRAL_RENDER_W 1920
#define CHIRAL_RENDER_H 1080

/* libmicrohttpd binds and accepts for itself. The alternative -- an accept
   loop of our own feeding MHD_add_connection, which is what the PS5 needs
   -- is a corner of libmicrohttpd that almost nothing exercises, and on
   Horizon MHD_start_daemon died inside it. */
#define CHIRAL_WEB_MHD_OWNS_LISTENER 1

/* Horizon has no exec, and MHD asks for its inter-thread channel with
   SOCK_CLOEXEC|SOCK_NONBLOCK set; platform/switch/net.c answers that. */
#define CHIRAL_WEB_MHD_NEEDS_SOCKETPAIR 1

/* What the web app calls this machine, and the id it reports in
   /api/state. The shared web UI says "the console" rather than naming
   either one, so this is for display and for anything that has to branch. */
#define CHIRAL_PLATFORM_ID   "switch"
#define CHIRAL_PLATFORM_NAME "Switch"

/* libnx hands the process its whole heap and newlib's malloc draws from
   all of it, so there is nothing for the mapping trick to work around --
   and Horizon has no mmap anyway. */
#define CHIRAL_BIG_ALLOC_VIA_MMAP 0

/* The bottom bar draws Switch buttons -- A, B, Y, X, L/R/ZL/ZR, Plus and
   Minus -- rather than the PlayStation ones the actions are named after
   (platform_common.cpp). */
#define CHIRAL_BUTTONS_PLAYSTATION 0
