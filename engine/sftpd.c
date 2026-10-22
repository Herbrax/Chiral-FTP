/* SFTP server (sftpd.h), on libssh. Built only where the platform has it
   (CHIRAL_HAS_SFTPD), which keeps libssh off the consoles entirely.

   Shaped like the FTP server next door: the same login and shared folder,
   one thread per connection, blocking sockets that sftpd_stop() shuts down
   so everything ends promptly, the same counters and activity list. Every
   path a client names stays inside the shared folder -- ".." stops at its
   top, and a symlink that leads out of it is refused.

   libssh 0.11 deprecated the message-by-message server API used here in
   favour of callbacks. The callbacks only come with libssh's own file
   handlers, which serve the whole disk with no way to confine them, so the
   messages are handled here instead and the deprecation warnings are
   silenced for this file. */

#define WITH_SERVER
#include "sftpd.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/sftp.h>

#include "config.h"
#include "localfs.h"

/* The UI's translation lookup (app/i18n.cpp), as in ftpd.c. */
const char *tr(const char *english);

#define MAX_LOGIN_FAILURES 3
#define RECENT_MAX 40
#define READ_MAX (64 * 1024)  // the most one READ returns; clients ask again
#define READDIR_BATCH 64
#define HOST_KEY_NAME "sftpd_host_key"
#define HOST_KEY_ECDSA_NAME "sftpd_host_key_ecdsa"

/* An open file or folder, as the client knows it by its handle. */
typedef struct handle {
  struct handle *next;
  DIR *dir;  // a folder being listed, or
  int fd;    // an open file
  int wrote;
  long long bytes;
  char real[PATH_MAX];
  char virt[PATH_MAX];
} handle_t;

typedef struct sclient {
  int fd;  // the connection, owned by the session; kept for sftpd_stop()
  int slot;
  struct sockaddr_in peer;
  char ip[INET_ADDRSTRLEN];
  char host[64];
  char who[128];
  int logged_in;
  handle_t *handles;
  char *buf;
} sclient_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static ftpd_config_t g_cfg;
static volatile int g_stop;
static int g_running;
static ssh_bind g_bind;
static sclient_t *g_clients[FTPD_MAX_CLIENTS];
static ftpd_stats_t g_stats;
static long long g_rate_bytes;
static struct timeval g_rate_start;
static char g_recent[RECENT_MAX][160];
static int g_recent_head, g_recent_count;
static char g_fingerprint[128];
static struct {
  char user[64], pass[64], ip[INET_ADDRSTRLEN];
} g_guests[FTPD_MAX_GUESTS];
static int g_guest_next;

void
sftpd_add_guest(const char *user, const char *pass, const char *ip) {
  pthread_mutex_lock(&g_lock);
  snprintf(g_guests[g_guest_next].user, sizeof(g_guests[0].user), "%s", user);
  snprintf(g_guests[g_guest_next].pass, sizeof(g_guests[0].pass), "%s", pass);
  snprintf(g_guests[g_guest_next].ip, sizeof(g_guests[0].ip), "%s", ip);
  g_guest_next = (g_guest_next + 1) % FTPD_MAX_GUESTS;
  pthread_mutex_unlock(&g_lock);
}

/* ---------- bookkeeping, as ftpd.c keeps it ---------- */

static void
event(const char *fmt, ...) {
  char line[160];
  va_list ap;
  time_t t = time(NULL);
  struct tm tm;

  localtime_r(&t, &tm);
  strftime(line, sizeof(line), "%H:%M:%S  ", &tm);
  va_start(ap, fmt);
  vsnprintf(line + strlen(line), sizeof(line) - strlen(line), fmt, ap);
  va_end(ap);
  pthread_mutex_lock(&g_lock);
  snprintf(g_recent[(g_recent_head + g_recent_count) % RECENT_MAX], 160, "%s", line);
  if(g_recent_count < RECENT_MAX) g_recent_count++;
  else g_recent_head = (g_recent_head + 1) % RECENT_MAX;
  pthread_mutex_unlock(&g_lock);
}

static void
count_bytes(int up, long long n) {
  pthread_mutex_lock(&g_lock);
  if(up) g_stats.bytes_up += n;
  else g_stats.bytes_down += n;
  g_rate_bytes += n;
  pthread_mutex_unlock(&g_lock);
}

static void
adjust(int *field, int delta) {
  pthread_mutex_lock(&g_lock);
  *field += delta;
  pthread_mutex_unlock(&g_lock);
}

void
sftpd_set_config(const ftpd_config_t *c) {
  pthread_mutex_lock(&g_lock);
  unsigned short port = g_cfg.port;
  g_cfg = *c;
  if(g_running) g_cfg.port = port;  // the listener keeps its port until restarted
  pthread_mutex_unlock(&g_lock);
}

