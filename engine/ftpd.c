/* FTP server. The command set and its replies follow ps5-payload-dev/ftpsrv
   (Copyright (C) 2023-2025 John Törnblom, GPL-3.0-or-later); this version adds
   a login, keeps every path inside the configured root, only opens active-mode
   data connections back to the client, and reports clients and transfers to
   the app. One thread per control connection, blocking sockets polled so
   ftpd_stop() ends everything promptly. */

#include "ftpd.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "config.h"

/* The UI's translation lookup (app/i18n.cpp): activity shown on the TV is
   in the app's language. */
const char *tr(const char *english);
#include "localfs.h"
#include "system.h"

#include <netdb.h>

#define LINE_MAX_LEN 4096
#define IO_BUFSIZE (512 * 1024)
#define IDLE_TIMEOUT_MS (10 * 60 * 1000)
#define DATA_TIMEOUT_MS (30 * 1000)
#define MAX_LOGIN_FAILURES 3
#define RECENT_MAX 40

typedef struct client {
  int ctrl;
  int data;     // open data connection, or -1
  int pasv;     // passive listener, or -1
  struct sockaddr_in peer;
  struct sockaddr_in port_addr;  // PORT/EPRT target; sin_port 0 = passive
  char cwd[PATH_MAX];            // virtual path, always absolute and normalized
  char user[64];
  char ip[INET_ADDRSTRLEN];
  char host[64];  // the device's network name, if the router knows it
  char app[48];   // the client program, if it says (CLNT)
  char who[128];  // how the activity list names this client
  int logged_in;
  int failures;
  long long rest;
  char rename_from[PATH_MAX];
  char line[LINE_MAX_LEN];
  char inbuf[LINE_MAX_LEN];
  int inlen;
  char *buf;
  int slot;
} client_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static ftpd_config_t g_cfg;
static volatile int g_stop;
static int g_running;
static int g_listen_fd = -1;
static time_t g_health_checked;
static client_t *g_clients[FTPD_MAX_CLIENTS];
static ftpd_stats_t g_stats;
static long long g_rate_bytes;     // bytes since g_rate_start
static struct timeval g_rate_start;
static char g_recent[RECENT_MAX][160];
static int g_recent_head, g_recent_count;

/* ---------- bookkeeping ---------- */

