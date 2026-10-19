#include "localfs.h"

#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifdef __SCE__
#include <sys/mount.h>
#endif


static int denied(void);  /* the last call failed for permissions */
void
path_join(char *out, size_t size, const char *dir, const char *name) {
  size_t len = strlen(dir);

  while(*name == '/') name++;
  if(len && dir[len - 1] == '/') {
    snprintf(out, size, "%s%s", dir, name);
  } else {
    snprintf(out, size, "%s/%s", dir, name);
  }
}

const char *
path_basename(const char *path) {
  const char *slash = strrchr(path, '/');

  if(slash && !slash[1]) {
    /* Trailing slash: look at the previous component. */
    const char *p = slash;
    while(p > path && p[-1] != '/') p--;
    return p;
  }
  return slash ? slash + 1 : path;
}

void
path_parent(char *out, size_t size, const char *path) {
  const char *slash = strrchr(path, '/');

  if(!slash || slash == path) {
    snprintf(out, size, "/");
  } else {
    snprintf(out, size, "%.*s", (int)(slash - path), path);
  }
}

int
path_is_safe_name(const char *name) {
  return name && *name && strcmp(name, ".") && strcmp(name, "..") &&
         !strchr(name, '/') && !strpbrk(name, "\r\n");
}

/* readdir that tells the end of a folder (NULL, *err 0) from a failed read
   (NULL, *err set). */
static struct dirent *
next_entry(DIR *dir, int *err) {
  struct dirent *ent;

  errno = 0;
  ent = readdir(dir);
  if(!ent) *err = errno;
  return ent;
}

/* Details for one entry. When the console refuses them, the entry is still
   listed: folder or file from the directory record, size and date unknown. */
static int
entry_stat(const char *full, const struct dirent *ent, struct stat *st, int *is_link) {
  *is_link = 0;
  if(!lstat(full, st)) {
    if(!S_ISLNK(st->st_mode)) return 0;
    *is_link = 1;
    if(!stat(full, st)) return 0;
  }
  memset(st, 0, sizeof(*st));
  st->st_mode = ent->d_type == DT_DIR ? S_IFDIR : S_IFREG;
  st->st_size = -1;
  st->st_mtime = -1;
  return 1;
}

int
localfs_list_json(strbuf_t *b, const char *path) {
  DIR *dir = opendir(path);
  struct dirent *ent;
  int comma = 0;

  int err = 0;

  if(!dir) return -1;
  strbuf_append(b, "[");
  while((ent = next_entry(dir, &err))) {
    char full[PATH_MAX];
    struct stat st;
    int is_link;

    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    path_join(full, sizeof(full), path, ent->d_name);
    entry_stat(full, ent, &st, &is_link);
    strbuf_printf(b, "%s{\"name\":", comma ? "," : "");
    json_str(b, ent->d_name);
    strbuf_printf(b, ",\"dir\":%d,\"link\":%d,\"size\":%lld,\"mtime\":%lld,"
                  "\"perm\":\"%04o\"}",
                  S_ISDIR(st.st_mode) ? 1 : 0, is_link,
                  S_ISDIR(st.st_mode) ? -1LL : (long long)st.st_size,
                  (long long)st.st_mtime, (unsigned)(st.st_mode & 07777));
    comma = 1;
  }
  closedir(dir);
  strbuf_append(b, "]");
  if(err) {
    errno = err;
    return -1;
  }
  return 0;
}

int
localfs_list(const char *path, ftp_listing_t *out) {
  DIR *dir = opendir(path);
  struct dirent *ent;

  int err = 0;

  memset(out, 0, sizeof(*out));
  if(!dir) return -1;
  while((ent = next_entry(dir, &err))) {
    char full[PATH_MAX];
    struct stat st;
    ftp_entry_t e;
    int is_link;

    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    path_join(full, sizeof(full), path, ent->d_name);
    memset(&e, 0, sizeof(e));
    int unknown = entry_stat(full, ent, &st, &is_link);
    e.is_link = is_link;
    snprintf(e.name, sizeof(e.name), "%s", ent->d_name);
    e.is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    e.size = e.is_dir ? -1 : (long long)st.st_size;
    e.mtime = (long long)st.st_mtime;
    if(!unknown) snprintf(e.perm, sizeof(e.perm), "%04o", (unsigned)(st.st_mode & 07777));
    if(ftp_listing_add(out, &e)) break;
  }
  closedir(dir);
  if(err) {
    ftp_listing_free(out);
    errno = err;
    return -1;
  }
  return 0;
}