void
sftpd_get_stats(ftpd_stats_t *out) {
  struct timeval now;
  double secs;

  pthread_mutex_lock(&g_lock);
  gettimeofday(&now, NULL);
  secs = (double)(now.tv_sec - g_rate_start.tv_sec) + (now.tv_usec - g_rate_start.tv_usec) / 1e6;
  if(secs >= 1.0) {
    g_stats.speed = g_rate_bytes / secs;
    g_rate_bytes = 0;
    g_rate_start = now;
  }
  *out = g_stats;
  pthread_mutex_unlock(&g_lock);
}

int
sftpd_recent(char lines[][160], int max) {
  int n;

  pthread_mutex_lock(&g_lock);
  n = g_recent_count < max ? g_recent_count : max;
  for(int i = 0; i < n; i++) {
    int idx = (g_recent_head + g_recent_count - n + i) % RECENT_MAX;
    memcpy(lines[i], g_recent[idx], 160);
  }
  pthread_mutex_unlock(&g_lock);
  return n;
}

int
sftpd_running(void) {
  int r;

  pthread_mutex_lock(&g_lock);
  r = g_running;
  pthread_mutex_unlock(&g_lock);
  return r;
}

void
sftpd_fingerprint(char *buf, size_t size) {
  pthread_mutex_lock(&g_lock);
  snprintf(buf, size, "%s", g_fingerprint);
  pthread_mutex_unlock(&g_lock);
}

/* ---------- the server's key ---------- */

/* Made once and kept in the data folder, so an SFTP app that has trusted
   this computer goes on trusting it after a restart.

   Two of them. Ed25519 is the one apps choose (OpenSSH, FileZilla, phones)
   and the one the server screen shows. ECDSA P-256 is for chiral-ftp itself:
   its client is libssh2 on Mbed TLS, on every platform, and that has no
   Ed25519 -- without a second key, quick connect between two copies
   couldn't agree on one. Each client takes the first of its own list the
   server has, so neither changes what the other sees. */
static int
load_host_key(const char *name, enum ssh_keytypes_e type, ssh_key *out) {
  char path[600];

  data_path(path, sizeof(path), name);
  if(ssh_pki_import_privkey_file(path, NULL, NULL, NULL, out) == SSH_OK) return 0;
  if(ssh_pki_generate_key(type, 0, out) != SSH_OK) return -1;
  /* A key that can't be saved still works; apps just see a new one next time. */
  if(ssh_pki_export_privkey_file(*out, NULL, NULL, NULL, path) == SSH_OK) chmod(path, 0600);
  return 0;
}

static void
remember_fingerprint(ssh_key key) {
  unsigned char *hash = NULL;
  size_t len = 0;
  char *text;

  if(ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &len)) return;
  if((text = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, len))) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_fingerprint, sizeof(g_fingerprint), "%s", text);
    pthread_mutex_unlock(&g_lock);
    ssh_string_free_char(text);
  }
  ssh_clean_pubkey_hash(&hash);
}

/* ---------- paths ---------- */

/* "/a/../b//c/." to "/b/c". Always absolute, and ".." never climbs above
   "/". A relative path is taken from "/", which is where a client starts. */
static void
normalize(const char *in, char *out, size_t size) {
  char buf[PATH_MAX];
  size_t len = 0;
  const char *p = in ? in : "";

  buf[0] = 0;
  while(*p) {
    const char *s;
    size_t n;

    while(*p == '/') p++;
    s = p;
    while(*p && *p != '/') p++;
    n = (size_t)(p - s);
    if(!n || (n == 1 && s[0] == '.')) continue;
    if(n == 2 && s[0] == '.' && s[1] == '.') {
      while(len > 0 && buf[len - 1] != '/') len--;
      if(len > 0) len--;
      buf[len] = 0;
      continue;
    }
    if(len + 1 + n >= sizeof(buf)) break;
    buf[len++] = '/';
    memcpy(buf + len, s, n);
    len += n;
    buf[len] = 0;
  }
  snprintf(out, size, "%s", len ? buf : "/");
}

/* The real path for a client's one, written to real, with its normalized
   form in virt. -1 (errno EACCES) when it would lead out of the shared
   folder. follow: judge by where the path itself leads, for opening,
   listing and stat; otherwise by the folder it is in, so that a symlink
   can be removed or renamed without being followed. */
