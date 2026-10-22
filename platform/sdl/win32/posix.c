/* The POSIX calls of posix.h, on Win32 and Winsock. Paths arrive as UTF-8
   POSIX paths and leave as UTF-16 Win32 ones; errors leave as errno. */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winioctl.h>

#include <ctype.h>
#include <strings.h>
#include <stdarg.h>

#include "posix.h"
#include "dirent.h"

/* Everything below calls the real functions, not the macros above. */
#undef stat
#undef fstat
#undef lstat
#undef open
#undef close
#undef lseek
#undef ftruncate
#undef truncate
#undef fsync
#undef pread
#undef pwrite
#undef mkdir
#undef rmdir
#undef unlink
#undef rename
#undef chmod
#undef fchmod
#undef access
#undef realpath
#undef readlink
#undef utimes
#undef utime
#undef fopen
#undef getenv
#undef mmap
#undef munmap
#undef timegm
#undef socket
#undef accept
#undef bind
#undef listen
#undef connect
#undef shutdown
#undef recv
#undef send
#undef recvfrom
#undef sendto
#undef setsockopt
#undef getsockopt
#undef getsockname
#undef getpeername
#undef poll
#undef fcntl
#undef read
#undef write
#undef signal
#undef opendir
#undef readdir
#undef closedir

/* ---------- start-up ---------- */

/* The CRT's answer to a bad descriptor is to end the program; POSIX's is
   EBADF, which is what the shared code checks for. */
static void
ignore_invalid_parameter(const wchar_t *expr, const wchar_t *fn, const wchar_t *file, unsigned line,
                         uintptr_t reserved) {
  (void)expr;
  (void)fn;
  (void)file;
  (void)line;
  (void)reserved;
}

__attribute__((constructor)) static void
posix_init(void) {
  WSADATA wsa;

  WSAStartup(MAKEWORD(2, 2), &wsa);
  _set_invalid_parameter_handler(ignore_invalid_parameter);
  /* Files are bytes: no CRLF translation, for anything opened by the CRT
     (the libraries' fopen() too). */
  _set_fmode(_O_BINARY);
}

/* ---------- errors ---------- */

static int
errno_from_win32(DWORD e) {
  switch(e) {
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
  case ERROR_INVALID_DRIVE:
  case ERROR_BAD_NETPATH:
  case ERROR_BAD_NET_NAME:
  case ERROR_INVALID_NAME:
  case ERROR_BAD_PATHNAME:
  case ERROR_NOT_READY:
    return ENOENT;
  case ERROR_ACCESS_DENIED:
  case ERROR_PRIVILEGE_NOT_HELD:
  case ERROR_CANNOT_MAKE:
  case ERROR_NETWORK_ACCESS_DENIED:
    return EACCES;
  case ERROR_WRITE_PROTECT: return EROFS;
  case ERROR_SHARING_VIOLATION:
  case ERROR_LOCK_VIOLATION:
  case ERROR_BUSY:
    return EBUSY;
  case ERROR_FILE_EXISTS:
  case ERROR_ALREADY_EXISTS:
    return EEXIST;
  case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
  case ERROR_DIRECTORY: return ENOTDIR;
  case ERROR_DISK_FULL:
  case ERROR_HANDLE_DISK_FULL:
    return ENOSPC;
  case ERROR_NOT_SAME_DEVICE: return EXDEV;
  case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
  case ERROR_INVALID_HANDLE: return EBADF;
  case ERROR_NOT_ENOUGH_MEMORY:
  case ERROR_OUTOFMEMORY:
    return ENOMEM;
  case ERROR_NOT_SUPPORTED: return ENOSYS;
  case ERROR_CANT_RESOLVE_FILENAME: return ELOOP;
  default: return EIO;
  }
}

static int
fail_win32(void) {
  errno = errno_from_win32(GetLastError());
  return -1;
}

static int
errno_from_wsa(int e) {
  switch(e) {
  case WSAEINTR: return EINTR;
  case WSAEBADF:
  case WSAENOTSOCK:
    return EBADF;
  case WSAEACCES: return EACCES;
  case WSAEFAULT: return EFAULT;
  case WSAEINVAL: return EINVAL;
  case WSAEMFILE: return EMFILE;
  case WSAEWOULDBLOCK: return EAGAIN;
  case WSAEINPROGRESS: return EINPROGRESS;
  case WSAEALREADY: return EALREADY;
  case WSAEDESTADDRREQ: return EDESTADDRREQ;
  case WSAEMSGSIZE: return EMSGSIZE;
  case WSAEPROTOTYPE: return EPROTOTYPE;
  case WSAENOPROTOOPT: return ENOPROTOOPT;
  case WSAEPROTONOSUPPORT: return EPROTONOSUPPORT;
  case WSAEOPNOTSUPP: return EOPNOTSUPP;
  case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
  case WSAEADDRINUSE: return EADDRINUSE;
  case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
  case WSAENETDOWN: return ENETDOWN;
  case WSAENETUNREACH: return ENETUNREACH;
  case WSAENETRESET: return ENETRESET;
  case WSAECONNABORTED: return ECONNABORTED;
  case WSAECONNRESET: return ECONNRESET;
  case WSAENOBUFS: return ENOBUFS;
  case WSAEISCONN: return EISCONN;
  case WSAENOTCONN: return ENOTCONN;
  case WSAESHUTDOWN: return EPIPE;
  case WSAETIMEDOUT: return ETIMEDOUT;
  case WSAECONNREFUSED: return ECONNREFUSED;
  case WSAELOOP: return ELOOP;
  case WSAENAMETOOLONG: return ENAMETOOLONG;
  case WSAEHOSTDOWN:
  case WSAEHOSTUNREACH:
    return EHOSTUNREACH;
  case WSANOTINITIALISED: return EIO;
  default: return EIO;
  }
}

