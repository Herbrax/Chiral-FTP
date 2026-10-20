/* Runtime pieces for the native title.

   Every Chiral build that imported a function the working native apps (the
   boilerplate Hello World, ProsperoRadio) never import was refused at launch
   ("can't start the game or app"). So eboot.bin keeps to their import set:
   the functions below are defined here, on top of imports those apps use,
   instead of being imported or pulled from the SDK's static libc (whose
   arc4random reads /dev/urandom through _open/_read/_close). */

#ifdef __PROSPERO__

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

extern int sceKernelUsleep(unsigned int microseconds);

/* libc++abi's exception storage needs it. */
enum { ONCE_NEVER, ONCE_RUNNING, ONCE_DONE };

int
pthread_once(pthread_once_t *once, void (*init)(void)) {
  int state = __atomic_load_n(&once->state, __ATOMIC_ACQUIRE);

  while(state != ONCE_DONE) {
    int expected = ONCE_NEVER;
    if(__atomic_compare_exchange_n(&once->state, &expected, ONCE_RUNNING, 0, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE)) {
      init();
      __atomic_store_n(&once->state, ONCE_DONE, __ATOMIC_RELEASE);
      return 0;
    }
    sceKernelUsleep(100);  // another thread is running init
    state = __atomic_load_n(&once->state, __ATOMIC_ACQUIRE);
  }
  return 0;
}

/* SSH randomness (mbedtls_hardware_poll) from the CPU's RDRAND. */
static int
rdrand64(uint64_t *out) {
  unsigned char ok;
  for(int tries = 0; tries < 10; tries++) {
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok));
    if(ok) return 1;
  }
  return 0;
}

/* mbedTLS is built with MBEDTLS_ENTROPY_HARDWARE_ALT, so it asks us for
   randomness rather than reading /dev/urandom -- which this title has no
   way to open anyway. It lived in engine/sftp.c behind #ifdef __SCE__
   before the tree was unified; it belongs here, beside the RDRAND it uses. */
void arc4random_buf(void *buf, size_t len);
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);

int
mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
  (void)data;
  arc4random_buf(output, len);
  if(olen) *olen = len;
  return 0;
}

void
arc4random_buf(void *buf, size_t len) {
  unsigned char *p = buf;
  uint64_t v, last = 0;

  while(len) {
    /* A stuck RDRAND (all ones, or repeats) falls back to the cycle counter,
       which is weak but never constant. */
    if(!rdrand64(&v) || v == ~(uint64_t)0 || v == last) {
      uint32_t lo, hi;
      __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
      v ^= ((uint64_t)hi << 32 | lo) * 0x9e3779b97f4a7c15ull;
    }
    last = v;
    size_t n = len < sizeof(v) ? len : sizeof(v);
    memcpy(p, &v, n);
    p += n;
    len -= n;
  }
}

/* lstat (linked with --wrap=lstat): the real one, so links aren't followed;
   the sandbox refuses it (EPERM), and then stat is the best there is. */
int __real_lstat(const char *path, struct stat *st);

int
__wrap_lstat(const char *path, struct stat *st) {
  if(!__real_lstat(path, st)) return 0;
  return errno == EPERM || errno == EACCES ? stat(path, st) : -1;
}

int
utimes(const char *path, const struct timeval times[2]) {
  struct utimbuf t = {times[0].tv_sec, times[1].tv_sec};
  return utime(path, &t);
}

unsigned int
sleep(unsigned int seconds) {
  while(seconds--) sceKernelUsleep(1000000);
  return 0;
}

/* Times show in UTC: the console's local-time conversion isn't among the
   known-good imports. */
struct tm *
localtime_r(const time_t *t, struct tm *out) {
  return gmtime_r(t, out);
}

double
difftime(time_t end, time_t start) {
  return (double)(end - start);
}

/* Folders: the console's opendir answers "operation not permitted" in a
   title, so list them the way ProsperoEden does (native_directory.h), with
   open(O_DIRECTORY) and sceKernelGetdents into a 64 KiB buffer: mounted PS5
   filesystems refuse smaller directory reads. A failed read sets errno, so a
   caller can tell an error from the end of the folder. */
extern int sceKernelGetdents(int fd, char *buf, int nbytes);

struct _dirdesc {
  int fd;
  int pos;
  int len;
  char buf[65536];
};

DIR *
opendir(const char *path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY);
  DIR *dir;

  if(fd < 0) return NULL;
  if(!(dir = malloc(sizeof(*dir)))) {
    close(fd);
    return NULL;
  }
  dir->fd = fd;
  dir->pos = dir->len = 0;
  return dir;
}