static int
root_info(const char *path, const char *label, localfs_root_t *r) {
  struct stat st;

  if(stat(path, &st) || !S_ISDIR(st.st_mode)) return 0;
  memset(r, 0, sizeof(*r));
  snprintf(r->path, sizeof(r->path), "%s", path);
  snprintf(r->label, sizeof(r->label), "%s", label);
  r->free_bytes = localfs_free_space(path, &r->total_bytes);
  return 1;
}

/* A mount point with nothing in it: the PS5 keeps /mnt/usb0-7, /mnt/ext0-1
   and /mnt/disc as empty folders while no drive or disc is in. */
static int
is_empty_dir(const char *path) {
  DIR *dir = opendir(path);
  struct dirent *ent;
  int err = 0, empty = 1;

  if(!dir) return 1;
  while(empty && (ent = next_entry(dir, &err))) {
    if(strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..")) empty = 0;
  }
  closedir(dir);
  return empty;
}

/* A drive for the list; removable ones only when something is in them. */
static int
add_root(localfs_root_t *out, int *n, int max, const char *path, const char *label, int removable) {
  if(*n >= max || (removable && is_empty_dir(path))) return 0;
  if(!root_info(path, label, &out[*n])) return 0;
  (*n)++;
  return 1;
}

/* The drive list, for the TV and the web app alike: shortcuts first. */
int
localfs_roots(localfs_root_t *out, int max) {
  int n = 0;
#ifdef __SCE__
  add_root(out, &n, max, "/data", "Internal storage", 0);
  add_root(out, &n, max, "/data/homebrew", "Homebrew", 0);
  for(int i = 0; i < 8; i++) {
    char path[32];
    char label[32];
    snprintf(path, sizeof(path), "/mnt/usb%d", i);
    snprintf(label, sizeof(label), "USB drive %d", i);
    add_root(out, &n, max, path, label, 1);
  }
  add_root(out, &n, max, "/mnt/ext0", "Extended storage", 1);
  add_root(out, &n, max, "/mnt/ext1", "Extended storage 2", 1);
  add_root(out, &n, max, "/mnt/disc", "Disc", 1);
  add_root(out, &n, max, "/user", "User data", 0);
  add_root(out, &n, max, "/", "Root", 0);
#else
  {
    const char *home = getenv("HOME");
    const char *test = getenv("PS5FTP_TEST_ROOT");  /* automated UI tests */
    if(test) add_root(out, &n, max, test, "Test drive", 0);
    if(home) add_root(out, &n, max, home, "Home", 0);
  }
  add_root(out, &n, max, "/tmp", "/tmp", 0);
  add_root(out, &n, max, "/", "Root", 0);
#endif
  return n;
}

void
localfs_roots_json(strbuf_t *b) {
  localfs_root_t roots[24];
  int n = localfs_roots(roots, 24);

  strbuf_append(b, "[");
  for(int i = 0; i < n; i++) {
    strbuf_printf(b, "%s{\"path\":", i ? "," : "");
    json_str(b, roots[i].path);
    strbuf_append(b, ",\"label\":");
    json_str(b, roots[i].label);
    strbuf_printf(b, ",\"free\":%lld,\"total\":%lld}", roots[i].free_bytes, roots[i].total_bytes);
  }
  strbuf_append(b, "]");
}

int
localfs_mkdir_p(const char *path) {
  char tmp[PATH_MAX];
  size_t len;

  snprintf(tmp, sizeof(tmp), "%s", path);
  len = strlen(tmp);
  for(size_t i = 1; i <= len; i++) {
    if(tmp[i] == '/' || tmp[i] == 0) {
      char c = tmp[i];
      tmp[i] = 0;
      if(localfs_mkdir(tmp) && errno != EEXIST) return -1;
      tmp[i] = c;
    }
  }
  return 0;
}

/* All names in a folder, read before anything in it changes: deleting while
   reading makes some PS5 filesystems skip entries. */
typedef struct names {
  char **name;
  unsigned char *type;
  int count, cap;
} names_t;

static void
names_free(names_t *n) {
  for(int i = 0; i < n->count; i++) free(n->name[i]);
  free(n->name);
  free(n->type);
  memset(n, 0, sizeof(*n));
}

static int
read_names(const char *path, names_t *out) {
  DIR *dir;
  struct dirent *ent;
  int err = 0;

  memset(out, 0, sizeof(*out));
  if(!(dir = opendir(path))) return -1;
  while((ent = next_entry(dir, &err))) {
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    if(out->count == out->cap) {
      int cap = out->cap ? out->cap * 2 : 64;
      char **name = realloc(out->name, sizeof(*name) * (size_t)cap);
      unsigned char *type = name ? realloc(out->type, (size_t)cap) : NULL;
      if(name) out->name = name;
      if(!name || !type) {
        err = ENOMEM;
        break;
      }
      out->type = type;
      out->cap = cap;
    }
    if(!(out->name[out->count] = strdup(ent->d_name))) {
      err = ENOMEM;
      break;
    }
    out->type[out->count++] = ent->d_type;
  }
  closedir(dir);
  if(err) {
    names_free(out);
    errno = err;
    return -1;
  }
  return 0;
}

/* Deletes a folder's contents, then the folder. Links are removed, never
   followed. Keeps going past failures; returns -1 with the first error. */
static int
delete_tree(const char *path, int depth) {
  int first_err = 0;

  if(depth > 64) {
    errno = ELOOP;
    return -1;
  }
  chmod(path, PS5_FILE_MODE);  // its entries can only go if it's writable
  /* A few passes: anything a pass couldn't see is picked up by the next. */
  for(int pass = 0; pass < 4; pass++) {
    names_t n;
    int deleted = 0;

    if(read_names(path, &n)) {
      if(!first_err) first_err = errno;
      break;
    }
    if(!n.count) break;
    for(int i = 0; i < n.count; i++) {
      char child[PATH_MAX];
      int is_dir = n.type[i] == DT_DIR;
      struct stat st;

      path_join(child, sizeof(child), path, n.name[i]);
      if(n.type[i] == DT_UNKNOWN && !lstat(child, &st)) is_dir = S_ISDIR(st.st_mode);
      if(is_dir ? delete_tree(child, depth + 1) : localfs_unlink(child)) {
        if(!first_err) first_err = errno;
      } else {
        deleted++;
      }
    }
    names_free(&n);
    if(!deleted) break;  // nothing more can go
  }
  if(rmdir(path) && (!denied() || localfs_open_up_parent(path) || rmdir(path))) {
    if(!first_err) first_err = errno;
  }
  if(first_err) {
    errno = first_err;
    return -1;
  }
  return 0;
}

int
localfs_delete(const char *path) {
  struct stat st;

  if(lstat(path, &st)) return -1;
  if(!S_ISDIR(st.st_mode)) return localfs_unlink(path);
  return delete_tree(path, 0);
}

int
localfs_free_name(char *out, size_t size, const char *dir, const char *name) {
  struct stat st;
  const char *dot = strrchr(name, '.');
  int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);

  path_join(out, size, dir, name);
  for(int n = 2; !stat(out, &st); n++) {
    char alt[512];
    if(n > 999) return -1;
    snprintf(alt, sizeof(alt), "%.*s (%d)%s", stem, name, n, name + stem);
    path_join(out, size, dir, alt);
  }
  return 0;
}

