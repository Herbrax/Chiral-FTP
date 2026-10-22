#pragma once

#include <stddef.h>

#include "ftp.h"
#include "strbuf.h"

/* Joins dir and name with exactly one slash. */
void path_join(char *out, size_t size, const char *dir, const char *name);
const char *path_basename(const char *path);
/* Writes the parent directory of path ("/" for top-level entries). */
void path_parent(char *out, size_t size, const char *path);
int path_is_safe_name(const char *name);

int localfs_list_json(strbuf_t *b, const char *path);
void localfs_roots_json(strbuf_t *b);
typedef struct localfs_root {
  char path[1024];
  char label[64];
  long long free_bytes;
  long long total_bytes;
} localfs_root_t;

/* Struct versions of the JSON listings, for the native UI. */
int localfs_list(const char *path, ftp_listing_t *out);
int localfs_roots(localfs_root_t *out, int max);

/* realpath(), without a device prefix. libnx answers realpath("/switch")
   with "sdmc:/switch", which is right for the C library and wrong for this
   app: every path it stores, shows, joins or sends to a client is
   unprefixed, and a prefixed one fails the web API's validation and breaks
   path_join. A no-op beyond realpath where there are no device prefixes.
   Falls back to a copy of the input if it can't be resolved. */
void localfs_realpath(const char *in, char *out, size_t size);

int localfs_mkdir_p(const char *path);
/* mkdir, then chmod to the folder mode (CHIRAL_FILE_MODE plus x wherever it
   has r: 0777 on a console, 0755 on a computer) whatever the umask. */
int localfs_mkdir(const char *path);

/* File changes that get past a folder whose permissions are in the way:
   when one fails with EACCES or EPERM, the folder (and the file, for a
   replace) is opened up and it is tried once more.

   This matters on the PS5, where a title's sandbox leaves folders it may
   not write; on a filesystem without permissions the retry never fires and
   these are just the plain calls. Shared code uses them throughout rather
   than rename()/unlink()/open() so that it needn't know which it is on. */
int localfs_rename(const char *from, const char *to);
int localfs_open_write(const char *path, int flags, int mode);
int localfs_unlink(const char *path);
int localfs_rmdir(const char *path);
/* Sets the folder holding path to 0777. 0 if that worked. */
int localfs_open_up_parent(const char *path);
/* dir/name, or "name (2).ext", "name (3).ext"... when that's taken. */
int localfs_free_name(char *out, size_t size, const char *dir, const char *name);
int localfs_delete(const char *path);
/* True for the app's own data folder (data_dir) and anything in it, on a
   computer (CHIRAL_DEVICE_COMPUTER): saved sites with their passwords, the
   server login, the SFTP server's private key. The servers never hand it
   out, whatever folder they share. Links are followed; a name that doesn't
   exist yet is judged by its folder. Always 0 on a console, whose servers
   behave as they always have. */
int localfs_private(const char *path);
/* chmod a file, or a folder and everything in it. Folders get mode plus x
   wherever it has r, so they stay openable (644 makes them 755, like
   chmod -R a+rX); links are left alone. Keeps going past failures and
   returns -1 with the first error in errno. */
int localfs_chmod_tree(const char *path, unsigned int mode);
// The mode a folder gets when mode is applied recursively.
unsigned int localfs_dir_mode(unsigned int mode);