static int
fail_wsa(void) {
  errno = errno_from_wsa(WSAGetLastError());
  return -1;
}

/* A recv() or send() that ran out of SO_RCVTIMEO/SO_SNDTIMEO: EAGAIN, as a
   POSIX system says, rather than Winsock's WSAETIMEDOUT. */
static int
fail_wsa_io(void) {
  int e = WSAGetLastError();
  errno = e == WSAETIMEDOUT ? EAGAIN : errno_from_wsa(e);
  return -1;
}

/* ---------- paths ---------- */

static int
is_slash(char c) {
  return c == '/' || c == '\\';
}

/* "/" alone, or any number of slashes: the folder of drives. */
static int
is_root(const char *path) {
  if(!is_slash(path[0])) return 0;
  while(is_slash(*path)) path++;
  return !*path;
}

/* A device rather than a file, in any folder: CON, NUL, COM1... with or
   without an extension ("nul.txt" is NUL too). */
static int
is_device_name(const char *s, size_t n) {
  static const char *const names[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$", "CLOCK$"};
  size_t base = 0;

  while(base < n && s[base] != '.') base++;
  while(base > 0 && s[base - 1] == ' ') base--;
  for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if(strlen(names[i]) == base && !strncasecmp(s, names[i], base)) return 1;
  }
  return base == 4 && (!strncasecmp(s, "COM", 3) || !strncasecmp(s, "LPT", 3)) && isdigit((unsigned char)s[3]);
}

/* One name of a path, as POSIX means it, is one name on Windows: nothing
   Windows would read as a separator (\), a stream (:), a pattern (* ?), a
   device (NUL), or quietly change (a trailing dot or space). This is what
   keeps "..\..\Windows" from leaving a shared folder, since everything
   above this file only knows "/" as a separator. */
static int
valid_name(const char *s, size_t n) {
  if(!n || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 1;
  for(size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)s[i];
    if(c < 32 || strchr("\\:*?\"<>|", c)) return 0;
  }
  if(s[n - 1] == '.' || s[n - 1] == ' ') return 0;
  return !is_device_name(s, n);
}

static int
valid_names(const char *p) {
  while(*p) {
    const char *end = strchr(p, '/');
    const size_t n = end ? (size_t)(end - p) : strlen(p);
    if(!valid_name(p, n)) return 0;
    p += n;
    while(*p == '/') p++;
  }
  return 1;
}

/* "/c" or "/c/...": the drive letter, else 0. */
static char
drive_of(const char *path) {
  if(path[0] == '/' && isalpha((unsigned char)path[1]) && (!path[2] || path[2] == '/')) {
    return (char)toupper((unsigned char)path[1]);
  }
  return 0;
}

int
chiral_win_path(const char *path, wchar_t *out, size_t count) {
  char buf[PATH_MAX + 8];
  char letter;
  size_t len;
  int n;

  if(!path || !*path) {
    errno = ENOENT;
    return -1;
  }
  if(is_root(path)) return 0;
  if((letter = drive_of(path))) {
    snprintf(buf, sizeof(buf), "%c:/%s", letter, path[2] ? path + 3 : "");
  } else if(path[0] == '/' && path[1] != '/') {
    /* "/tmp", "/etc": not on a drive, so not anywhere on Windows. */
    errno = ENOENT;
    return -1;
  } else {
    /* Relative, a network share ("//server/share"), or already Win32 ("C:/"). */
    if(snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) {
      errno = ENAMETOOLONG;
      return -1;
    }
  }
  if(!valid_names(isalpha((unsigned char)buf[0]) && buf[1] == ':' && buf[2] == '/' ? buf + 3 : buf)) {
    errno = EINVAL;
    return -1;
  }
  /* No trailing slash, except a drive's own ("C:/"): FindFirstFile and
     friends refuse "C:\Users\". */
  len = strlen(buf);
  while(len > 3 && is_slash(buf[len - 1])) buf[--len] = 0;
  if(len == 2 && buf[1] == ':') {
    buf[2] = '/';
    buf[3] = 0;
  }

  /* Past MAX_PATH only with the \\?\ prefix, which also means a full,
     backslashed path with nothing to resolve. */
  if(count < 8) {
    errno = ENAMETOOLONG;
    return -1;
  }
  n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf, -1, out + 4, (int)count - 4);
  if(n <= 0) {
    errno = GetLastError() == ERROR_INSUFFICIENT_BUFFER ? ENAMETOOLONG : EINVAL;
    return -1;
  }
  for(wchar_t *p = out + 4; *p; p++) {
    if(*p == L'/') *p = L'\\';
  }
  if(n > MAX_PATH - 12 && iswalpha(out[4]) && out[5] == L':') {
    out[0] = L'\\';
    out[1] = L'\\';
    out[2] = L'?';
    out[3] = L'\\';
  } else {
    memmove(out, out + 4, (size_t)n * sizeof(wchar_t));
  }
  return 1;
}