static int
map_path(const char *client, char virt[PATH_MAX], char real[PATH_MAX], int follow) {
  char root[PATH_MAX], rroot[PATH_MAX], probe[PATH_MAX], resolved[PATH_MAX];
  size_t n;

  normalize(client, virt, PATH_MAX);
  pthread_mutex_lock(&g_lock);
  snprintf(root, sizeof(root), "%s", g_cfg.root);
  pthread_mutex_unlock(&g_lock);
  n = strlen(root);
  while(n > 1 && root[n - 1] == '/') root[--n] = 0;
  if(!strcmp(root, "/")) snprintf(real, PATH_MAX, "%s", virt);
  else snprintf(real, PATH_MAX, "%s%s", root, strcmp(virt, "/") ? virt : "");
  if(localfs_private(real)) {  // the app's own settings and keys, whatever is shared
    errno = EACCES;
    return -1;
  }

  if(!realpath(root, rroot)) return -1;
  if(!strcmp(rroot, "/")) return 0;  // the whole disk: nowhere to leave
  if(!strcmp(virt, "/")) return 0;   // the shared folder itself

  if(!follow || !realpath(real, resolved)) {
    char *slash;
    snprintf(probe, sizeof(probe), "%s", real);
    slash = strrchr(probe, '/');
    if(slash == probe) probe[1] = 0;
    else if(slash) *slash = 0;
    if(!realpath(probe, resolved)) return -1;  // no such folder: errno says so
  }
  n = strlen(rroot);
  if(!strcmp(resolved, rroot) || (!strncmp(resolved, rroot, n) && resolved[n] == '/')) return 0;
  errno = EACCES;
  return -1;
}

/* ---------- replies ---------- */

static int
reply_errno(sftp_client_message m, int err) {
  uint32_t code = SSH_FX_FAILURE;

  if(err == ENOENT || err == ENOTDIR) code = SSH_FX_NO_SUCH_FILE;
  else if(err == EACCES || err == EPERM || err == EROFS) code = SSH_FX_PERMISSION_DENIED;
  return sftp_reply_status(m, code, strerror(err));
}

static void
fill_attr(const struct stat *st, struct sftp_attributes_struct *a) {
  memset(a, 0, sizeof(*a));
  a->flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_UIDGID | SSH_FILEXFER_ATTR_PERMISSIONS |
             SSH_FILEXFER_ATTR_ACMODTIME;
  a->size = (uint64_t)st->st_size;
  a->uid = st->st_uid;
  a->gid = st->st_gid;
  a->permissions = st->st_mode;
  a->atime = (uint32_t)st->st_atime;
  a->atime64 = (uint64_t)st->st_atime;
  a->mtime = (uint32_t)st->st_mtime;
  a->mtime64 = (uint64_t)st->st_mtime;
  a->type = S_ISDIR(st->st_mode)   ? SSH_FILEXFER_TYPE_DIRECTORY
            : S_ISLNK(st->st_mode) ? SSH_FILEXFER_TYPE_SYMLINK
            : S_ISREG(st->st_mode) ? SSH_FILEXFER_TYPE_REGULAR
                                   : SSH_FILEXFER_TYPE_SPECIAL;
}

/* What "ls -l" would print: some apps show this line rather than the
   attributes. */
static void
long_name(const char *name, const struct stat *st, char *out, size_t size) {
  char perm[11] = "----------", date[32];
  const char *bits = "rwxrwxrwx";
  struct tm tm;
  time_t t = st->st_mtime;

  perm[0] = S_ISDIR(st->st_mode) ? 'd' : S_ISLNK(st->st_mode) ? 'l' : '-';
  for(int i = 0; i < 9; i++) if(st->st_mode & (0400 >> i)) perm[i + 1] = bits[i];
  localtime_r(&t, &tm);
  strftime(date, sizeof(date), time(NULL) - t < 180L * 24 * 3600 ? "%b %e %H:%M" : "%b %e  %Y", &tm);
  snprintf(out, size, "%s %3u %-8u %-8u %10lld %s %s", perm, (unsigned)st->st_nlink, (unsigned)st->st_uid,
           (unsigned)st->st_gid, (long long)st->st_size, date, name);
}

/* ---------- handles ---------- */

static handle_t *
find_handle(sclient_t *c, sftp_session sftp, sftp_client_message m) {
  handle_t *h = m->handle ? sftp_handle(sftp, m->handle) : NULL;
  for(handle_t *x = c->handles; x; x = x->next) if(x == h) return h;
  return NULL;  // not one of ours
}