static int
denied(void) {
  return errno == EACCES || errno == EPERM;
}

int
localfs_open_up_parent(const char *path) {
  char dir[PATH_MAX];

  path_parent(dir, sizeof(dir), path);
  return chmod(dir, PS5_FILE_MODE);
}

int
localfs_mkdir(const char *path) {
  if(mkdir(path, PS5_FILE_MODE) && (!denied() || localfs_open_up_parent(path) || mkdir(path, PS5_FILE_MODE)))
    return -1;
  chmod(path, PS5_FILE_MODE);
  return 0;
}

int
localfs_rename(const char *from, const char *to) {
  if(!rename(from, to)) return 0;
  if(!denied()) return -1;
  localfs_open_up_parent(from);
  localfs_open_up_parent(to);
  chmod(from, PS5_FILE_MODE);  // a folder being moved needs its own write access too
  return rename(from, to);
}

int
localfs_open_write(const char *path, int flags, int mode) {
  int fd = open(path, flags, mode);
  if(fd >= 0 || !denied()) return fd;
  localfs_open_up_parent(path);
  chmod(path, PS5_FILE_MODE);  // replacing a file that isn't writable
  return open(path, flags, mode);
}

int
localfs_rmdir(const char *path) {
  if(!rmdir(path)) return 0;
  if(!denied() || localfs_open_up_parent(path)) return -1;
  return rmdir(path);
}

int
localfs_unlink(const char *path) {
  if(!unlink(path)) return 0;
  if(!denied() || localfs_open_up_parent(path)) return -1;
  return unlink(path);
}

/* Free space on the drive holding path (-1 if unknown). statvfs comes from
   libkernel on the PS5; the SDK's own statfs is a raw system call a title
   may not make. */
long long
localfs_free_space(const char *path, long long *total) {
  struct statvfs vfs;

  if(total) *total = -1;
  if(statvfs(path, &vfs)) return -1;
  if(total) *total = (long long)vfs.f_blocks * (long long)vfs.f_frsize;
  return (long long)vfs.f_bavail * (long long)vfs.f_frsize;
}