void
chiral_posix_path(const wchar_t *path, char *out, size_t size) {
  char buf[PATH_MAX];

  if(!WideCharToMultiByte(CP_UTF8, 0, path, -1, buf, sizeof(buf), NULL, NULL)) buf[0] = 0;
  chiral_posix_path_utf8(buf, out, size);
}

void
chiral_posix_path_utf8(const char *path, char *in_out, size_t size) {
  char buf[PATH_MAX];
  const char *p = path;
  size_t len;

  if(!strncmp(p, "\\\\?\\UNC\\", 8)) {
    snprintf(buf, sizeof(buf), "//%s", p + 8);
  } else {
    if(!strncmp(p, "\\\\?\\", 4)) p += 4;
    if(isalpha((unsigned char)p[0]) && p[1] == ':') {
      snprintf(buf, sizeof(buf), "/%c%s%s", tolower((unsigned char)p[0]), is_slash(p[2]) || !p[2] ? "" : "/",
               p + 2);
    } else {
      snprintf(buf, sizeof(buf), "%s", p);
    }
  }
  for(char *c = buf; *c; c++) {
    if(*c == '\\') *c = '/';
  }
  len = strlen(buf);
  while(len > 1 && buf[len - 1] == '/') buf[--len] = 0;
  snprintf(in_out, size, "%s", buf);
}

/* The Win32 form of a path, or -1 with errno set; 0 for "/". */
#define WPATH(var, path) \
  wchar_t var[PATH_MAX + 8]; \
  const int var##_kind = chiral_win_path(path, var, sizeof(var) / sizeof(var[0]))

/* ---------- stat ---------- */

static time_t
unix_time(FILETIME ft) {
  const unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  if(!t) return 0;
  return (time_t)((long long)(t / 10000000ULL) - 11644473600LL);
}

static FILETIME
file_time(time_t t) {
  const unsigned long long v = ((unsigned long long)t + 11644473600ULL) * 10000000ULL;
  FILETIME ft = {(DWORD)v, (DWORD)(v >> 32)};
  return ft;
}

/* Symbolic links and junctions are links; every other reparse point
   (OneDrive's placeholders, deduplicated files) is the file it stands for. */
static int
is_link_tag(DWORD tag) {
  return tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT;
}