static void
close_handle(sclient_t *c, sftp_session sftp, handle_t *h) {
  for(handle_t **p = &c->handles; *p; p = &(*p)->next) {
    if(*p == h) {
      *p = h->next;
      break;
    }
  }
  if(h->dir) {
    closedir(h->dir);
  } else {
    close(h->fd);
    adjust(&g_stats.transfers, -1);
    if(h->bytes > 0) {
      if(h->wrote) event(tr("%s uploaded %s (%lld bytes)"), c->who, h->virt, h->bytes);
      else event(tr("%s downloaded %s (%lld bytes)"), c->who, h->virt, h->bytes);
    }
  }
  if(sftp) sftp_handle_remove(sftp, h);
  free(h);
}

static void
reply_handle(sclient_t *c, sftp_session sftp, sftp_client_message m, handle_t *h) {
  ssh_string id = sftp_handle_alloc(sftp, h);

  if(!id) {
    if(h->dir) closedir(h->dir);
    else close(h->fd);
    free(h);
    sftp_reply_status(m, SSH_FX_FAILURE, "Too many open files");
    return;
  }
  h->next = c->handles;
  c->handles = h;
  if(!h->dir) adjust(&g_stats.transfers, 1);
  sftp_reply_handle(m, id);
  ssh_string_free(id);
}

/* ---------- the SFTP requests ---------- */

static void
on_open(sclient_t *c, sftp_session sftp, sftp_client_message m) {
  char virt[PATH_MAX], real[PATH_MAX];
  const uint32_t f = m->flags;
  int flags, mode = 0644, fd;
  struct stat st;
  handle_t *h;

  if(map_path(m->filename, virt, real, 1)) {
    reply_errno(m, errno);
    return;
  }
  flags = (f & SSH_FXF_READ) && (f & SSH_FXF_WRITE) ? O_RDWR : (f & SSH_FXF_WRITE) ? O_WRONLY : O_RDONLY;
  if(f & SSH_FXF_CREAT) flags |= O_CREAT;
  if(f & SSH_FXF_TRUNC) flags |= O_TRUNC;
  if(f & SSH_FXF_EXCL) flags |= O_EXCL;
  if(f & SSH_FXF_APPEND) flags |= O_APPEND;
  if(m->attr && (m->attr->flags & SSH_FILEXFER_ATTR_PERMISSIONS)) mode = (int)(m->attr->permissions & 07777);
  if((fd = open(real, flags, mode)) < 0) {
    reply_errno(m, errno);
    return;
  }
  if(!fstat(fd, &st) && S_ISDIR(st.st_mode)) {
    close(fd);
    sftp_reply_status(m, SSH_FX_FAILURE, "Is a folder");
    return;
  }
  if(!(h = calloc(1, sizeof(*h)))) {
    close(fd);
    sftp_reply_status(m, SSH_FX_FAILURE, "Out of memory");
    return;
  }
  h->fd = fd;
  h->wrote = (f & SSH_FXF_WRITE) != 0;
  snprintf(h->real, sizeof(h->real), "%s", real);
  snprintf(h->virt, sizeof(h->virt), "%s", virt);
  reply_handle(c, sftp, m, h);
}

static void
on_opendir(sclient_t *c, sftp_session sftp, sftp_client_message m) {
  char virt[PATH_MAX], real[PATH_MAX];
  DIR *d;
  handle_t *h;

  if(map_path(m->filename, virt, real, 1) || !(d = opendir(real))) {
    reply_errno(m, errno);
    return;
  }
  if(!(h = calloc(1, sizeof(*h)))) {
    closedir(d);
    sftp_reply_status(m, SSH_FX_FAILURE, "Out of memory");
    return;
  }
  h->dir = d;
  h->fd = -1;
  snprintf(h->real, sizeof(h->real), "%s", real);
  snprintf(h->virt, sizeof(h->virt), "%s", virt);
  reply_handle(c, sftp, m, h);
}

static void
on_readdir(sclient_t *c, sftp_session sftp, sftp_client_message m) {
  handle_t *h = find_handle(c, sftp, m);
  int count = 0;

  if(!h || !h->dir) {
    sftp_reply_status(m, SSH_FX_FAILURE, "Not an open folder");
    return;
  }
  for(struct dirent *e; count < READDIR_BATCH && (e = readdir(h->dir));) {
    char path[PATH_MAX], line[PATH_MAX + 80];
    struct sftp_attributes_struct a;
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", h->real, e->d_name);
    if(lstat(path, &st)) continue;
    fill_attr(&st, &a);
    long_name(e->d_name, &st, line, sizeof(line));
    sftp_reply_names_add(m, e->d_name, line, &a);
    count++;
  }
  if(count) sftp_reply_names(m);
  else sftp_reply_status(m, SSH_FX_EOF, NULL);
}