struct dirent *
readdir(DIR *dir) {
  for(;;) {
    struct dirent *ent;

    if(dir->pos >= dir->len) {
      int n = sceKernelGetdents(dir->fd, dir->buf, sizeof(dir->buf));
      dir->pos = 0;
      dir->len = n > 0 && n <= (int)sizeof(dir->buf) ? n : 0;
      if(n < 0 || n > (int)sizeof(dir->buf)) {
        if(!errno) errno = EIO;
        return NULL;
      }
      if(!n) return NULL;  // end of the folder
    }
    ent = (struct dirent *)(dir->buf + dir->pos);
    if(ent->d_reclen <= offsetof(struct dirent, d_name) || ent->d_reclen > dir->len - dir->pos) {
      dir->len = 0;
      errno = EIO;  // a malformed record
      return NULL;
    }
    dir->pos += ent->d_reclen;
    /* No filtering on d_fileno: a filesystem may leave it 0. A leftover
       deleted entry fails stat() in the caller and is skipped there. */
    if(ent->d_namlen) return ent;
  }
}

int
closedir(DIR *dir) {
  int ret = close(dir->fd);
  free(dir);
  return ret;
}

/* Sockets: the console's fcntl refuses sockets in a title, and setting
   O_NONBLOCK through it doesn't take effect anyway (ps5-native-app-boilerplate,
   docs/CURL.md). Linked with --wrap=fcntl, so libssh2's calls land here too:
   on a socket, non-blocking mode goes through the console's SO_NBIO option.
   The real fcntl can't report that mode, so it's tracked per descriptor and
   reset when socket() or accept() hands the descriptor out again. */
#define SO_NBIO 0x1200
#define MAX_TRACKED_FD 4096

int __real_fcntl(int fd, int cmd, ...);
int __real_socket(int domain, int type, int protocol);
int __real_accept(int fd, struct sockaddr *addr, socklen_t *len);

static unsigned char g_nonblocking[MAX_TRACKED_FD];

static void
track(int fd, int on) {
  if(fd >= 0 && fd < MAX_TRACKED_FD) __atomic_store_n(&g_nonblocking[fd], (unsigned char)on, __ATOMIC_RELAXED);
}

int
__wrap_socket(int domain, int type, int protocol) {
  int fd = __real_socket(domain, type, protocol);
  track(fd, 0);
  return fd;
}

int
__wrap_accept(int fd, struct sockaddr *addr, socklen_t *len) {
  int client = __real_accept(fd, addr, len);
  track(client, 0);
  return client;
}

static int
is_socket(int fd) {
  int type;
  socklen_t len = sizeof(type);
  return getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0;
}

int
__wrap_fcntl(int fd, int cmd, ...) {
  va_list ap;
  long arg;
  int ret;

  va_start(ap, cmd);
  arg = va_arg(ap, long);
  va_end(ap);
  if(fd < 0 || fd >= MAX_TRACKED_FD || !is_socket(fd)) return __real_fcntl(fd, cmd, arg);
  switch(cmd) {
  case F_GETFL:
    ret = __real_fcntl(fd, F_GETFL, 0);
    if(ret < 0) ret = O_RDWR;
    ret &= ~O_NONBLOCK;
    return __atomic_load_n(&g_nonblocking[fd], __ATOMIC_RELAXED) ? ret | O_NONBLOCK : ret;
  case F_SETFL: {
    int on = ((int)arg & O_NONBLOCK) != 0;
    if(setsockopt(fd, SOL_SOCKET, SO_NBIO, &on, sizeof(on))) return -1;
    track(fd, on);
    return 0;
  }
  case F_GETFD:
  case F_SETFD:
    /* Close-on-exec: a title never runs another program. */
    ret = __real_fcntl(fd, cmd, (int)arg);
    return ret < 0 ? 0 : ret;
  default:
    return __real_fcntl(fd, cmd, arg);
  }
}

/* libmicrohttpd wakes its threads through a pipe made with pipe2, which the
   SDK doesn't export: pipe plus the flags. */
int
pipe2(int fds[2], int flags) {
  if(pipe(fds)) return -1;
  for(int i = 0; i < 2; i++) {
    if(flags & O_CLOEXEC) (void)__real_fcntl(fds[i], F_SETFD, FD_CLOEXEC);  // a title never execs
    if(flags & O_NONBLOCK && __real_fcntl(fds[i], F_SETFL, O_NONBLOCK)) {
      close(fds[0]);
      close(fds[1]);
      return -1;
    }
  }
  return 0;
}

/* stdio pieces mbedTLS and libssh2 use, on fseeko/ftello. */
int
fseek(FILE *f, long offset, int whence) {
  return fseeko(f, offset, whence);
}

long
ftell(FILE *f) {
  return (long)ftello(f);
}

void
rewind(FILE *f) {
  fseeko(f, 0, SEEK_SET);
}

/* mbedTLS only turns buffering off for key files; keeping it is harmless. */
void
setbuf(FILE *f, char *buf) {
  (void)f;
  (void)buf;
}

#endif
