#pragma once

/* Windows: the POSIX the shared code is written against, on Winsock and
   Win32. The Windows build forces this into every file (-include), and
   platform/sdl/win32/include stands in for the headers MinGW lacks
   (<sys/socket.h>, <poll.h>, <dirent.h>...), so engine/, web/ and app/
   build unchanged.

   Three things it decides for the whole program:

   - Paths are POSIX paths over the Windows drives, as MSYS2 writes them:
     "/c/Users/Simo" is C:\Users\Simo, and "/" is a folder holding one
     entry per drive. Every "is this absolute", "go up one level" and
     "is this inside the shared folder" in the shared code then works as
     it does elsewhere, and "/" is the whole computer.
   - Names are UTF-8, converted to UTF-16 at the Win32 call, so any file
     name works whatever the system's code page.
   - Sockets are ints, as everywhere else. A SOCKET fits in one (Windows
     keeps them below 2^24), and close() tells a socket from a file.

   Socket calls set errno, not WSAGetLastError(), so the shared code's
   EINTR, EAGAIN and strerror() mean what they say. */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
/* localtime_r() and gmtime_r(), which MinGW's <time.h> only declares then. */
#ifndef _POSIX_THREAD_SAFE_FUNCTIONS
#define _POSIX_THREAD_SAFE_FUNCTIONS 200112L
#endif

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utime.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#ifndef __cplusplus
/* Without RPC and the rest, whose names (handle_t...) the shared code uses
   for its own things. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* MinGW's is Windows' MAX_PATH, 260. The shared code sizes buffers with it,
   and the long-path form (\\?\) goes much further. */
#undef PATH_MAX
#define PATH_MAX 4096

/* ---------- paths ---------- */

/* A POSIX path ("/c/Users") as a Win32 one (L"C:\\Users"); 0 when it can't
   be one ("/" itself, or too long). */
int chiral_win_path(const char *path, wchar_t *out, size_t count);
/* A Win32 path (L"C:\\Users") as a POSIX one ("/c/Users"). */
void chiral_posix_path(const wchar_t *path, char *out, size_t size);
/* The same for a UTF-8 Win32 path, "C:\\Users" -> "/c/Users". Anything
   already POSIX is copied. */
void chiral_posix_path_utf8(const char *path, char *out, size_t size);

/* ---------- files ---------- */

#ifndef S_IFLNK
#define S_IFLNK 0xA000
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x40000000
#endif

/* Sizes and times in 64 bits, whatever MinGW's own struct stat holds. */
struct chiral_stat {
  unsigned long long st_dev, st_ino;
  unsigned st_mode, st_nlink, st_uid, st_gid;
  long long st_size;
  time_t st_atime, st_mtime, st_ctime;
};

int chiral_stat(const char *path, struct chiral_stat *st);
int chiral_lstat(const char *path, struct chiral_stat *st);
int chiral_fstat(int fd, struct chiral_stat *st);
int chiral_open(const char *path, int flags, ...);
int chiral_close(int fd);
long long chiral_lseek(int fd, long long offset, int whence);
int chiral_ftruncate(int fd, long long size);
int chiral_truncate(const char *path, long long size);
int chiral_fsync(int fd);
ssize_t chiral_pread(int fd, void *buf, size_t size, long long offset);
ssize_t chiral_pwrite(int fd, const void *buf, size_t size, long long offset);
int chiral_mkdir(const char *path, int mode);
int chiral_rmdir(const char *path);
int chiral_unlink(const char *path);
int chiral_rename(const char *from, const char *to);
int chiral_chmod(const char *path, int mode);
int chiral_fchmod(int fd, int mode);
int chiral_access(const char *path, int mode);
char *chiral_realpath(const char *path, char *resolved);
ssize_t chiral_readlink(const char *path, char *buf, size_t size);
int chiral_utimes(const char *path, const struct timeval times[2]);
FILE *chiral_fopen(const char *path, const char *mode);
char *chiral_getenv(const char *name);

int chiral_utime(const char *path, const struct utimbuf *times);

/* stat by name, not stat(): the struct and the call share it. The rest
   are function-like, so a member or a C++ method called open() or read()
   keeps its name. */
