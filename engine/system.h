#pragma once

/* What the shared code needs from the console it is running on.

   Everything above this line is the same on every platform: the FTP and
   SFTP engine, the transfer queue, the FTP server, the web app, the UI.
   Everything below it differs, and rather than scatter #ifdefs through
   files that have no business knowing where they run, each platform
   implements these in platform/<name>/system.c.

   Its companion is platform/<name>/platform_config.h, which carries the
   traits that have to be known at compile time (see below). The Makefile
   puts the right platform directory on the include path, so a shared file
   just writes #include "platform_config.h".

   app/backend.h is the same idea for the screen, the pad and the keyboard. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where settings, saved sites, history, host keys and logs live. Returned
   as a NUL-terminated absolute path; the engine creates it if missing.
   First writable candidate wins, so a platform may offer more than one. */
int system_data_dir_candidates(const char **out, int max);

/* The drives and shortcuts the file manager and the web app list, most
   useful first. Writes at most max entries; returns how many. */
typedef struct system_root {
  const char *path;
  const char *label;
  int removable; /* listed only when something is actually in it */
} system_root_t;
int system_storage_roots(system_root_t *out, int max);

/* Free bytes on the drive holding path, and its total size. Either may be
   -1 for "don't know", and callers must treat it that way -- on a console
   where answering costs a full scan of the filesystem, -1 is the right
   answer rather than a slow one. */
long long system_free_space(const char *path, long long *total);

/* The address other devices use to reach this machine, for the FTP server
   and web app screens. Writes "unknown" when there isn't one. */
void system_local_ip(char *buf, size_t size);

/* Tells the user something outside the app's own window, if the platform
   has any way to. Falls back to the in-app message log. */
void system_notify(const char *message);

/* The machine's own language as one of the app's codes ("en", "ja", ...),
   or "en" when it can't be told. */
const char *system_language(void);

/* Which of the catalogue's languages this build can actually draw. Writes
   at most max codes and returns how many; 0 means "all of them", which is
   the answer wherever one font covers every script.

   The Switch returns a short list: the console's CJK fonts arrive in
   Nintendo's BFTTF container, which the renderer can't read, so offering
   those languages would mean a screen of empty boxes. */
int system_renderable_languages(const char **out, int max);

/* Extra fonts to merge into the app's own, for scripts it doesn't cover --
   the console's CJK and Arabic faces. Each entry is a block of memory that
   must stay valid for the life of the process. current_lang is the language
   in use, so the platform can put its script first: Han characters that
   Japanese and Chinese share take the forms of whichever font comes first.
   Writes at most max entries and returns how many. */
typedef struct system_font {
  const void *data;
  size_t size;
} system_font_t;
int system_script_fonts(system_font_t *out, int max, const char *current_lang);

/* Default root the FTP server shares. The whole drive on a console; a
   folder you nominate on a computer, which has no business serving "/". */
const char *system_ftpd_default_root(void);

/* UTC struct tm to time_t. timegm() where it exists; computed where it
   doesn't (newlib has no such function). */
#include <time.h>
time_t system_timegm(const struct tm *tm);

#ifdef __cplusplus
}
#endif