static void
event(const char *fmt, ...) {
  char line[160];
  va_list ap;
  time_t t = time(NULL);
  struct tm tm;

  gmtime_r(&t, &tm);
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
ftpd_config_default(ftpd_config_t *c) {
  memset(c, 0, sizeof(*c));
  c->port = FTPD_DEFAULT_PORT;
  snprintf(c->user, sizeof(c->user), "%s", FTPD_DEFAULT_USER);
  snprintf(c->pass, sizeof(c->pass), "%s", FTPD_DEFAULT_PASS);
  snprintf(c->root, sizeof(c->root), "/");
}

void
ftpd_set_config(const ftpd_config_t *c) {
  pthread_mutex_lock(&g_lock);
  unsigned short port = g_cfg.port;
  g_cfg = *c;
  if(g_running) g_cfg.port = port;  // the listener keeps its port until restarted
  pthread_mutex_unlock(&g_lock);
}

void
ftpd_get_stats(ftpd_stats_t *out) {
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
ftpd_recent(char lines[][160], int max) {
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

/* Is the listening socket still real?

   A console that has slept can come back with its sockets quietly dead.
   Nothing fails loudly: the listener thread's poll() just stops reporting
   anything, g_running is still 1, and the screen goes on saying the server
   is up and reachable at an address that answers nothing. So the flag is
   not trusted on its own -- every FTPD_HEALTH_SECONDS the socket is asked
   whether it still exists, which is cheap and catches exactly this. */
#define FTPD_HEALTH_SECONDS 20

/* Call with g_lock held. */
static int
listener_alive(void) {
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);

  if(g_listen_fd < 0) return 0;
  return getsockname(g_listen_fd, (struct sockaddr *)&addr, &len) == 0;
}

int
ftpd_running(void) {
  int r;

  pthread_mutex_lock(&g_lock);
  if(g_running) {
    const time_t now = time(NULL);
    /* Also when the clock has jumped backwards, which waking up can do. */
    if(now - g_health_checked >= FTPD_HEALTH_SECONDS || now < g_health_checked) {
      g_health_checked = now;
      if(!listener_alive()) {
        /* event() is this file's own channel into the activity list the
           server screen shows; ftpd.c deliberately doesn't pull in log.h. */
        event(tr("FTP server stopped: the connection was lost"));
        g_stop = 1;
        g_running = 0;
      }
    }
  }
  r = g_running;
  pthread_mutex_unlock(&g_lock);
  return r;
}

void
net_local_ip(char *buf, unsigned long size) {
  /* How a machine finds its own address differs enough between consoles to
     be the platform's business: platform/<name>/system.c. */
  system_local_ip(buf, (size_t)size);
}

/* ---------- naming clients ---------- */

/* "MacBook-Pro" from "MacBook-Pro.lan": routers add their local domain. */
static void
peer_name(client_t *c) {
  char name[256] = "";

  /* libnx resolves this through sfdnsres, so the Switch build uses the same
     call a PC does (the PS5 needed sceNetResolver instead). */
  if(getnameinfo((struct sockaddr *)&c->peer, sizeof(c->peer), name, sizeof(name), NULL, 0, NI_NAMEREQD)) return;
  if(!name[0] || !strcmp(name, c->ip)) return;
  char *dot = strchr(name, '.');
  if(dot && dot != name) *dot = 0;
  snprintf(c->host, sizeof(c->host), "%s", name);
}

/* FileZilla and most clients never say their name; a few send CLNT. */
static void
name_client(client_t *c) {
  if(c->app[0] && c->host[0]) snprintf(c->who, sizeof(c->who), "%s on %s", c->app, c->host);
  else if(c->app[0]) snprintf(c->who, sizeof(c->who), "%s (%s)", c->app, c->ip);
  else if(c->host[0]) snprintf(c->who, sizeof(c->who), "%s", c->host);
  else snprintf(c->who, sizeof(c->who), "%s", c->ip);
}

/* ---------- socket helpers ---------- */

static int
send_all(int fd, const void *data, size_t len) {
  const char *p = data;

  while(len) {
    ssize_t n = send(fd, p, len, 0);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) return -1;
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static int
reply(client_t *c, const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
  va_end(ap);
  if(n < 0) return -1;
  if(n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
  buf[n++] = '\r';
  buf[n++] = '\n';
  return send_all(c->ctrl, buf, (size_t)n);
}

static int
reply_errno(client_t *c, int code) {
  return reply(c, "%d %s", code, strerror(errno));
}

/* Waits for fd to become readable; 0 on timeout or stop. */
static int
wait_readable(int fd, int timeout_ms) {
  for(int waited = 0; !g_stop && waited < timeout_ms; waited += 250) {
    struct pollfd pfd = {fd, POLLIN, 0};
    int n = poll(&pfd, 1, 250);
    if(n > 0) return 1;
    if(n < 0 && errno != EINTR) return -1;
  }
  return 0;
}

/* One command line, without CR/LF. 0 on success, -1 on disconnect/timeout. */
static int
read_line(client_t *c) {
  for(;;) {
    char *nl = memchr(c->inbuf, '\n', (size_t)c->inlen);
    if(nl) {
      int len = (int)(nl - c->inbuf);
      int keep = c->inlen - len - 1;
      if(len && c->inbuf[len - 1] == '\r') len--;
      memcpy(c->line, c->inbuf, (size_t)len);
      c->line[len] = 0;
      memmove(c->inbuf, nl + 1, (size_t)keep);
      c->inlen = keep;
      return 0;
    }
    if(c->inlen >= (int)sizeof(c->inbuf) - 1) return -1;  // line too long
    if(wait_readable(c->ctrl, IDLE_TIMEOUT_MS) <= 0) return -1;
    ssize_t n = recv(c->ctrl, c->inbuf + c->inlen, sizeof(c->inbuf) - 1 - (size_t)c->inlen, 0);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) return -1;
    c->inlen += (int)n;
  }
}

static void
close_fd(int *fd) {
  if(*fd >= 0) close(*fd);
  *fd = -1;
}

/* ---------- paths ---------- */

/* Normalizes arg against the client's cwd into an absolute virtual path:
   "." and ".." are resolved, so it can't climb above "/". */
static int
virtual_path(client_t *c, const char *arg, char *out, size_t size) {
  char tmp[PATH_MAX * 2];
  char *parts[PATH_MAX / 2];
  int n = 0;
  size_t len = 0;

  if(arg[0] == '/') snprintf(tmp, sizeof(tmp), "%s", arg);
  else snprintf(tmp, sizeof(tmp), "%s/%s", c->cwd, arg);
  for(char *save = NULL, *p = strtok_r(tmp, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
    if(!strcmp(p, ".")) continue;
    if(!strcmp(p, "..")) {
      if(n) n--;
      continue;
    }
    if(n >= (int)(sizeof(parts) / sizeof(parts[0]))) return -1;
    parts[n++] = p;
  }
  if(!n) {
    snprintf(out, size, "/");
    return 0;
  }
  out[0] = 0;
  for(int i = 0; i < n; i++) {
    size_t pl = strlen(parts[i]);
    if(len + 1 + pl + 1 > size) return -1;
    out[len++] = '/';
    memcpy(out + len, parts[i], pl);
    len += pl;
    out[len] = 0;
  }
  return 0;
}

/* The real path for a virtual one: the configured root plus the path. */
static int
real_path(const char *vpath, char *out, size_t size) {
  char root[PATH_MAX];
  size_t rl;

  pthread_mutex_lock(&g_lock);
  snprintf(root, sizeof(root), "%s", g_cfg.root);
  pthread_mutex_unlock(&g_lock);
  rl = strlen(root);
  while(rl > 1 && root[rl - 1] == '/') root[--rl] = 0;
  if(!strcmp(root, "/")) return snprintf(out, size, "%s", vpath) >= (int)size ? -1 : 0;
  return snprintf(out, size, "%s%s", root, strcmp(vpath, "/") ? vpath : "") >= (int)size ? -1 : 0;
}

/* Resolves an argument to a real path; replies 501/553 itself on failure. */
static int
resolve(client_t *c, const char *arg, char *real, size_t size, char *virt) {
  char v[PATH_MAX];

  if(!arg[0]) {
    reply(c, "501 Missing path");
    return -1;
  }
  if(virtual_path(c, arg, v, sizeof(v)) || real_path(v, real, size)) {
    reply(c, "553 Path too long");
    return -1;
  }
  if(virt) snprintf(virt, PATH_MAX, "%s", v);
  return 0;
}

/* ---------- data connections ---------- */

static int
data_open(client_t *c) {
  close_fd(&c->data);
  if(c->port_addr.sin_port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return -1;
    if(connect(fd, (struct sockaddr *)&c->port_addr, sizeof(c->port_addr))) {
      close(fd);
      return -1;
    }
    c->data = fd;
  } else {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if(c->pasv < 0) {
      errno = ENOTCONN;
      return -1;
    }
    if(wait_readable(c->pasv, DATA_TIMEOUT_MS) <= 0) {
      errno = ETIMEDOUT;
      return -1;
    }
    if((c->data = accept(c->pasv, (struct sockaddr *)&addr, &len)) < 0) return -1;
    close_fd(&c->pasv);
    /* Only the client that asked may use the passive port. */
    if(addr.sin_addr.s_addr != c->peer.sin_addr.s_addr) {
      close_fd(&c->data);
      errno = EACCES;
      return -1;
    }
  }
  int opt = 1 << 20;
  setsockopt(c->data, SOL_SOCKET, SO_SNDBUF, &opt, sizeof(opt));
  setsockopt(c->data, SOL_SOCKET, SO_RCVBUF, &opt, sizeof(opt));
  return 0;
}

static void
data_done(client_t *c) {
  close_fd(&c->data);
  memset(&c->port_addr, 0, sizeof(c->port_addr));
}

static int
cmd_pasv(client_t *c, int extended) {
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);
  unsigned char *ip, *port;

  close_fd(&c->pasv);
  memset(&c->port_addr, 0, sizeof(c->port_addr));
  if(getsockname(c->ctrl, (struct sockaddr *)&addr, &len)) return reply_errno(c, 425);
  addr.sin_port = 0;
  if((c->pasv = socket(AF_INET, SOCK_STREAM, 0)) < 0) return reply_errno(c, 425);
  len = sizeof(addr);
  if(bind(c->pasv, (struct sockaddr *)&addr, sizeof(addr)) || listen(c->pasv, 1) ||
     getsockname(c->pasv, (struct sockaddr *)&addr, &len)) {
    int err = errno;
    close_fd(&c->pasv);
    errno = err;
    return reply_errno(c, 425);
  }
  if(extended) return reply(c, "229 Entering Extended Passive Mode (|||%u|)", ntohs(addr.sin_port));
  ip = (unsigned char *)&addr.sin_addr.s_addr;
  port = (unsigned char *)&addr.sin_port;
  return reply(c, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u).", ip[0], ip[1], ip[2], ip[3], port[0], port[1]);
}

static int
set_port(client_t *c, struct sockaddr_in *addr) {
  /* No FTP bounce: data only goes back to the client's own address. */
  if(addr->sin_addr.s_addr != c->peer.sin_addr.s_addr || ntohs(addr->sin_port) < 1024) {
    return reply(c, "504 Data connections only go back to your own address");
  }
  close_fd(&c->pasv);
  c->port_addr = *addr;
  return reply(c, "200 PORT command successful");
}

static int
cmd_port(client_t *c, const char *arg) {
  unsigned a[6];
  struct sockaddr_in addr;

  if(sscanf(arg, "%u,%u,%u,%u,%u,%u", a, a + 1, a + 2, a + 3, a + 4, a + 5) != 6) {
    return reply(c, "501 Usage: PORT h1,h2,h3,h4,p1,p2");
  }
  for(int i = 0; i < 6; i++) if(a[i] > 255) return reply(c, "501 Bad address");
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(a[0] << 24 | a[1] << 16 | a[2] << 8 | a[3]);
  addr.sin_port = htons((unsigned short)(a[4] << 8 | a[5]));
  return set_port(c, &addr);
}

static int
cmd_eprt(client_t *c, const char *arg) {
  char d = arg[0], host[64];
  unsigned proto, port;
  struct sockaddr_in addr;
  char fmt[32];

  if(!d) return reply(c, "501 Usage: EPRT |1|address|port|");
  snprintf(fmt, sizeof(fmt), "%c%%u%c%%63[^%c]%c%%u%c", d, d, d, d, d);
  if(sscanf(arg, fmt, &proto, host, &port) != 3 || port > 65535) return reply(c, "501 Bad EPRT argument");
  if(proto != 1) return reply(c, "522 Only IPv4 is supported (1)");
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  if(inet_pton(AF_INET, host, &addr.sin_addr) != 1) return reply(c, "501 Bad address");
  return set_port(c, &addr);
}

/* ---------- listings ---------- */

static void
mode_string(mode_t mode, char *buf) {
  const char *rwx = "rwxrwxrwx";

  buf[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : '-';
  for(int i = 0; i < 9; i++) buf[i + 1] = mode & (1 << (8 - i)) ? rwx[i] : '-';
  buf[10] = 0;
}

static int
data_send(client_t *c, const char *text, size_t len) {
  if(send_all(c->data, text, len)) return -1;
  count_bytes(1, (long long)len);
  return 0;
}

static int
list_entry(client_t *c, int style, const char *name, const struct stat *st) {
  char line[PATH_MAX + 200], date[32];
  struct tm tm;
  time_t now = time(NULL);
  int n;

  gmtime_r(&st->st_mtime, &tm);
  if(style == 'M') {  // MLSD / MLST facts
    strftime(date, sizeof(date), "%Y%m%d%H%M%S", &tm);
    n = snprintf(line, sizeof(line), "type=%s;size=%lld;modify=%s;UNIX.mode=%04o; %s\r\n",
                 S_ISDIR(st->st_mode) ? "dir" : "file", (long long)st->st_size, date,
                 (unsigned)(st->st_mode & 07777), name);
  } else if(style == 'N') {  // NLST
    n = snprintf(line, sizeof(line), "%s\r\n", name);
  } else {  // LIST, like ls -l
    char mode[12];
    mode_string(st->st_mode, mode);
    strftime(date, sizeof(date), now - st->st_mtime > 180 * 24 * 3600 || st->st_mtime > now + 3600
                                     ? "%b %e  %Y" : "%b %e %H:%M", &tm);
    n = snprintf(line, sizeof(line), "%s 1 switch switch %13lld %s %s\r\n", mode, (long long)st->st_size, date, name);
  }
  return data_send(c, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
}

static int
cmd_list(client_t *c, const char *arg, int style) {
  char real[PATH_MAX], child[PATH_MAX * 2];
  struct stat st;
  struct dirent *ent;
  DIR *dir;
  int err = 0;

  /* Skip ls-style flags such as "-la" (FileZilla sends LIST -a). */
  while(*arg == '-') {
    while(*arg && *arg != ' ') arg++;
    while(*arg == ' ') arg++;
  }
  if(resolve(c, *arg ? arg : ".", real, sizeof(real), NULL)) return 0;
  if(stat(real, &st)) return reply_errno(c, 550);
  if(!S_ISDIR(st.st_mode)) {  // a single file
    if(style == 'M') return reply(c, "501 Not a folder");
    if(reply(c, "150 Opening data connection")) return -1;
    if(data_open(c)) return reply_errno(c, 425);
    const char *base = strrchr(real, '/');
    list_entry(c, style, base ? base + 1 : real, &st);
    data_done(c);
    return reply(c, "226 Transfer complete");
  }
  if(!(dir = opendir(real))) return reply_errno(c, 550);
  if(reply(c, "150 Opening data connection")) {
    closedir(dir);
    return -1;
  }
  if(data_open(c)) {
    closedir(dir);
    return reply_errno(c, 425);
  }
  adjust(&g_stats.transfers, 1);
  while(!err && !g_stop && (ent = readdir(dir))) {
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    snprintf(child, sizeof(child), "%s/%s", strcmp(real, "/") ? real : "", ent->d_name);
    if(stat(child, &st)) continue;
    err = list_entry(c, style, ent->d_name, &st);
  }
  adjust(&g_stats.transfers, -1);
  closedir(dir);
  data_done(c);
  return err ? reply(c, "426 Connection closed; transfer aborted") : reply(c, "226 Transfer complete");
}

static int
cmd_mlst(client_t *c, const char *arg) {
  char real[PATH_MAX], virt[PATH_MAX], date[32], line[PATH_MAX + 200];
  struct stat st;
  struct tm tm;

  if(resolve(c, *arg ? arg : ".", real, sizeof(real), virt)) return 0;
  if(stat(real, &st)) return reply_errno(c, 550);
  gmtime_r(&st.st_mtime, &tm);
  strftime(date, sizeof(date), "%Y%m%d%H%M%S", &tm);
  snprintf(line, sizeof(line), "250-Listing %s\r\n type=%s;size=%lld;modify=%s;UNIX.mode=%04o; %s\r\n250 End",
           virt, S_ISDIR(st.st_mode) ? "dir" : "file", (long long)st.st_size, date,
           (unsigned)(st.st_mode & 07777), virt);
  return reply(c, "%s", line);
}

/* ---------- transfers ---------- */

static int
cmd_retr(client_t *c, const char *arg) {
  char real[PATH_MAX];
  struct stat st;
  long long offset = c->rest, sent = 0;
  int fd, err = 0;

  c->rest = 0;
  if(resolve(c, arg, real, sizeof(real), NULL)) return 0;
  if((fd = open(real, O_RDONLY)) < 0) return reply_errno(c, 550);
  if(fstat(fd, &st) || S_ISDIR(st.st_mode)) {
    close(fd);
    return reply(c, "550 Not a file");
  }
  if(offset && lseek(fd, (off_t)offset, SEEK_SET) < 0) {
    close(fd);
    return reply_errno(c, 550);
  }
  if(reply(c, "150 Opening BINARY data connection (%lld bytes)", (long long)st.st_size - offset)) {
    close(fd);
    return -1;
  }
  if(data_open(c)) {
    close(fd);
    return reply_errno(c, 425);
  }
  adjust(&g_stats.transfers, 1);
  for(;;) {
    ssize_t n = read(fd, c->buf, IO_BUFSIZE);
    if(n < 0 && errno == EINTR) continue;
    if(n < 0) {
      err = errno;
      break;
    }
    if(!n) break;
    if(g_stop || send_all(c->data, c->buf, (size_t)n)) {
      err = ECONNRESET;
      break;
    }
    sent += n;
    count_bytes(1, n);
  }
  adjust(&g_stats.transfers, -1);
  close(fd);
  data_done(c);
  if(err) {
    event(tr("Download to %s stopped: %s (%s)"), c->who, arg, strerror(err));
    return reply(c, "426 Transfer aborted: %s", strerror(err));
  }
  event(tr("%s downloaded %s (%lld bytes)"), c->who, arg, sent);
  return reply(c, "226 Transfer complete");
}

static int
cmd_stor(client_t *c, const char *arg, int append) {
  char real[PATH_MAX];
  long long offset = c->rest, got = 0;
  int fd, err = 0;

  c->rest = 0;
  if(resolve(c, arg, real, sizeof(real), NULL)) return 0;
  if(append) fd = localfs_open_write(real, O_WRONLY | O_CREAT | O_APPEND, CHIRAL_FILE_MODE);
  else fd = localfs_open_write(real, O_WRONLY | O_CREAT | (offset ? 0 : O_TRUNC), CHIRAL_FILE_MODE);
  if(fd < 0) return reply_errno(c, 550);
  fchmod(fd, CHIRAL_FILE_MODE);  // also when replacing a file, and regardless of the umask
  if(!append && offset && lseek(fd, (off_t)offset, SEEK_SET) < 0) {
    close(fd);
    return reply_errno(c, 550);
  }
  if(reply(c, "150 Ready to receive")) {
    close(fd);
    return -1;
  }
  if(data_open(c)) {
    close(fd);
    return reply_errno(c, 425);
  }
  adjust(&g_stats.transfers, 1);
  for(;;) {
    if(g_stop) {
      err = ECONNRESET;
      break;
    }
    ssize_t n = recv(c->data, c->buf, IO_BUFSIZE, 0);
    if(n < 0 && errno == EINTR) continue;
    if(n < 0) {
      err = errno;
      break;
    }
    if(!n) break;
    for(ssize_t done = 0; done < n;) {
      ssize_t w = write(fd, c->buf + done, (size_t)(n - done));
      if(w < 0 && errno == EINTR) continue;
      if(w <= 0) {
        err = w < 0 ? errno : ENOSPC;
        break;
      }
      done += w;
    }
    if(err) break;
    got += n;
    count_bytes(0, n);
  }
  /* A resumed upload ends where the data ended. */
  if(!err && !append && offset) (void)ftruncate(fd, (off_t)(offset + got));
  adjust(&g_stats.transfers, -1);
  if(close(fd) && !err) err = errno;
  data_done(c);
  if(err) {
    event(tr("Upload from %s stopped: %s (%s)"), c->who, arg, strerror(err));
    return reply(c, "426 Transfer aborted: %s", strerror(err));
  }
  event(tr("%s uploaded %s (%lld bytes)"), c->who, arg, got);
  return reply(c, "226 Transfer complete");
}

/* ---------- login ---------- */

static int
same_secret(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  unsigned char diff = (unsigned char)(la != lb);

  for(size_t i = 0; i < la; i++) diff |= (unsigned char)(a[i] ^ b[i < lb ? i : 0]);
  return !diff;
}

static int
is_anonymous_name(const char *user) {
  return !strcasecmp(user, "anonymous") || !strcasecmp(user, "ftp");
}

static int
cmd_pass(client_t *c, const char *pass) {
  ftpd_config_t cfg;
  int ok;

  pthread_mutex_lock(&g_lock);
  cfg = g_cfg;
  pthread_mutex_unlock(&g_lock);
  if(!c->user[0]) return reply(c, "503 Send USER first");
  if(cfg.anonymous && is_anonymous_name(c->user)) ok = 1;
  else ok = !strcmp(c->user, cfg.user) && same_secret(pass, cfg.pass);
  if(!ok) {
    c->failures++;
    event(tr("%s: wrong username or password"), c->who);
    sleep(1);  // slows down guessing
    reply(c, "530 Login incorrect");
    return c->failures >= MAX_LOGIN_FAILURES ? -1 : 0;
  }
  c->logged_in = 1;
  adjust(&g_stats.clients, 1);
  if(c->host[0] || c->app[0]) event(tr("%s connected (%s)"), c->who, c->ip);
  else event(tr("%s connected"), c->who);
  return reply(c, "230 Logged in");
}

/* ---------- commands ---------- */

static int
cmd_simple_path(client_t *c, const char *verb, const char *arg) {
  char real[PATH_MAX], virt[PATH_MAX];
  struct stat st;

  if(resolve(c, arg, real, sizeof(real), virt)) return 0;
  if(!strcmp(verb, "CWD")) {
    if(stat(real, &st)) return reply_errno(c, 550);
    if(!S_ISDIR(st.st_mode)) return reply(c, "550 Not a folder");
    snprintf(c->cwd, sizeof(c->cwd), "%s", virt);
    return reply(c, "250 Folder changed to %s", virt);
  }
  if(!strcmp(verb, "MKD")) {
    if(localfs_mkdir(real)) return reply_errno(c, 550);
    event(tr("%s created folder %s"), c->who, virt);
    return reply(c, "257 \"%s\" created", virt);
  }
  if(!strcmp(verb, "RMD")) {
    if(localfs_rmdir(real)) return reply_errno(c, 550);
    event(tr("%s removed folder %s"), c->who, virt);
    return reply(c, "250 Folder removed");
  }
  if(!strcmp(verb, "DELE")) {
    if(localfs_unlink(real)) return reply_errno(c, 550);
    event(tr("%s deleted %s"), c->who, virt);
    return reply(c, "250 File deleted");
  }
  if(!strcmp(verb, "SIZE")) {
    if(stat(real, &st)) return reply_errno(c, 550);
    if(S_ISDIR(st.st_mode)) return reply(c, "550 Not a file");
    return reply(c, "213 %lld", (long long)st.st_size);
  }
  if(!strcmp(verb, "MDTM")) {
    char date[32];
    struct tm tm;
    if(stat(real, &st)) return reply_errno(c, 550);
    gmtime_r(&st.st_mtime, &tm);
    strftime(date, sizeof(date), "%Y%m%d%H%M%S", &tm);
    return reply(c, "213 %s", date);
  }
  if(!strcmp(verb, "RNFR")) {
    if(stat(real, &st)) return reply_errno(c, 550);
    snprintf(c->rename_from, sizeof(c->rename_from), "%s", real);
    return reply(c, "350 Ready for RNTO");
  }
  if(!strcmp(verb, "RNTO")) {
    if(!c->rename_from[0]) return reply(c, "503 Send RNFR first");
    int r = localfs_rename(c->rename_from, real);
    c->rename_from[0] = 0;
    if(r) return reply_errno(c, 550);
    event(tr("%s renamed to %s"), c->who, virt);
    return reply(c, "250 Renamed");
  }
  return reply(c, "502 Not implemented");
}


static int
cmd_mfmt(client_t *c, const char *arg) {
  char stamp[16], real[PATH_MAX];
  struct tm tm;
  struct utimbuf t;
  const char *path = strchr(arg, ' ');

  if(!path || path - arg != 14) return reply(c, "501 Usage: MFMT YYYYMMDDHHMMSS path");
  memcpy(stamp, arg, 14);
  stamp[14] = 0;
  memset(&tm, 0, sizeof(tm));
  if(sscanf(stamp, "%4d%2d%2d%2d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
            &tm.tm_sec) != 6) {
    return reply(c, "501 Bad time");
  }
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  if(resolve(c, path + 1, real, sizeof(real), NULL)) return 0;
  t.actime = t.modtime = system_timegm(&tm);
  if(utime(real, &t)) return reply_errno(c, 550);
  return reply(c, "213 Modify=%s; %s", stamp, path + 1);
}

static int
cmd_site(client_t *c, const char *arg) {
  if(!strncasecmp(arg, "CHMOD ", 6)) {
    char real[PATH_MAX];
    char *end;
    long mode = strtol(arg + 6, &end, 8);
    if(end == arg + 6 || *end != ' ' || mode < 0 || mode > 07777) return reply(c, "501 Usage: SITE CHMOD mode path");
    if(resolve(c, end + 1, real, sizeof(real), NULL)) return 0;
    if(chmod(real, (mode_t)mode)) return reply_errno(c, 550);
    return reply(c, "200 Permissions changed");
  }
  return reply(c, "504 Unknown SITE command");
}

/* Returns -1 to end the session. */
static int
execute(client_t *c) {
  char *arg = strchr(c->line, ' ');
  char verb[8];
  size_t vl;

  if(arg) *arg++ = 0;
  else arg = c->line + strlen(c->line);
  vl = strlen(c->line);
  if(!vl || vl >= sizeof(verb)) return reply(c, "500 Unknown command");
  for(size_t i = 0; i <= vl; i++) verb[i] = (char)(c->line[i] >= 'a' && c->line[i] <= 'z' ? c->line[i] - 32 : c->line[i]);
  /* X-prefixed aliases (XPWD, XMKD...) behave like the plain commands. */
  if(verb[0] == 'X' && (!strcmp(verb, "XPWD") || !strcmp(verb, "XCWD") || !strcmp(verb, "XCUP") ||
                        !strcmp(verb, "XMKD") || !strcmp(verb, "XRMD"))) {
    memmove(verb, verb + 1, vl);
  }

  if(!strcmp(verb, "QUIT")) {
    reply(c, "221 Goodbye");
    return -1;
  }
  if(!strcmp(verb, "CLNT")) {
    snprintf(c->app, sizeof(c->app), "%.*s", (int)strcspn(arg, " /"), arg);
    name_client(c);
    return reply(c, "200 Hello %s", c->app);
  }
  if(!strcmp(verb, "USER")) {
    if(c->logged_in) return reply(c, "530 Already logged in");
    snprintf(c->user, sizeof(c->user), "%s", arg);
    return reply(c, "331 Password required for %s", arg);
  }
  if(!strcmp(verb, "PASS")) {
    if(c->logged_in) return reply(c, "230 Already logged in");
    return cmd_pass(c, arg);
  }
  if(!strcmp(verb, "FEAT"))
    return reply(c, "211-Features:\r\n EPSV\r\n EPRT\r\n MDTM\r\n MFMT\r\n MLST type*;size*;modify*;UNIX.mode*;\r\n"
                    " REST STREAM\r\n SIZE\r\n UTF8\r\n211 End");
  if(!strcmp(verb, "SYST")) return reply(c, "215 UNIX Type: L8");
  if(!strcmp(verb, "NOOP")) return reply(c, "200 OK");
  if(!strcmp(verb, "OPTS")) return reply(c, "200 OK");
  if(!c->logged_in) return reply(c, "530 Please log in with USER and PASS");

  if(!strcmp(verb, "PWD")) return reply(c, "257 \"%s\" is the current folder", c->cwd);
  if(!strcmp(verb, "CDUP")) return cmd_simple_path(c, "CWD", "..");
  if(!strcmp(verb, "CWD") || !strcmp(verb, "MKD") || !strcmp(verb, "RMD") || !strcmp(verb, "DELE") ||
     !strcmp(verb, "SIZE") || !strcmp(verb, "MDTM") || !strcmp(verb, "RNFR") || !strcmp(verb, "RNTO")) {
    return cmd_simple_path(c, verb, arg);
  }
  if(!strcmp(verb, "TYPE")) return reply(c, "200 Type set to %s", toupper((unsigned char)arg[0]) == 'A' ? "A" : "I");
  if(!strcmp(verb, "MODE")) return reply(c, "%s", toupper((unsigned char)arg[0]) == 'S' ? "200 OK" : "504 Only stream mode");
  if(!strcmp(verb, "STRU")) return reply(c, "%s", toupper((unsigned char)arg[0]) == 'F' ? "200 OK" : "504 Only file structure");
  if(!strcmp(verb, "ALLO")) return reply(c, "202 No storage allocation needed");
  if(!strcmp(verb, "PASV")) return cmd_pasv(c, 0);
  if(!strcmp(verb, "EPSV")) return cmd_pasv(c, 1);
  if(!strcmp(verb, "PORT")) return cmd_port(c, arg);
  if(!strcmp(verb, "EPRT")) return cmd_eprt(c, arg);
  if(!strcmp(verb, "LIST")) return cmd_list(c, arg, 'L');
  if(!strcmp(verb, "NLST")) return cmd_list(c, arg, 'N');
  if(!strcmp(verb, "MLSD")) return cmd_list(c, arg, 'M');
  if(!strcmp(verb, "MLST")) return cmd_mlst(c, arg);
  if(!strcmp(verb, "REST")) {
    char *end;
    long long n = strtoll(arg, &end, 10);
    if(end == arg || *end || n < 0) return reply(c, "501 Bad offset");
    c->rest = n;
    return reply(c, "350 Restarting at %lld", n);
  }
  if(!strcmp(verb, "RETR")) return cmd_retr(c, arg);
  if(!strcmp(verb, "STOR")) return cmd_stor(c, arg, 0);
  if(!strcmp(verb, "APPE")) return cmd_stor(c, arg, 1);
  if(!strcmp(verb, "MFMT")) return cmd_mfmt(c, arg);
  if(!strcmp(verb, "SITE")) return cmd_site(c, arg);
  if(!strcmp(verb, "ABOR")) return reply(c, "226 No transfer to abort");
  return reply(c, "502 %s is not supported", verb);
}

static void *
client_main(void *arg) {
  client_t *c = arg;

  inet_ntop(AF_INET, &c->peer.sin_addr, c->ip, sizeof(c->ip));
  peer_name(c);
  name_client(c);
  setsockopt(c->ctrl, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
  if(!reply(c, "220 %s %s FTP server ready", APP_NAME, VERSION_TAG)) {
    while(!g_stop && !read_line(c)) {
      if(execute(c)) break;
    }
  }
  if(c->logged_in) {
    adjust(&g_stats.clients, -1);
    event(tr("%s disconnected"), c->who);
  }
  /* Leave the table first, so ftpd_stop() never shuts down a reused fd. */
  pthread_mutex_lock(&g_lock);
  g_clients[c->slot] = NULL;
  pthread_mutex_unlock(&g_lock);
  close_fd(&c->data);
  close_fd(&c->pasv);
  close_fd(&c->ctrl);
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
    client_t *c;
    int fd, slot = -1;

    if(poll(&pfd, 1, 300) <= 0) continue;
    /* The socket itself has failed -- after a sleep, typically. Without
       this the loop spins forever on a dead listener while every flag
       still says the server is running. */
    if(pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
    if((fd = accept(srv, (struct sockaddr *)&peer, &len)) < 0) {
      if(errno == EBADF || errno == ENOTSOCK || errno == EINVAL) break;
      continue;
    }
    pthread_mutex_lock(&g_lock);
    for(int i = 0; i < FTPD_MAX_CLIENTS; i++) if(!g_clients[i]) { slot = i; break; }
    pthread_mutex_unlock(&g_lock);
    if(slot < 0) {
      const char busy[] = "421 Too many connections, try again later\r\n";
      send(fd, busy, sizeof(busy) - 1, 0);
      close(fd);
      continue;
    }
    if(!(c = calloc(1, sizeof(*c))) || !(c->buf = malloc(IO_BUFSIZE))) {
      if(c) free(c);
      close(fd);
      continue;
    }
    c->ctrl = fd;
    c->data = c->pasv = -1;
    c->peer = peer;
    c->slot = slot;
    snprintf(c->cwd, sizeof(c->cwd), "/");
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
  pthread_mutex_lock(&g_lock);
  g_listen_fd = -1;
  pthread_mutex_unlock(&g_lock);
  close(srv);

  /* Wake sessions blocked in a transfer, then wait for them to finish. */
  for(int tries = 0; tries < 100; tries++) {
    int left = 0;
    pthread_mutex_lock(&g_lock);
    for(int i = 0; i < FTPD_MAX_CLIENTS; i++) {
      if(!g_clients[i]) continue;
      left++;
      shutdown(g_clients[i]->ctrl, SHUT_RDWR);
      if(g_clients[i]->data >= 0) shutdown(g_clients[i]->data, SHUT_RDWR);
    }
    pthread_mutex_unlock(&g_lock);
    if(!left) break;
    usleep(100 * 1000);
  }
  event(tr("FTP server stopped"));
  pthread_mutex_lock(&g_lock);
  g_running = 0;
  pthread_mutex_unlock(&g_lock);
  return NULL;
}

int
ftpd_start(const ftpd_config_t *cfg) {
  struct sockaddr_in addr;
  pthread_t t;
  int srv, err;

  signal(SIGPIPE, SIG_IGN);
  pthread_mutex_lock(&g_lock);
  if(g_running) {
    pthread_mutex_unlock(&g_lock);
    return 0;
  }
  g_cfg = *cfg;
  if((srv = socket(AF_INET, SOCK_STREAM, 0)) < 0) goto fail;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(cfg->port);
  if(bind(srv, (struct sockaddr *)&addr, sizeof(addr)) || listen(srv, 16)) {
    err = errno;
    close(srv);
    errno = err;
    goto fail;
  }
  g_stop = 0;
  g_listen_fd = srv;
  g_health_checked = time(NULL);
  memset(&g_stats, 0, sizeof(g_stats));
  g_rate_bytes = 0;
  gettimeofday(&g_rate_start, NULL);
  if((err = pthread_create(&t, NULL, listen_main, (void *)(long)srv))) {
    close(srv);
    errno = err;
    goto fail;
  }
  pthread_detach(t);
  g_running = 1;
  pthread_mutex_unlock(&g_lock);
  event(tr("FTP server started on port %u"), cfg->port);
  return 0;

fail:
  err = errno;
  pthread_mutex_unlock(&g_lock);
  errno = err;
  return -1;
}

void
ftpd_stop(void) {
  g_stop = 1;
}