static void
on_read(sclient_t *c, sftp_session sftp, sftp_client_message m) {
  handle_t *h = find_handle(c, sftp, m);
  ssize_t n;

  if(!h || h->dir) {
    sftp_reply_status(m, SSH_FX_FAILURE, "Not an open file");
    return;
  }
  n = pread(h->fd, c->buf, m->len < READ_MAX ? m->len : READ_MAX, (off_t)m->offset);
  if(n < 0) {
    reply_errno(m, errno);
  } else if(n == 0) {
    sftp_reply_status(m, SSH_FX_EOF, NULL);
  } else {
    sftp_reply_data(m, c->buf, (int)n);
    h->bytes += n;
    count_bytes(1, n);
  }
}

static void
on_write(sclient_t *c, sftp_session sftp, sftp_client_message m) {
  handle_t *h = find_handle(c, sftp, m);
  const char *data = m->data ? ssh_string_data(m->data) : NULL;
  size_t len = m->data ? ssh_string_len(m->data) : 0, done = 0;

  if(!h || h->dir) {
    sftp_reply_status(m, SSH_FX_FAILURE, "Not an open file");
    return;
  }
  while(done < len) {
    ssize_t n = pwrite(h->fd, data + done, len - done, (off_t)(m->offset + done));
    if(n < 0) {
      if(errno == EINTR) continue;
      reply_errno(m, errno);
      return;
    }
    done += (size_t)n;
  }
  h->wrote = 1;
  h->bytes += (long long)len;
  count_bytes(0, (long long)len);
  sftp_reply_status(m, SSH_FX_OK, NULL);
}

static void
on_stat(sclient_t *c, sftp_session sftp, sftp_client_message m, int type) {
  char virt[PATH_MAX], real[PATH_MAX];
  struct sftp_attributes_struct a;
  struct stat st;
  int rc;

  if(type == SSH_FXP_FSTAT) {
    handle_t *h = find_handle(c, sftp, m);
    if(!h) {
      sftp_reply_status(m, SSH_FX_FAILURE, "Not an open file");
      return;
    }
    rc = h->dir ? stat(h->real, &st) : fstat(h->fd, &st);
  } else {
    const int follow = type == SSH_FXP_STAT;
    rc = map_path(m->filename, virt, real, follow) ? -1 : follow ? stat(real, &st) : lstat(real, &st);
  }
  if(rc) {
    reply_errno(m, errno);
    return;
  }
  fill_attr(&st, &a);
  sftp_reply_attr(m, &a);
}

static void
on_setstat(sclient_t *c, sftp_session sftp, sftp_client_message m, int type) {
  char virt[PATH_MAX], real[PATH_MAX];
  sftp_attributes a = m->attr;
  handle_t *h = NULL;
  int rc = 0;

  if(type == SSH_FXP_FSETSTAT) {
    if(!(h = find_handle(c, sftp, m))) {
      sftp_reply_status(m, SSH_FX_FAILURE, "Not an open file");
      return;
    }
    snprintf(real, sizeof(real), "%s", h->real);
  } else if(map_path(m->filename, virt, real, 1)) {
    reply_errno(m, errno);
    return;
  }
  if(a && (a->flags & SSH_FILEXFER_ATTR_PERMISSIONS)) rc = rc ? rc : chmod(real, (mode_t)(a->permissions & 07777));
  if(a && (a->flags & SSH_FILEXFER_ATTR_SIZE)) {
    rc = rc ? rc : h && !h->dir ? ftruncate(h->fd, (off_t)a->size) : truncate(real, (off_t)a->size);
  }
  if(a && (a->flags & SSH_FILEXFER_ATTR_ACMODTIME)) {
    struct timeval tv[2] = {{(time_t)a->atime, 0}, {(time_t)a->mtime, 0}};
    rc = rc ? rc : utimes(real, tv);
  }
  if(rc) reply_errno(m, errno);
  else sftp_reply_status(m, SSH_FX_OK, NULL);
}

static void
on_path_op(sclient_t *c, sftp_client_message m, int type) {
  char virt[PATH_MAX], real[PATH_MAX];
  int mode = 0755, rc;

  if(map_path(m->filename, virt, real, 0)) {
    reply_errno(m, errno);
    return;
  }
  if(type == SSH_FXP_MKDIR) {
    if(m->attr && (m->attr->flags & SSH_FILEXFER_ATTR_PERMISSIONS)) mode = (int)(m->attr->permissions & 07777);
    rc = mkdir(real, (mode_t)mode);
  } else {
    rc = type == SSH_FXP_RMDIR ? rmdir(real) : unlink(real);
  }
  if(rc) {
    reply_errno(m, errno);
    return;
  }
  if(type == SSH_FXP_MKDIR) event(tr("%s created folder %s"), c->who, virt);
  else if(type == SSH_FXP_RMDIR) event(tr("%s removed folder %s"), c->who, virt);
  else event(tr("%s deleted %s"), c->who, virt);
  sftp_reply_status(m, SSH_FX_OK, NULL);
}

