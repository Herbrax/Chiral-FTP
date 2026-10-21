/* USB drives on the Switch: libusbhsfs, exactly as Goldleaf uses it
   (commit b1ff881, GPL build: FAT, exFAT, NTFS, EXT2/3/4) -- proved on the
   console first.

   libusbhsfs mounts each drive as a device of its own, "ums0:", "ums1:".
   The app keeps every path unprefixed (docs/AGENT-NOTES.md, 1), so instead
   of teaching engine/ about devices, a small router becomes newlib's
   default device -- the one an unprefixed path goes to -- and sends
   /usbN/... to umsN:/... and everything else to the SD card ("sdmc"),
   exactly as before. Open files and folders remember which of the two they
   belong to. Listing "/" adds a usbN folder for each mounted drive, so the
   FTP server and the web app reach them too. */

#include "usb_drives.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <switch.h>
#include <usbhsfs.h>

#define MAX_DRIVES 8
#define PATH_LEN 1024


static Mutex g_lock;
/* One call into libusbhsfs at a time, from whichever thread. Each of its
   calls takes the manager's mutex and then the drive's
   (usbHsFsManagerIsDriveContextPointerValid), and then, still holding the
   drive's, takes the manager's again to read the disk
   (usbHsFsManagerGetLogicalUnitContextForFatFsDriveNumber). Two threads on
   one drive -- the free-space thread's statvfs and the UI's listing, on the
   console -- each end up waiting for the other's, for ever. Goldleaf only
   reaches drives from one thread and never meets it; chiral-ftp has the UI,
   the servers and the free-space thread. A drive answers one command at a
   time anyway, so this costs nothing. (Its own thread, mounting a drive
   being plugged in, can still meet a caller that way; that is the
   library's to fix.) */
static RMutex g_call;
static UsbHsFsDevice g_drives[MAX_DRIVES];
static u32 g_count;
static bool g_started;
static const devoptab_t *g_sd;

// Goldleaf's populate callback (drive/drive_Drives.cpp): a copy, under a lock.
static void
populate(const UsbHsFsDevice *devices, u32 count, void *user) {
  (void)user;
  mutexLock(&g_lock);
  g_count = 0;
  if(devices && count) {
    g_count = count > MAX_DRIVES ? MAX_DRIVES : count;
    memcpy(g_drives, devices, g_count * sizeof(*devices));
  }
  mutexUnlock(&g_lock);
}

