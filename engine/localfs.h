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

int localfs_mkdir_p(const char *path);
/* Free bytes on the drive holding path, and its size (both -1 if unknown). */
long long localfs_free_space(const char *path, long long *total);
/* mkdir, then chmod to PS5_FILE_MODE (0777) whatever the umask. */
int localfs_mkdir(const char *path);

/* File changes that get past a folder the console's sandbox won't let us
   write: when one fails for permissions (EACCES/EPERM), the folder (and the
   file, for a replace) is set to 0777 and it is tried once more. */
int localfs_rename(const char *from, const char *to);
int localfs_open_write(const char *path, int flags, int mode);
int localfs_unlink(const char *path);
int localfs_rmdir(const char *path);
/* Sets the folder holding path to 0777. 0 if that worked. */
int localfs_open_up_parent(const char *path);
/* dir/name, or "name (2).ext", "name (3).ext"... when that's taken. */
int localfs_free_name(char *out, size_t size, const char *dir, const char *name);
int localfs_delete(const char *path);