/* SFTP's own RENAME refuses to replace a file; the OpenSSH extension
   (posix-rename) replaces it, as rename() does. */
static void
on_rename(sclient_t *c, sftp_client_message m, int replace) {
  char from_virt[PATH_MAX], from[PATH_MAX], to_virt[PATH_MAX], to[PATH_MAX];
  const char *target = sftp_client_message_get_data(m);
  struct stat st;

  if(!target || map_path(m->filename, from_virt, from, 0) || map_path(target, to_virt, to, 0)) {
    reply_errno(m, target ? errno : EINVAL);
    return;
  }
  if(!replace && !lstat(to, &st)) {
    sftp_reply_status(m, SSH_FX_FAILURE, "It already exists");
    return;
  }
  if(rename(from, to)) {
    reply_errno(m, errno);
    return;
  }
  event(tr("%s renamed to %s"), c->who, to_virt);
  sftp_reply_status(m, SSH_FX_OK, NULL);
}

static void
on_message(sclient_t *c, sftp_session sftp, sftp_client_message m) {
  const int type = sftp_client_message_get_type(m);
  char virt[PATH_MAX], real[PATH_MAX];

  switch(type) {
  case SSH_FXP_REALPATH: {
    struct sftp_attributes_struct a;
    memset(&a, 0, sizeof(a));
    normalize(m->filename, virt, sizeof(virt));
    sftp_reply_name(m, virt, &a);
    break;
  }
  case SSH_FXP_OPEN: on_open(c, sftp, m); break;
  case SSH_FXP_OPENDIR: on_opendir(c, sftp, m); break;
  case SSH_FXP_READDIR: on_readdir(c, sftp, m); break;
  case SSH_FXP_READ: on_read(c, sftp, m); break;
  case SSH_FXP_WRITE: on_write(c, sftp, m); break;
  case SSH_FXP_CLOSE: {
    handle_t *h = find_handle(c, sftp, m);
    if(!h) {
      sftp_reply_status(m, SSH_FX_FAILURE, "Not an open file");
      break;
    }
    close_handle(c, sftp, h);
    sftp_reply_status(m, SSH_FX_OK, NULL);
    break;
  }
  case SSH_FXP_STAT:
  case SSH_FXP_LSTAT:
  case SSH_FXP_FSTAT: on_stat(c, sftp, m, type); break;
  case SSH_FXP_SETSTAT:
  case SSH_FXP_FSETSTAT: on_setstat(c, sftp, m, type); break;
  case SSH_FXP_MKDIR:
  case SSH_FXP_RMDIR:
  case SSH_FXP_REMOVE: on_path_op(c, m, type); break;
  case SSH_FXP_RENAME: on_rename(c, m, 0); break;
  case SSH_FXP_READLINK: {
    char target[PATH_MAX];
    struct sftp_attributes_struct a;
    ssize_t n;
    if(map_path(m->filename, virt, real, 0) || (n = readlink(real, target, sizeof(target) - 1)) < 0) {
      reply_errno(m, errno);
      break;
    }
    target[n] = 0;
    memset(&a, 0, sizeof(a));
    sftp_reply_name(m, target, &a);
    break;
  }
  case SSH_FXP_EXTENDED: {
    const char *what = sftp_client_message_get_submessage(m);
    if(what && !strcmp(what, "posix-rename@openssh.com")) on_rename(c, m, 1);
    else sftp_reply_status(m, SSH_FX_OP_UNSUPPORTED, "Not supported");
    break;
  }
  default:
    /* SYMLINK among them: a link made here could point anywhere. */
    sftp_reply_status(m, SSH_FX_OP_UNSUPPORTED, "Not supported");
    break;
  }
}

/* ---------- a connection ---------- */

static void
name_client(sclient_t *c) {
  char name[256] = "";

  inet_ntop(AF_INET, &c->peer.sin_addr, c->ip, sizeof(c->ip));
  if(!getnameinfo((struct sockaddr *)&c->peer, sizeof(c->peer), name, sizeof(name), NULL, 0, NI_NAMEREQD) &&
     name[0] && strcmp(name, c->ip)) {
    char *dot = strchr(name, '.');
    if(dot && dot != name) *dot = 0;
    snprintf(c->host, sizeof(c->host), "%s", name);
  }
  snprintf(c->who, sizeof(c->who), "%s", c->host[0] ? c->host : c->ip);
}