static unsigned
mode_of(DWORD attr, int link) {
  if(link) return S_IFLNK | 0777;
  if(attr & FILE_ATTRIBUTE_DIRECTORY) return S_IFDIR | 0755;
  return S_IFREG | ((attr & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
}

static void
fill_root(struct chiral_stat *st) {
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFDIR | 0755;
  st->st_nlink = 1;
}

static void
fill_info(struct chiral_stat *st, const BY_HANDLE_FILE_INFORMATION *i) {
  memset(st, 0, sizeof(*st));
  st->st_mode = mode_of(i->dwFileAttributes, 0);
  st->st_nlink = i->nNumberOfLinks;
  st->st_dev = i->dwVolumeSerialNumber;
  st->st_ino = ((unsigned long long)i->nFileIndexHigh << 32) | i->nFileIndexLow;
  st->st_size = (i->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    ? 0
                    : (long long)(((unsigned long long)i->nFileSizeHigh << 32) | i->nFileSizeLow);
  st->st_atime = unix_time(i->ftLastAccessTime);
  st->st_mtime = unix_time(i->ftLastWriteTime);
  st->st_ctime = unix_time(i->ftCreationTime);
}

static void
fill_attr(struct chiral_stat *st, const WIN32_FILE_ATTRIBUTE_DATA *a, int link) {
  memset(st, 0, sizeof(*st));
  st->st_mode = mode_of(a->dwFileAttributes, link);
  st->st_nlink = 1;
  st->st_size = (a->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    ? 0
                    : (long long)(((unsigned long long)a->nFileSizeHigh << 32) | a->nFileSizeLow);
  st->st_atime = unix_time(a->ftLastAccessTime);
  st->st_mtime = unix_time(a->ftLastWriteTime);
  st->st_ctime = unix_time(a->ftCreationTime);
}

/* The reparse tag of a path that has one, else 0. */
static DWORD
reparse_tag(const wchar_t *path) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW(path, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);

  if(h == INVALID_HANDLE_VALUE) return 0;
  FindClose(h);
  return (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? fd.dwReserved0 : 0;
}

int
chiral_stat(const char *path, struct chiral_stat *st) {
  WPATH(w, path);
  BY_HANDLE_FILE_INFORMATION info;
  WIN32_FILE_ATTRIBUTE_DATA attr;
  HANDLE h;

  if(w_kind < 0) return -1;
  if(!w_kind) {
    fill_root(st);
    return 0;
  }
  /* Through any link, to what it points at. */
  h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if(h != INVALID_HANDLE_VALUE) {
    const BOOL ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if(ok) {
      fill_info(st, &info);
      return 0;
    }
  }
  /* Files nobody may open (pagefile.sys) still have attributes. */
  if(!GetFileAttributesExW(w, GetFileExInfoStandard, &attr)) return fail_win32();
  fill_attr(st, &attr, 0);
  return 0;
}

int
chiral_lstat(const char *path, struct chiral_stat *st) {
  WPATH(w, path);
  WIN32_FILE_ATTRIBUTE_DATA attr;

  if(w_kind < 0) return -1;
  if(!w_kind) {
    fill_root(st);
    return 0;
  }
  if(!GetFileAttributesExW(w, GetFileExInfoStandard, &attr)) return fail_win32();
  if((attr.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && is_link_tag(reparse_tag(w))) {
    fill_attr(st, &attr, 1);
    return 0;
  }
  return chiral_stat(path, st);
}

static HANDLE
fd_handle(int fd) {
  return (HANDLE)_get_osfhandle(fd);
}

int
chiral_fstat(int fd, struct chiral_stat *st) {
  BY_HANDLE_FILE_INFORMATION info;
  HANDLE h = fd_handle(fd);

  if(h == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  if(GetFileType(h) != FILE_TYPE_DISK) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFIFO | 0600;
    return 0;
  }
  if(!GetFileInformationByHandle(h, &info)) return fail_win32();
  fill_info(st, &info);
  return 0;
}

/* ---------- files ---------- */

static int
is_dir_w(const wchar_t *w) {
  const DWORD a = GetFileAttributesW(w);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int
chiral_open(const char *path, int flags, ...) {
  WPATH(w, path);
  DWORD access, disposition, attrs = FILE_ATTRIBUTE_NORMAL;
  HANDLE h;
  int mode = 0666, fd;

  if(flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }
  if(w_kind < 0) return -1;
  if(!w_kind) {
    errno = EISDIR;
    return -1;
  }
  switch(flags & (O_RDONLY | O_WRONLY | O_RDWR)) {
  case O_WRONLY: access = GENERIC_WRITE; break;
  case O_RDWR: access = GENERIC_READ | GENERIC_WRITE; break;
  default: access = GENERIC_READ; break;
  }
  if((flags & O_CREAT) && (flags & O_EXCL)) disposition = CREATE_NEW;
  else if((flags & O_CREAT) && (flags & O_TRUNC)) disposition = CREATE_ALWAYS;
  else if(flags & O_CREAT) disposition = OPEN_ALWAYS;
  else if(flags & O_TRUNC) disposition = TRUNCATE_EXISTING;
  else disposition = OPEN_EXISTING;
  if((flags & O_CREAT) && !(mode & 0200)) attrs = FILE_ATTRIBUTE_READONLY;

  /* Shared every way, as on POSIX: an open file can still be read,
     renamed or deleted by someone else. */
  h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disposition, attrs,
                  NULL);
  if(h == INVALID_HANDLE_VALUE) {
    const DWORD e = GetLastError();
    if(e == ERROR_ACCESS_DENIED && is_dir_w(w)) {
      errno = EISDIR;
      return -1;
    }
    errno = errno_from_win32(e);
    return -1;
  }
  fd = _open_osfhandle((intptr_t)h, (flags & O_APPEND ? _O_APPEND : 0) |
                                        ((flags & (O_WRONLY | O_RDWR)) ? 0 : _O_RDONLY));
  if(fd < 0) {
    CloseHandle(h);
    errno = EMFILE;
  }
  return fd;
}

/* A live socket with this number, as opposed to a CRT descriptor. */
static int
is_socket(int fd) {
  int type;
  int len = sizeof(type);

  if(fd < 0) return 0;
  return getsockopt((SOCKET)fd, SOL_SOCKET, SO_TYPE, (char *)&type, &len) == 0;
}

int
chiral_close(int fd) {
  if(is_socket(fd)) return closesocket((SOCKET)fd) ? fail_wsa() : 0;
  return _close(fd);
}

ssize_t
chiral_read(int fd, void *buf, size_t len) {
  if(len > INT_MAX) len = INT_MAX;
  if(is_socket(fd)) {
    const int n = recv((SOCKET)fd, (char *)buf, (int)len, 0);
    return n < 0 ? fail_wsa_io() : n;
  }
  return _read(fd, buf, (unsigned)len);
}

ssize_t
chiral_write(int fd, const void *buf, size_t len) {
  if(len > INT_MAX) len = INT_MAX;
  if(is_socket(fd)) {
    const int n = send((SOCKET)fd, (const char *)buf, (int)len, 0);
    return n < 0 ? fail_wsa_io() : n;
  }
  return _write(fd, buf, (unsigned)len);
}

long long
chiral_lseek(int fd, long long offset, int whence) {
  return _lseeki64(fd, offset, whence);
}

int
chiral_ftruncate(int fd, long long size) {
  const errno_t e = _chsize_s(fd, size);
  if(e) {
    errno = e;
    return -1;
  }
  return 0;
}

int
chiral_truncate(const char *path, long long size) {
  int fd = chiral_open(path, O_WRONLY);
  int r;

  if(fd < 0) return -1;
  r = chiral_ftruncate(fd, size);
  _close(fd);
  return r;
}

int
chiral_fsync(int fd) {
  HANDLE h = fd_handle(fd);

  if(h == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  return FlushFileBuffers(h) ? 0 : fail_win32();
}

ssize_t
chiral_pread(int fd, void *buf, size_t size, long long offset) {
  HANDLE h = fd_handle(fd);
  OVERLAPPED o;
  DWORD n = 0;

  if(h == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  memset(&o, 0, sizeof(o));
  o.Offset = (DWORD)offset;
  o.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
  if(size > 0x7fffffff) size = 0x7fffffff;
  if(!ReadFile(h, buf, (DWORD)size, &n, &o)) {
    if(GetLastError() == ERROR_HANDLE_EOF) return 0;
    return fail_win32();
  }
  return (ssize_t)n;
}

ssize_t
chiral_pwrite(int fd, const void *buf, size_t size, long long offset) {
  HANDLE h = fd_handle(fd);
  OVERLAPPED o;
  DWORD n = 0;

  if(h == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  memset(&o, 0, sizeof(o));
  o.Offset = (DWORD)offset;
  o.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
  if(size > 0x7fffffff) size = 0x7fffffff;
  if(!WriteFile(h, buf, (DWORD)size, &n, &o)) return fail_win32();
  return (ssize_t)n;
}

int
chiral_mkdir(const char *path, int mode) {
  WPATH(w, path);

  (void)mode;
  if(w_kind < 0) return -1;
  if(!w_kind) {
    errno = EEXIST;
    return -1;
  }
  if(CreateDirectoryW(w, NULL)) return 0;
  /* A drive's own folder answers "access denied" rather than "exists". */
  if(GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES) {
    errno = EEXIST;
    return -1;
  }
  return fail_win32();
}

int
chiral_rmdir(const char *path) {
  WPATH(w, path);

  if(w_kind < 0) return -1;
  if(!w_kind) {
    errno = EBUSY;
    return -1;
  }
  if(RemoveDirectoryW(w)) return 0;
  if(GetLastError() == ERROR_DIRECTORY) {
    errno = ENOTDIR;
    return -1;
  }
  return fail_win32();
}

int
chiral_unlink(const char *path) {
  WPATH(w, path);
  DWORD attr;

  if(w_kind < 0) return -1;
  if(!w_kind) {
    errno = EISDIR;
    return -1;
  }
  if(DeleteFileW(w)) return 0;
  if(GetLastError() != ERROR_ACCESS_DENIED || (attr = GetFileAttributesW(w)) == INVALID_FILE_ATTRIBUTES) {
    return fail_win32();
  }
  if(attr & FILE_ATTRIBUTE_DIRECTORY) {
    /* A link to a folder goes the way a link does; a folder is EISDIR. */
    if((attr & FILE_ATTRIBUTE_REPARSE_POINT) && is_link_tag(reparse_tag(w))) {
      return RemoveDirectoryW(w) ? 0 : fail_win32();
    }
    errno = EISDIR;
    return -1;
  }
  /* POSIX deletes a read-only file from a folder that can be written. */
  if(attr & FILE_ATTRIBUTE_READONLY) {
    SetFileAttributesW(w, attr & ~FILE_ATTRIBUTE_READONLY);
    if(DeleteFileW(w)) return 0;
    SetFileAttributesW(w, attr);
  }
  return fail_win32();
}

int
chiral_rename(const char *from, const char *to) {
  WPATH(wf, from);
  WPATH(wt, to);

  if(wf_kind < 0 || wt_kind < 0) return -1;
  if(!wf_kind || !wt_kind) {
    errno = EBUSY;
    return -1;
  }
  /* Over an existing file, as rename() does; never by copying, so a move
     between drives fails with EXDEV and the caller copies. */
  if(MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING)) return 0;
  return fail_win32();
}

int
chiral_chmod(const char *path, int mode) {
  WPATH(w, path);
  DWORD attr;

  if(w_kind < 0) return -1;
  if(!w_kind) return 0;
  if((attr = GetFileAttributesW(w)) == INVALID_FILE_ATTRIBUTES) return fail_win32();
  /* What Windows has of a mode: a file without the owner's w is read-only.
     A folder's read-only flag means something else entirely (Explorer's
     customization), so folders are left alone. */
  if(attr & FILE_ATTRIBUTE_DIRECTORY) return 0;
  attr = (mode & 0200) ? (attr & ~FILE_ATTRIBUTE_READONLY) : (attr | FILE_ATTRIBUTE_READONLY);
  return SetFileAttributesW(w, attr) ? 0 : fail_win32();
}

int
chiral_fchmod(int fd, int mode) {
  (void)fd;
  (void)mode;
  return 0;
}

int
chiral_access(const char *path, int mode) {
  WPATH(w, path);
  DWORD attr;

  if(w_kind < 0) return -1;
  if(!w_kind) return 0;
  if((attr = GetFileAttributesW(w)) == INVALID_FILE_ATTRIBUTES) return fail_win32();
  if((mode & W_OK) && (attr & FILE_ATTRIBUTE_READONLY) && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
    errno = EACCES;
    return -1;
  }
  return 0;
}

char *
chiral_realpath(const char *path, char *resolved) {
  WPATH(w, path);
  wchar_t full[PATH_MAX + 8];
  char out[PATH_MAX];
  HANDLE h;
  DWORD n = 0;

  if(w_kind < 0) return NULL;
  if(!w_kind) {
    snprintf(out, sizeof(out), "/");
  } else {
    /* The final path: links followed, and each name as the disk spells it,
       so "inside the shared folder" can be decided by comparing strings. */
    h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if(h != INVALID_HANDLE_VALUE) {
      n = GetFinalPathNameByHandleW(h, full, PATH_MAX, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      CloseHandle(h);
      /* A mapped network drive comes back as \\?\UNC\server\share; keep
         its letter, which is how the rest of the program knows it. */
      if(n && !wcsncmp(full, L"\\\\?\\UNC\\", 8) && iswalpha(w[0]) && w[1] == L':') n = 0;
    }
    if(!n || n >= PATH_MAX) {
      if(GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) {
        fail_win32();
        return NULL;
      }
      n = GetFullPathNameW(w, PATH_MAX, full, NULL);
      if(!n || n >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
      }
    }
    chiral_posix_path(full, out, sizeof(out));
  }
  if(!resolved && !(resolved = malloc(PATH_MAX))) {
    errno = ENOMEM;
    return NULL;
  }
  snprintf(resolved, PATH_MAX, "%s", out);
  return resolved;
}

/* Where a link leads, as a full POSIX path. Windows keeps a symbolic
   link's own text too, but a junction has none, and a full path is a
   correct answer for both. */
ssize_t
chiral_readlink(const char *path, char *buf, size_t size) {
  struct chiral_stat st;
  char target[PATH_MAX];
  size_t n;

  if(chiral_lstat(path, &st)) return -1;
  if(!S_ISLNK(st.st_mode)) {
    errno = EINVAL;
    return -1;
  }
  if(!chiral_realpath(path, target)) return -1;
  n = strlen(target);
  if(n > size) n = size;
  memcpy(buf, target, n);  /* not terminated, as readlink() doesn't */
  return (ssize_t)n;
}

static int
set_times(const char *path, time_t atime, time_t mtime) {
  WPATH(w, path);
  FILETIME a, m;
  HANDLE h;
  BOOL ok;

  if(w_kind < 0) return -1;
  if(!w_kind) {
    errno = EPERM;
    return -1;
  }
  h = CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if(h == INVALID_HANDLE_VALUE) return fail_win32();
  a = file_time(atime);
  m = file_time(mtime);
  ok = SetFileTime(h, NULL, &a, &m);
  CloseHandle(h);
  return ok ? 0 : fail_win32();
}

int
chiral_utimes(const char *path, const struct timeval times[2]) {
  const time_t now = time(NULL);
  return set_times(path, times ? times[0].tv_sec : now, times ? times[1].tv_sec : now);
}

int
chiral_utime(const char *path, const struct utimbuf *times) {
  const time_t now = time(NULL);
  return set_times(path, times ? times->actime : now, times ? times->modtime : now);
}

FILE *
chiral_fopen(const char *path, const char *mode) {
  WPATH(w, path);
  wchar_t wmode[16];

  if(w_kind < 0) return NULL;
  if(!w_kind) {
    errno = EISDIR;
    return NULL;
  }
  if(!MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16)) {
    errno = EINVAL;
    return NULL;
  }
  return _wfopen(w, wmode);
}

char *
chiral_getenv(const char *name) {
  /* HOME is the user's folder, as the shared code expects; Windows calls
     it USERPROFILE and writes it C:\Users\<name>. */
  if(!strcmp(name, "HOME")) {
    static char home[PATH_MAX];
    const char *profile = getenv("USERPROFILE");

    if(!profile || !*profile) return NULL;
    if(!home[0]) chiral_posix_path_utf8(profile, home, sizeof(home));
    return home;
  }
  return getenv(name);
}

/* ---------- folders ---------- */

struct chiral_dir {
  HANDLE find;
  int first;      /* data holds an entry readdir() hasn't returned yet */
  int root;       /* "/", listing drives */
  int drives;     /* the drive letters left to list, as a bit mask */
  int done;
  WIN32_FIND_DATAW data;
  struct dirent ent;
};

DIR *
chiral_opendir(const char *path) {
  WPATH(w, path);
  wchar_t pattern[PATH_MAX + 16];
  DIR *d;
  size_t len;
  DWORD attr;

  if(w_kind < 0) return NULL;
  if(!(d = calloc(1, sizeof(*d)))) {
    errno = ENOMEM;
    return NULL;
  }
  d->find = INVALID_HANDLE_VALUE;
  if(!w_kind) {
    d->root = 1;
    d->drives = (int)GetLogicalDrives();
    return d;
  }
  if((attr = GetFileAttributesW(w)) == INVALID_FILE_ATTRIBUTES) {
    fail_win32();
    free(d);
    return NULL;
  }
  if(!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
    errno = ENOTDIR;
    free(d);
    return NULL;
  }
  len = wcslen(w);
  if(len + 3 > sizeof(pattern) / sizeof(pattern[0])) {
    errno = ENAMETOOLONG;
    free(d);
    return NULL;
  }
  memcpy(pattern, w, len * sizeof(wchar_t));
  if(len && pattern[len - 1] != L'\\') pattern[len++] = L'\\';
  pattern[len++] = L'*';
  pattern[len] = 0;
  d->find = FindFirstFileExW(pattern, FindExInfoBasic, &d->data, FindExSearchNameMatch, NULL,
                             FIND_FIRST_EX_LARGE_FETCH);
  if(d->find == INVALID_HANDLE_VALUE) {
    if(GetLastError() == ERROR_FILE_NOT_FOUND) {
      d->done = 1;  /* an empty drive: not even "." */
      return d;
    }
    fail_win32();
    free(d);
    return NULL;
  }
  d->first = 1;
  return d;
}

struct dirent *
chiral_readdir(DIR *d) {
  if(d->root) {
    for(int i = 0; i < 26; i++) {
      if(!(d->drives & (1 << i))) continue;
      d->drives &= ~(1 << i);
      memset(&d->ent, 0, sizeof(d->ent));
      d->ent.d_name[0] = (char)('a' + i);
      d->ent.d_type = DT_DIR;
      return &d->ent;
    }
    return NULL;
  }
  if(d->done) return NULL;
  if(!d->first) {
    if(!FindNextFileW(d->find, &d->data)) {
      if(GetLastError() != ERROR_NO_MORE_FILES) fail_win32();
      d->done = 1;
      return NULL;
    }
  }
  d->first = 0;
  memset(&d->ent, 0, sizeof(d->ent));
  if(!WideCharToMultiByte(CP_UTF8, 0, d->data.cFileName, -1, d->ent.d_name, sizeof(d->ent.d_name), NULL, NULL)) {
    snprintf(d->ent.d_name, sizeof(d->ent.d_name), "?");
  }
  if((d->data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && is_link_tag(d->data.dwReserved0)) {
    d->ent.d_type = DT_LNK;
  } else {
    d->ent.d_type = (d->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
  }
  return &d->ent;
}

int
chiral_closedir(DIR *d) {
  if(!d) {
    errno = EBADF;
    return -1;
  }
  if(d->find != INVALID_HANDLE_VALUE) FindClose(d->find);
  free(d);
  return 0;
}

/* ---------- memory maps ---------- */

void *
chiral_mmap(void *addr, size_t len, int prot, int flags, int fd, long long offset) {
  HANDLE file, map;
  DWORD protect, access;
  void *p;

  (void)addr;
  if(flags & MAP_ANON) {
    p = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, (prot & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY);
    if(!p) {
      errno = ENOMEM;
      return MAP_FAILED;
    }
    return p;
  }
  if((file = fd_handle(fd)) == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return MAP_FAILED;
  }
  if(!(prot & PROT_WRITE)) {
    protect = PAGE_READONLY;
    access = FILE_MAP_READ;
  } else if(flags & MAP_PRIVATE) {
    protect = PAGE_WRITECOPY;
    access = FILE_MAP_COPY;
  } else {
    protect = PAGE_READWRITE;
    access = FILE_MAP_WRITE;
  }
  if(!(map = CreateFileMappingW(file, NULL, protect, 0, 0, NULL))) {
    fail_win32();
    return MAP_FAILED;
  }
  /* The view keeps the mapping, and the file, for as long as it lasts. */
  p = MapViewOfFile(map, access, (DWORD)((unsigned long long)offset >> 32), (DWORD)offset, len);
  CloseHandle(map);
  if(!p) {
    fail_win32();
    return MAP_FAILED;
  }
  return p;
}

int
chiral_munmap(void *addr, size_t len) {
  MEMORY_BASIC_INFORMATION info;

  (void)len;
  if(!VirtualQuery(addr, &info, sizeof(info))) return fail_win32();
  if(info.Type == MEM_MAPPED) return UnmapViewOfFile(addr) ? 0 : fail_win32();
  return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : fail_win32();
}

/* ---------- time ---------- */

time_t
chiral_timegm(struct tm *tm) {
  return _mkgmtime(tm);
}

/* ---------- sockets ---------- */

int
chiral_socket(int domain, int type, int protocol) {
  SOCKET s = socket(domain, type, protocol);

  if(s == INVALID_SOCKET) return fail_wsa();
  SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
  return (int)s;
}

int
chiral_accept(int fd, struct sockaddr *addr, socklen_t *len) {
  SOCKET s = accept((SOCKET)fd, addr, len);

  if(s == INVALID_SOCKET) return fail_wsa();
  SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
  return (int)s;
}

int
chiral_bind(int fd, const struct sockaddr *addr, socklen_t len) {
  return bind((SOCKET)fd, addr, len) ? fail_wsa() : 0;
}

int
chiral_listen(int fd, int backlog) {
  return listen((SOCKET)fd, backlog) ? fail_wsa() : 0;
}

int
chiral_connect(int fd, const struct sockaddr *addr, socklen_t len) {
  if(!connect((SOCKET)fd, addr, len)) return 0;
  /* A non-blocking connect still on its way. */
  if(WSAGetLastError() == WSAEWOULDBLOCK) {
    errno = EINPROGRESS;
    return -1;
  }
  return fail_wsa();
}

int
chiral_shutdown(int fd, int how) {
  return shutdown((SOCKET)fd, how) ? fail_wsa() : 0;
}

ssize_t
chiral_recv(int fd, void *buf, size_t len, int flags) {
  int n;

  if(len > INT_MAX) len = INT_MAX;
  n = recv((SOCKET)fd, (char *)buf, (int)len, flags);
  return n < 0 ? fail_wsa_io() : n;
}

ssize_t
chiral_send(int fd, const void *buf, size_t len, int flags) {
  int n;

  if(len > INT_MAX) len = INT_MAX;
  n = send((SOCKET)fd, (const char *)buf, (int)len, flags);
  return n < 0 ? fail_wsa_io() : n;
}

ssize_t
chiral_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen) {
  int n;

  if(len > INT_MAX) len = INT_MAX;
  n = recvfrom((SOCKET)fd, (char *)buf, (int)len, flags, from, fromlen);
  /* A datagram larger than the buffer: POSIX truncates it silently. */
  if(n < 0 && WSAGetLastError() == WSAEMSGSIZE) return (ssize_t)len;
  return n < 0 ? fail_wsa_io() : n;
}

ssize_t
chiral_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen) {
  int n;

  if(len > INT_MAX) len = INT_MAX;
  n = sendto((SOCKET)fd, (const char *)buf, (int)len, flags, to, tolen);
  return n < 0 ? fail_wsa_io() : n;
}

int
chiral_setsockopt(int fd, int level, int name, const void *val, socklen_t len) {
  if(level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO) && len == sizeof(struct timeval)) {
    /* Milliseconds in a DWORD on Windows, not a struct timeval; a timeval
       passed as is would make 30 seconds 30 milliseconds. 0 is "never" on
       both. */
    const struct timeval *tv = (const struct timeval *)val;
    long long ms = (long long)tv->tv_sec * 1000 + tv->tv_usec / 1000;
    DWORD dw;

    if(ms <= 0 && (tv->tv_sec || tv->tv_usec)) ms = 1;
    dw = ms > 0x7fffffff ? 0x7fffffff : (DWORD)ms;
    return setsockopt((SOCKET)fd, level, name, (const char *)&dw, sizeof(dw)) ? fail_wsa() : 0;
  }
  if(level == SOL_SOCKET && name == SO_REUSEADDR) {
    /* On Windows it lets another program take over a listening port. A
       server can already listen again at once after a restart without it;
       a UDP socket that shares its port with others still needs it. */
    int type = 0;
    int tlen = sizeof(type);
    getsockopt((SOCKET)fd, SOL_SOCKET, SO_TYPE, (char *)&type, &tlen);
    if(type == SOCK_STREAM) return 0;
  }
  return setsockopt((SOCKET)fd, level, name, (const char *)val, len) ? fail_wsa() : 0;
}

int
chiral_getsockopt(int fd, int level, int name, void *val, socklen_t *len) {
  if(getsockopt((SOCKET)fd, level, name, (char *)val, len)) return fail_wsa();
  /* SO_ERROR carries a Winsock code; the caller hands it on as errno. */
  if(level == SOL_SOCKET && name == SO_ERROR && *len >= (socklen_t)sizeof(int) && *(int *)val) {
    *(int *)val = errno_from_wsa(*(int *)val);
  }
  return 0;
}

int
chiral_getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
  return getsockname((SOCKET)fd, addr, len) ? fail_wsa() : 0;
}

int
chiral_getpeername(int fd, struct sockaddr *addr, socklen_t *len) {
  return getpeername((SOCKET)fd, addr, len) ? fail_wsa() : 0;
}

int
chiral_poll(struct pollfd *fds, nfds_t n, int timeout_ms) {
  WSAPOLLFD stack[16], *w = stack;
  ULONG count = 0;
  int r;

  if(n > 16 && !(w = malloc(n * sizeof(*w)))) {
    errno = ENOMEM;
    return -1;
  }
  /* WSAPoll refuses the whole call for a flag it doesn't take (POLLPRI)
     and for a negative descriptor, which POSIX simply skips. */
  for(nfds_t i = 0; i < n; i++) {
    fds[i].revents = 0;
    if(fds[i].fd < 0) continue;
    w[count].fd = (SOCKET)fds[i].fd;
    w[count].events = fds[i].events & (POLLIN | POLLOUT);
    w[count].revents = 0;
    count++;
  }
  if(!count) {
    if(w != stack) free(w);
    Sleep(timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
    return 0;
  }
  r = WSAPoll(w, count, timeout_ms);
  if(r < 0) {
    fail_wsa();
  } else {
    count = 0;
    for(nfds_t i = 0; i < n; i++) {
      if(fds[i].fd < 0) continue;
      fds[i].revents = w[count++].revents;
    }
  }
  if(w != stack) free(w);
  return r;
}

int
chiral_fcntl(int fd, int cmd, ...) {
  va_list ap;
  int arg;

  va_start(ap, cmd);
  arg = va_arg(ap, int);
  va_end(ap);
  switch(cmd) {
  case F_GETFL:
    /* Every socket starts blocking, and the only flag asked about is
       O_NONBLOCK, set and then cleared again. */
    return 0;
  case F_SETFL: {
    u_long on = (arg & O_NONBLOCK) ? 1 : 0;
    if(!is_socket(fd)) return 0;
    return ioctlsocket((SOCKET)fd, FIONBIO, &on) ? fail_wsa() : 0;
  }
  default:
    return 0;
  }
}

chiral_sighandler_t
chiral_signal(int sig, chiral_sighandler_t handler) {
  if(sig == SIGPIPE) return SIG_DFL;
  return signal(sig, handler);
}