int
usb_drives_index(const char *path) {
  int n = 0;
  const char *p;

  if(strncmp(path, "/usb", 4) || path[4] < '0' || path[4] > '9') return -1;
  for(p = path + 4; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
  return (*p == 0 || *p == '/') ? n : -1;
}

/* Where a path goes, and as what: /usbN/x -> umsN:/x, anything else ->
   sdmc:<path>. NULL, with errno, when that drive isn't there. */
static const devoptab_t *
route(const char *path, char *out, size_t size, int *drive_root) {
  const int n = usb_drives_index(path);
  if(drive_root) *drive_root = 0;
  if(n >= 0) {
    char dev[16];
    const char *rest = path + 4;
    while(*rest >= '0' && *rest <= '9') rest++;
    snprintf(dev, sizeof(dev), "ums%d:", n);
    const devoptab_t *d = GetDeviceOpTab(dev);
    if(!d) {
      errno = ENOENT;
      return NULL;
    }
    snprintf(out, size, "%s%s", dev, *rest ? rest : "/");
    if(drive_root) *drive_root = !*rest || !strcmp(rest, "/");
    return d;
  }
  snprintf(out, size, "sdmc:%s", path);
  return g_sd;
}

#define ROUTE(path, buf, d)                                                                                            \
  char buf[PATH_LEN];                                                                                                  \
  const devoptab_t *d = route(path, buf, sizeof(buf), NULL);                                                           \
  if(!d) {                                                                                                             \
    r->_errno = ENOENT;                                                                                                \
    return -1;                                                                                                         \
  }

#define NEED(d, fn)                                                                                                    \
  if(!(d)->fn) {                                                                                                       \
    r->_errno = ENOSYS;                                                                                                \
    return -1;                                                                                                         \
  }

/* The call itself. newlib hands every device its own context in
   r->deviceData -- set from the device it thinks it is calling, which is
   the router -- and libusbhsfs's handlers take their drive from exactly
   there (ff_dev.c, ntfs_dev.c, ext_dev.c: fs_ctx = r->deviceData). So the
   real device's context goes in for the call and the router's comes back
   after. Without it, the first call that reached a drive had no drive
   behind it, and crashed. (what and path name the call, for reading.) */
#define CALL(d, what, path, expr)                                                                                      \
  ({                                                                                                                   \
    void *saved_ = r->deviceData;                                                                                      \
    const bool usb_ = (d) != g_sd;                                                                                     \
    if(usb_) rmutexLock(&g_call);                                                                                      \
    r->deviceData = (d)->deviceData;                                                                                   \
    __typeof__(expr) rc_ = (expr);                                                                                     \
    r->deviceData = saved_;                                                                                            \
    if(usb_) rmutexUnlock(&g_call);                                                                                    \
    rc_;                                                                                                               \
  })

/* ---------- open files: the real device's own state, behind ours ---------- */

typedef struct {
  const devoptab_t *real;
  void *inner;
} rfile_t;

static int
r_open(struct _reent *r, void *fs, const char *path, int flags, int mode) {
  rfile_t *f = fs;
  ROUTE(path, real, d);
  NEED(d, open_r);
  f->real = d;
  f->inner = calloc(1, d->structSize ? d->structSize : 1);
  if(!f->inner) {
    r->_errno = ENOMEM;
    return -1;
  }
  int rc = CALL(d, "open", real, d->open_r(r, f->inner, real, flags, mode));
  if(rc == -1) {
    free(f->inner);
    f->inner = NULL;
  }
  return rc;
}

static int
r_close(struct _reent *r, void *fs) {
  rfile_t *f = fs;
  int rc = f->real->close_r ? CALL(f->real, "close", "", f->real->close_r(r, f->inner)) : 0;
  free(f->inner);
  f->inner = NULL;
  return rc;
}

static ssize_t
r_write(struct _reent *r, void *fs, const char *ptr, size_t len) {
  rfile_t *f = fs;
  NEED(f->real, write_r);
  return CALL(f->real, "write", "", f->real->write_r(r, f->inner, ptr, len));
}

static ssize_t
r_read(struct _reent *r, void *fs, char *ptr, size_t len) {
  rfile_t *f = fs;
  NEED(f->real, read_r);
  return CALL(f->real, "read", "", f->real->read_r(r, f->inner, ptr, len));
}

static off_t
r_seek(struct _reent *r, void *fs, off_t pos, int dir) {
  rfile_t *f = fs;
  NEED(f->real, seek_r);
  return CALL(f->real, "seek", "", f->real->seek_r(r, f->inner, pos, dir));
}

static int
r_fstat(struct _reent *r, void *fs, struct stat *st) {
  rfile_t *f = fs;
  NEED(f->real, fstat_r);
  return CALL(f->real, "fstat", "", f->real->fstat_r(r, f->inner, st));
}

static int
r_ftruncate(struct _reent *r, void *fs, off_t len) {
  rfile_t *f = fs;
  NEED(f->real, ftruncate_r);
  return CALL(f->real, "ftruncate", "", f->real->ftruncate_r(r, f->inner, len));
}

static int
r_fsync(struct _reent *r, void *fs) {
  rfile_t *f = fs;
  NEED(f->real, fsync_r);
  return CALL(f->real, "fsync", "", f->real->fsync_r(r, f->inner));
}

static int
r_fchmod(struct _reent *r, void *fs, mode_t mode) {
  rfile_t *f = fs;
  NEED(f->real, fchmod_r);
  return CALL(f->real, "fchmod", "", f->real->fchmod_r(r, f->inner, mode));
}

static long
r_fpathconf(struct _reent *r, void *fs, int name) {
  rfile_t *f = fs;
  NEED(f->real, fpathconf_r);
  return CALL(f->real, "fpathconf", "", f->real->fpathconf_r(r, f->inner, name));
}

/* ---------- paths ---------- */

// A drive's top folder is a folder, whatever its filesystem says about "/".
static void
root_stat(struct stat *st) {
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFDIR | 0777;
  st->st_nlink = 1;
}

static int
r_stat(struct _reent *r, const char *path, struct stat *st) {
  char real[PATH_LEN];
  int root;
  const devoptab_t *d = route(path, real, sizeof(real), &root);
  if(!d) {
    r->_errno = ENOENT;
    return -1;
  }
  NEED(d, stat_r);
  int rc = CALL(d, "stat", real, d->stat_r(r, real, st));
  if(rc && root) {
    root_stat(st);
    return 0;
  }
  return rc;
}

static int
r_lstat(struct _reent *r, const char *path, struct stat *st) {
  char real[PATH_LEN];
  int root;
  const devoptab_t *d = route(path, real, sizeof(real), &root);
  if(!d) {
    r->_errno = ENOENT;
    return -1;
  }
  int (*fn)(struct _reent *, const char *, struct stat *) = d->lstat_r ? d->lstat_r : d->stat_r;
  if(!fn) {
    r->_errno = ENOSYS;
    return -1;
  }
  int rc = CALL(d, "lstat", real, fn(r, real, st));
  if(rc && root) {
    root_stat(st);
    return 0;
  }
  return rc;
}

static int
r_unlink(struct _reent *r, const char *path) {
  ROUTE(path, real, d);
  NEED(d, unlink_r);
  return CALL(d, "unlink", real, d->unlink_r(r, real));
}

static int
r_chdir(struct _reent *r, const char *path) {
  ROUTE(path, real, d);
  NEED(d, chdir_r);
  return CALL(d, "chdir", real, d->chdir_r(r, real));
}

static int
r_mkdir(struct _reent *r, const char *path, int mode) {
  ROUTE(path, real, d);
  NEED(d, mkdir_r);
  return CALL(d, "mkdir", real, d->mkdir_r(r, real, mode));
}

static int
r_rmdir(struct _reent *r, const char *path) {
  ROUTE(path, real, d);
  NEED(d, rmdir_r);
  return CALL(d, "rmdir", real, d->rmdir_r(r, real));
}

static int
r_chmod(struct _reent *r, const char *path, mode_t mode) {
  ROUTE(path, real, d);
  NEED(d, chmod_r);
  return CALL(d, "chmod", real, d->chmod_r(r, real, mode));
}

static int
r_utimes(struct _reent *r, const char *path, const struct timeval times[2]) {
  ROUTE(path, real, d);
  NEED(d, utimes_r);
  return CALL(d, "utimes", real, d->utimes_r(r, real, times));
}

static int
r_statvfs(struct _reent *r, const char *path, struct statvfs *buf) {
  ROUTE(path, real, d);
  NEED(d, statvfs_r);
  return CALL(d, "statvfs", real, d->statvfs_r(r, real, buf));
}

static long
r_pathconf(struct _reent *r, const char *path, int name) {
  ROUTE(path, real, d);
  NEED(d, pathconf_r);
  return CALL(d, "pathconf", real, d->pathconf_r(r, real, name));
}

static ssize_t
r_readlink(struct _reent *r, const char *path, char *buf, size_t size) {
  ROUTE(path, real, d);
  NEED(d, readlink_r);
  return CALL(d, "readlink", real, d->readlink_r(r, real, buf, size));
}

// Two paths: both must land on the same device, or it is a move between drives.
static int
two(struct _reent *r, const char *a, const char *b, char *ra, char *rb, const devoptab_t **d) {
  const devoptab_t *da = route(a, ra, PATH_LEN, NULL), *db = route(b, rb, PATH_LEN, NULL);
  if(!da || !db) {
    r->_errno = ENOENT;
    return -1;
  }
  if(da != db) {
    r->_errno = EXDEV;  // the app copies and deletes instead, as across any two drives
    return -1;
  }
  *d = da;
  return 0;
}

static int
r_rename(struct _reent *r, const char *from, const char *to) {
  char ra[PATH_LEN], rb[PATH_LEN];
  const devoptab_t *d;
  if(two(r, from, to, ra, rb, &d)) return -1;
  NEED(d, rename_r);
  return CALL(d, "rename", ra, d->rename_r(r, ra, rb));
}

static int
r_link(struct _reent *r, const char *existing, const char *link) {
  char ra[PATH_LEN], rb[PATH_LEN];
  const devoptab_t *d;
  if(two(r, existing, link, ra, rb, &d)) return -1;
  NEED(d, link_r);
  return CALL(d, "link", ra, d->link_r(r, ra, rb));
}

static int
r_symlink(struct _reent *r, const char *target, const char *link) {
  char rb[PATH_LEN];
  const devoptab_t *d = route(link, rb, sizeof(rb), NULL);
  if(!d) {
    r->_errno = ENOENT;
    return -1;
  }
  NEED(d, symlink_r);
  return CALL(d, "symlink", rb, d->symlink_r(r, target, rb));
}

/* ---------- folders, and the drives in "/" ---------- */

typedef struct {
  const devoptab_t *real;
  DIR_ITER inner;
  bool real_done;
  int extra, extra_count;
  char extra_name[MAX_DRIVES][12];
} rdir_t;

static DIR_ITER *
r_diropen(struct _reent *r, DIR_ITER *state, const char *path) {
  rdir_t *dir = state->dirStruct;
  char real[PATH_LEN];
  const devoptab_t *d = route(path, real, sizeof(real), NULL);

  memset(dir, 0, sizeof(*dir));
  if(!d) {
    r->_errno = ENOENT;
    return NULL;
  }
  if(!d->diropen_r) {
    r->_errno = ENOSYS;
    return NULL;
  }
  char dev[16];
  snprintf(dev, sizeof(dev), "%.*s", (int)(strchr(real, ':') - real + 1), real);
  dir->real = d;
  dir->inner.device = FindDevice(dev);
  dir->inner.dirStruct = calloc(1, d->dirStateSize ? d->dirStateSize : 1);
  if(!dir->inner.dirStruct) {
    r->_errno = ENOMEM;
    return NULL;
  }
  if(!CALL(d, "diropen", real, d->diropen_r(r, &dir->inner, real))) {
    free(dir->inner.dirStruct);
    dir->inner.dirStruct = NULL;
    return NULL;
  }
  if(d == g_sd && (!strcmp(path, "/") || !path[0])) {
    mutexLock(&g_lock);
    for(u32 i = 0; i < g_count && dir->extra_count < MAX_DRIVES; i++) {
      int n = -1;
      if(sscanf(g_drives[i].name, "ums%d:", &n) == 1 && n >= 0) {
        snprintf(dir->extra_name[dir->extra_count++], sizeof(dir->extra_name[0]), "usb%d", n);
      }
    }
    mutexUnlock(&g_lock);
  }
  return state;
}

static int
r_dirnext(struct _reent *r, DIR_ITER *state, char *name, struct stat *st) {
  rdir_t *dir = state->dirStruct;
  if(!dir->real_done) {
    if(dir->real->dirnext_r && !CALL(dir->real, "dirnext", "", dir->real->dirnext_r(r, &dir->inner, name, st))) return 0;
    dir->real_done = true;
  }
  if(dir->extra < dir->extra_count) {
    strcpy(name, dir->extra_name[dir->extra++]);
    if(st) root_stat(st);
    return 0;
  }
  r->_errno = ENOENT;  // the end, as every device says it
  return -1;
}

static int
r_dirreset(struct _reent *r, DIR_ITER *state) {
  rdir_t *dir = state->dirStruct;
  dir->real_done = false;
  dir->extra = 0;
  NEED(dir->real, dirreset_r);
  return CALL(dir->real, "dirreset", "", dir->real->dirreset_r(r, &dir->inner));
}

static int
r_dirclose(struct _reent *r, DIR_ITER *state) {
  rdir_t *dir = state->dirStruct;
  int rc = 0;
  if(dir->real && dir->real->dirclose_r && dir->inner.dirStruct) rc = CALL(dir->real, "dirclose", "", dir->real->dirclose_r(r, &dir->inner));
  free(dir->inner.dirStruct);
  dir->inner.dirStruct = NULL;
  return rc;
}

static const devoptab_t g_router = {
    .name = "chiral",
    .structSize = sizeof(rfile_t),
    .open_r = r_open,
    .close_r = r_close,
    .write_r = r_write,
    .read_r = r_read,
    .seek_r = r_seek,
    .fstat_r = r_fstat,
    .stat_r = r_stat,
    .link_r = r_link,
    .unlink_r = r_unlink,
    .chdir_r = r_chdir,
    .rename_r = r_rename,
    .mkdir_r = r_mkdir,
    .dirStateSize = sizeof(rdir_t),
    .diropen_r = r_diropen,
    .dirreset_r = r_dirreset,
    .dirnext_r = r_dirnext,
    .dirclose_r = r_dirclose,
    .statvfs_r = r_statvfs,
    .ftruncate_r = r_ftruncate,
    .fsync_r = r_fsync,
    .deviceData = NULL,
    .chmod_r = r_chmod,
    .fchmod_r = r_fchmod,
    .rmdir_r = r_rmdir,
    .lstat_r = r_lstat,
    .utimes_r = r_utimes,
    .fpathconf_r = r_fpathconf,
    .pathconf_r = r_pathconf,
    .symlink_r = r_symlink,
    .readlink_r = r_readlink,
};

/* ---------- start, stop, list ---------- */

void
usb_drives_start(void) {
  mutexInit(&g_lock);
  rmutexInit(&g_call);
  g_sd = GetDeviceOpTab("sdmc:");
  if(!g_sd) return;  // no SD card device: leave everything as it is
  // Goldleaf's drive::Initialize().
  const Result rc = usbHsFsInitialize(0);
  if(R_FAILED(rc)) return;
  usbHsFsSetPopulateCallback(populate, NULL);
  const int router = AddDevice(&g_router);
  if(router < 0) {
    usbHsFsExit();
    return;
  }
  setDefaultDevice(router);
  g_started = true;
}

void
usb_drives_stop(void) {
  if(!g_started) return;
  rmutexLock(&g_call);  // no call into a drive half-way through as it goes
  usbHsFsExit();
  rmutexUnlock(&g_call);
  mutexLock(&g_lock);
  g_count = 0;
  mutexUnlock(&g_lock);
  g_started = false;
}

static const char *
fs_name(u8 type) {
  switch(type) {
  case UsbHsFsDeviceFileSystemType_FAT12: return "FAT12";
  case UsbHsFsDeviceFileSystemType_FAT16: return "FAT16";
  case UsbHsFsDeviceFileSystemType_FAT32: return "FAT32";
  case UsbHsFsDeviceFileSystemType_exFAT: return "exFAT";
  case UsbHsFsDeviceFileSystemType_NTFS: return "NTFS";
  case UsbHsFsDeviceFileSystemType_EXT2: return "EXT2";
  case UsbHsFsDeviceFileSystemType_EXT3: return "EXT3";
  case UsbHsFsDeviceFileSystemType_EXT4: return "EXT4";
  default: return "";
  }
}

int
usb_drives_list(usb_drive_t *out, int max) {
  int n = 0;
  mutexLock(&g_lock);
  for(u32 i = 0; i < g_count && n < max; i++) {
    int idx = -1;
    if(sscanf(g_drives[i].name, "ums%d:", &idx) != 1 || idx < 0) continue;
    snprintf(out[n].path, sizeof(out[n].path), "/usb%d", idx);
    const char *what = g_drives[i].product_name[0] ? g_drives[i].product_name : g_drives[i].manufacturer;
    snprintf(out[n].label, sizeof(out[n].label), "USB drive%s%s%s%s", what[0] ? ": " : "", what,
             fs_name(g_drives[i].fs_type)[0] ? ", " : "", fs_name(g_drives[i].fs_type));
    n++;
  }
  mutexUnlock(&g_lock);
  return n;
}