/* Password login, the FTP server's. Nothing else is offered. */
static int
log_in(sclient_t *c, ssh_session s) {
  int failures = 0;

  ssh_set_auth_methods(s, SSH_AUTH_METHOD_PASSWORD);
  while(!g_stop) {
    ssh_message m = ssh_message_get(s);
    int ok = 0;

    if(!m) return -1;
    if(ssh_message_type(m) == SSH_REQUEST_AUTH && ssh_message_subtype(m) == SSH_AUTH_METHOD_PASSWORD) {
      const char *user = ssh_message_auth_user(m), *pass = ssh_message_auth_password(m);
      pthread_mutex_lock(&g_lock);
      ok = user && pass && !strcmp(user, g_cfg.user) && !strcmp(pass, g_cfg.pass);
      for(int i = 0; i < FTPD_MAX_GUESTS && !ok && user && pass; i++) {  // quick connect's, from its address
        ok = g_guests[i].user[0] && !strcmp(user, g_guests[i].user) && !strcmp(pass, g_guests[i].pass) &&
             !strcmp(c->ip, g_guests[i].ip);
      }
      pthread_mutex_unlock(&g_lock);
      if(ok) {
        ssh_message_auth_reply_success(m, 0);
        ssh_message_free(m);
        return 0;
      }
      event(tr("%s: wrong username or password"), c->who);
      failures++;
    }
    ssh_message_auth_set_methods(m, SSH_AUTH_METHOD_PASSWORD);
    ssh_message_reply_default(m);
    ssh_message_free(m);
    if(failures >= MAX_LOGIN_FAILURES) return -1;
  }
  return -1;
}

/* A session channel, then its "sftp" subsystem. Shells, commands and
   forwarding are all refused: this is a file server. */
static ssh_channel
open_sftp_channel(ssh_session s) {
  ssh_channel chan = NULL;

  while(!g_stop) {
    ssh_message m = ssh_message_get(s);
    int type, sub;

    if(!m) break;
    type = ssh_message_type(m);
    sub = ssh_message_subtype(m);
    if(type == SSH_REQUEST_CHANNEL_OPEN && sub == SSH_CHANNEL_SESSION && !chan) {
      chan = ssh_message_channel_request_open_reply_accept(m);
    } else if(type == SSH_REQUEST_CHANNEL && sub == SSH_CHANNEL_REQUEST_SUBSYSTEM && chan &&
              !strcmp(ssh_message_channel_request_subsystem(m), "sftp")) {
      ssh_message_channel_request_reply_success(m);
      ssh_message_free(m);
      return chan;
    } else {
      ssh_message_reply_default(m);
    }
    ssh_message_free(m);
  }
  return NULL;
}

static void *
client_main(void *arg) {
  sclient_t *c = arg;
  ssh_session s = ssh_new();
  sftp_session sftp = NULL;
  long timeout = 30;

  name_client(c);
  if(!s || ssh_bind_accept_fd(g_bind, s, c->fd) != SSH_OK) {
    if(s) ssh_free(s);
    else close(c->fd);
    s = NULL;
    goto out;
  }
  ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeout);
  if(ssh_handle_key_exchange(s) != SSH_OK || log_in(c, s)) goto out;
  c->logged_in = 1;
  adjust(&g_stats.clients, 1);
  event(tr("%s connected"), c->who);
  {
    ssh_channel chan = open_sftp_channel(s);
    if(!chan || !(sftp = sftp_server_new(s, chan)) || sftp_server_init(sftp) < 0) goto out;
  }
  while(!g_stop) {
    sftp_client_message m = sftp_get_client_message(sftp);
    if(!m) break;
    on_message(c, sftp, m);
    sftp_client_message_free(m);
  }

out:
  while(c->handles) close_handle(c, sftp, c->handles);
  if(c->logged_in) {
    adjust(&g_stats.clients, -1);
    event(tr("%s disconnected"), c->who);
  }
  /* Leave the table first, so sftpd_stop() never shuts down a reused fd. */
  pthread_mutex_lock(&g_lock);
  g_clients[c->slot] = NULL;
  pthread_mutex_unlock(&g_lock);
  if(sftp) sftp_free(sftp);
  if(s) {
    ssh_disconnect(s);
    ssh_free(s);  // closes the connection
  }
  free(c->buf);
  free(c);
  return NULL;
}

/* ---------- listener ---------- */