#undef stat
#undef fstat
#undef lstat
#define stat chiral_stat
#define fstat(...) chiral_fstat(__VA_ARGS__)
#define lstat(...) chiral_lstat(__VA_ARGS__)
#define open(...) chiral_open(__VA_ARGS__)
#define close(...) chiral_close(__VA_ARGS__)
#define lseek(...) chiral_lseek(__VA_ARGS__)
#define ftruncate(...) chiral_ftruncate(__VA_ARGS__)
#define truncate(...) chiral_truncate(__VA_ARGS__)
#define fsync(...) chiral_fsync(__VA_ARGS__)
#define pread(...) chiral_pread(__VA_ARGS__)
#define pwrite(...) chiral_pwrite(__VA_ARGS__)
#define mkdir(...) chiral_mkdir(__VA_ARGS__)
#define rmdir(...) chiral_rmdir(__VA_ARGS__)
#define unlink(...) chiral_unlink(__VA_ARGS__)
#define rename(...) chiral_rename(__VA_ARGS__)
#define chmod(...) chiral_chmod(__VA_ARGS__)
#define fchmod(...) chiral_fchmod(__VA_ARGS__)
#define access(...) chiral_access(__VA_ARGS__)
#define realpath(...) chiral_realpath(__VA_ARGS__)
#define readlink(...) chiral_readlink(__VA_ARGS__)
#define utimes(...) chiral_utimes(__VA_ARGS__)
#define utime(...) chiral_utime(__VA_ARGS__)
#define fopen(...) chiral_fopen(__VA_ARGS__)
#define getenv(...) chiral_getenv(__VA_ARGS__)

/* ---------- memory maps (<sys/mman.h>) ---------- */

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANON 0x20
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED ((void *)-1)
void *chiral_mmap(void *addr, size_t len, int prot, int flags, int fd, long long offset);
int chiral_munmap(void *addr, size_t len);
#define mmap(...) chiral_mmap(__VA_ARGS__)
#define munmap(...) chiral_munmap(__VA_ARGS__)

/* ---------- time ---------- */

time_t chiral_timegm(struct tm *tm);
#define timegm(...) chiral_timegm(__VA_ARGS__)

/* ---------- sockets ---------- */

#ifndef __cplusplus

#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
/* Windows never raises it: a write to a closed socket fails with
   WSAECONNRESET. signal() is told to ignore it and does nothing. */
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
typedef unsigned long nfds_t;

int chiral_socket(int domain, int type, int protocol);
int chiral_accept(int fd, struct sockaddr *addr, socklen_t *len);
int chiral_bind(int fd, const struct sockaddr *addr, socklen_t len);
int chiral_listen(int fd, int backlog);
int chiral_connect(int fd, const struct sockaddr *addr, socklen_t len);
int chiral_shutdown(int fd, int how);
ssize_t chiral_recv(int fd, void *buf, size_t len, int flags);
ssize_t chiral_send(int fd, const void *buf, size_t len, int flags);
ssize_t chiral_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen);
ssize_t chiral_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen);
int chiral_setsockopt(int fd, int level, int name, const void *val, socklen_t len);
int chiral_getsockopt(int fd, int level, int name, void *val, socklen_t *len);
int chiral_getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int chiral_getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int chiral_poll(struct pollfd *fds, nfds_t n, int timeout_ms);
int chiral_fcntl(int fd, int cmd, ...);
ssize_t chiral_read(int fd, void *buf, size_t len);
ssize_t chiral_write(int fd, const void *buf, size_t len);
typedef void (*chiral_sighandler_t)(int);
chiral_sighandler_t chiral_signal(int sig, chiral_sighandler_t handler);

#define socket(...) chiral_socket(__VA_ARGS__)
#define accept(...) chiral_accept(__VA_ARGS__)
#define bind(...) chiral_bind(__VA_ARGS__)
#define listen(...) chiral_listen(__VA_ARGS__)
#define connect(...) chiral_connect(__VA_ARGS__)
#define shutdown(...) chiral_shutdown(__VA_ARGS__)
#define recv(...) chiral_recv(__VA_ARGS__)
#define send(...) chiral_send(__VA_ARGS__)
#define recvfrom(...) chiral_recvfrom(__VA_ARGS__)
#define sendto(...) chiral_sendto(__VA_ARGS__)
#define setsockopt(...) chiral_setsockopt(__VA_ARGS__)
#define getsockopt(...) chiral_getsockopt(__VA_ARGS__)
#define getsockname(...) chiral_getsockname(__VA_ARGS__)
#define getpeername(...) chiral_getpeername(__VA_ARGS__)
#define poll(...) chiral_poll(__VA_ARGS__)
#define fcntl(...) chiral_fcntl(__VA_ARGS__)
#define read(...) chiral_read(__VA_ARGS__)
#define write(...) chiral_write(__VA_ARGS__)
#define signal(...) chiral_signal(__VA_ARGS__)

#endif  /* !__cplusplus */

#ifdef __cplusplus
}
#endif