static void *
listen_main(void *arg) {
  int srv = (int)(long)arg;

  while(!g_stop) {
    struct pollfd pfd = {srv, POLLIN, 0};
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    pthread_t t;
    sclient_t *c;
    int fd, slot = -1;

    if(poll(&pfd, 1, 300) <= 0) continue;
    if(pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
    if((fd = accept(srv, (struct sockaddr *)&peer, &len)) < 0) {
      if(errno == EBADF || errno == ENOTSOCK || errno == EINVAL) break;
      continue;
    }
    pthread_mutex_lock(&g_lock);
    for(int i = 0; i < FTPD_MAX_CLIENTS; i++) if(!g_clients[i]) { slot = i; break; }
    pthread_mutex_unlock(&g_lock);
    if(slot < 0 || !(c = calloc(1, sizeof(*c))) || !(c->buf = malloc(READ_MAX))) {
      if(slot >= 0 && c) free(c);
      close(fd);
      continue;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
    c->fd = fd;
    c->peer = peer;
    c->slot = slot;
    pthread_mutex_lock(&g_lock);
    g_clients[slot] = c;
    pthread_mutex_unlock(&g_lock);
    if(pthread_create(&t, NULL, client_main, c)) {
      pthread_mutex_lock(&g_lock);
      g_clients[slot] = NULL;
      pthread_mutex_unlock(&g_lock);
      close(fd);
      free(c->buf);
      free(c);
      continue;
    }
    pthread_detach(t);
  }
  close(srv);

  /* Wake sessions blocked mid-request, then wait for them to finish. */
  for(int tries = 0; tries < 100; tries++) {
    int left = 0;
    pthread_mutex_lock(&g_lock);
    for(int i = 0; i < FTPD_MAX_CLIENTS; i++) {
      if(!g_clients[i]) continue;
      left++;
      shutdown(g_clients[i]->fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&g_lock);
    if(!left) break;
    usleep(100 * 1000);
  }
  ssh_bind_free(g_bind);
  g_bind = NULL;
  event(tr("SFTP server stopped"));
  pthread_mutex_lock(&g_lock);
  memset(g_guests, 0, sizeof(g_guests));
  g_running = 0;
  pthread_mutex_unlock(&g_lock);
  return NULL;
}

int
sftpd_start(const ftpd_config_t *cfg) {
  struct sockaddr_in addr;
  ssh_key key = NULL, ecdsa = NULL;
  pthread_t t;
  int srv, err;

  signal(SIGPIPE, SIG_IGN);
  pthread_mutex_lock(&g_lock);
  if(g_running) {
    pthread_mutex_unlock(&g_lock);
    return 0;
  }
  g_cfg = *cfg;
  pthread_mutex_unlock(&g_lock);

  ssh_init();
  if(load_host_key(HOST_KEY_NAME, SSH_KEYTYPE_ED25519, &key)) {
    errno = EIO;
    return -1;
  }
  remember_fingerprint(key);
  /* Without it, only chiral-ftp's own client goes without (see
     load_host_key); every other app still connects. */
  if(load_host_key(HOST_KEY_ECDSA_NAME, SSH_KEYTYPE_ECDSA_P256, &ecdsa)) ecdsa = NULL;

  if((srv = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    ssh_key_free(key);
    if(ecdsa) ssh_key_free(ecdsa);
    return -1;
  }
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(cfg->port);
  if(bind(srv, (struct sockaddr *)&addr, sizeof(addr)) || listen(srv, 16)) {
    err = errno;
    close(srv);
    ssh_key_free(key);
    if(ecdsa) ssh_key_free(ecdsa);
    errno = err;
    return -1;
  }
  /* The bind object only holds the keys here: the socket is ours, so the
     port and its errors are the same as the FTP server's. It owns the keys
     from now on. */
  if(!(g_bind = ssh_bind_new()) || ssh_bind_options_set(g_bind, SSH_BIND_OPTIONS_IMPORT_KEY, key) != SSH_OK) {
    if(g_bind) ssh_bind_free(g_bind);
    else ssh_key_free(key);
    if(ecdsa) ssh_key_free(ecdsa);
    g_bind = NULL;
    close(srv);
    errno = EIO;
    return -1;
  }
  if(ecdsa && ssh_bind_options_set(g_bind, SSH_BIND_OPTIONS_IMPORT_KEY, ecdsa) != SSH_OK) ssh_key_free(ecdsa);

  pthread_mutex_lock(&g_lock);
  g_stop = 0;
  memset(&g_stats, 0, sizeof(g_stats));
  g_rate_bytes = 0;
  gettimeofday(&g_rate_start, NULL);
  if((err = pthread_create(&t, NULL, listen_main, (void *)(long)srv))) {
    pthread_mutex_unlock(&g_lock);
    ssh_bind_free(g_bind);
    g_bind = NULL;
    close(srv);
    errno = err;
    return -1;
  }
  pthread_detach(t);
  g_running = 1;
  pthread_mutex_unlock(&g_lock);
  event(tr("SFTP server started on port %u"), cfg->port);
  return 0;
}

void
sftpd_stop(void) {
  g_stop = 1;
}
