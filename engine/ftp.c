/* FTP client engine.

The command sequences follow FileZilla's FTP engine (src/engine/ftp in the
FileZilla 3.x sources): logon, FEAT-driven capabilities, MLSD with LIST
fallback, PASV with the unroutable-address workaround, REST/APPE resume. */

#include "ftp.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.h"
#include "ftp_internal.h"
#include "log.h"

#define set_error ftp_set_error
#define sock_timeouts ftp_sock_timeouts

#define CONNECT_TIMEOUT_MS 15000
#define CONTROL_TIMEOUT_S  30
#define DATA_TIMEOUT_S     60
#define DATA_BUFFER_SIZE   (1024 * 1024)
#define SOCKET_BUFFER_SIZE (1024 * 1024)
#define LISTING_MAX        (64 * 1024 * 1024)

#ifdef MSG_NOSIGNAL
#define MSG_NOSIGNAL_FLAG MSG_NOSIGNAL
#else
#define MSG_NOSIGNAL_FLAG 0
#endif

void
ftp_set_error(ftp_conn_t *c, const char *fmt, ...) {
  va_list args;

  va_start(args, fmt);
  vsnprintf(c->error, sizeof(c->error), fmt, args);
  va_end(args);
  log_msg(c->log_id, LOG_ERROR, "%s", c->error);
}

/* Uses the server's reply text, minus the code, as the error message. */
static int
fail_reply(ftp_conn_t *c, const char *what) {
  const char *text = c->reply;
  const char *nl = strrchr(text, '\n');

  if(nl) text = nl + 1;
  if(strlen(text) > 4 && isdigit((unsigned char)text[0])) text += 4;
  set_error(c, "%s: %s", what, *text ? text : "unexpected reply");
  return -1;
}

static int
canceled(const ftp_conn_t *c) {
  return c->cancel && *c->cancel;
}

static void
drop(ftp_conn_t *c) {
  if(c->ssh) sftpc_drop(c);
  if(c->fd >= 0) {
    close(c->fd);
    c->fd = -1;
  }
  c->rlen = 0;
  c->binary = 0;
}

void
ftp_sock_timeouts(int fd, int seconds) {
  struct timeval tv = {seconds, 0};

  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int
tcp_connect(ftp_conn_t *c, const struct sockaddr *addr, socklen_t len) {
  int fd;
  int flags;
  int err = 0;
  socklen_t errlen = sizeof(err);
  int waited = 0;

  if((fd = socket(addr->sa_family, SOCK_STREAM, 0)) < 0) {
    return -1;
  }
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &(int){1}, sizeof(int));
#endif
  flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if(connect(fd, addr, len) && errno != EINPROGRESS) {
    err = errno;
    close(fd);
    errno = err;
    return -1;
  }
  /* Poll in short slices so a canceled transfer stops promptly. */
  for(;;) {
    struct pollfd pfd = {fd, POLLOUT, 0};
    int n = poll(&pfd, 1, 250);

    if(n > 0) break;
    if(n < 0 && errno != EINTR) {
      err = errno;
      close(fd);
      errno = err;
      return -1;
    }
    waited += 250;
    if(canceled(c) || waited >= CONNECT_TIMEOUT_MS) {
      close(fd);
      errno = canceled(c) ? ECANCELED : ETIMEDOUT;
      return -1;
    }
  }
  if(getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) || err) {
    close(fd);
    errno = err ? err : EIO;
    return -1;
  }
  fcntl(fd, F_SETFL, flags);
  return fd;
}

void
ftp_init(ftp_conn_t *c, int log_id) {
  memset(c, 0, sizeof(*c));
  c->fd = -1;
  c->log_id = log_id;
}

int
ftp_is_open(const ftp_conn_t *c) {
  return c->fd >= 0;
}

int
ftp_same_server(const ftp_server_t *a, const ftp_server_t *b) {
  return a->protocol == b->protocol && !strcmp(a->keyfile, b->keyfile) && !strcasecmp(a->host, b->host) && a->port == b->port &&
         !strcmp(a->user, b->user) && !strcmp(a->pass, b->pass);
}

static int
read_line(ftp_conn_t *c, char *line, size_t size) {
  for(;;) {
    char *nl = memchr(c->rbuf, '\n', c->rlen);
    ssize_t n;

    if(nl) {
      size_t len = (size_t)(nl - c->rbuf);
      size_t copy = len < size - 1 ? len : size - 1;

      memcpy(line, c->rbuf, copy);
      line[copy] = 0;
      if(copy && line[copy - 1] == '\r') line[copy - 1] = 0;
      memmove(c->rbuf, nl + 1, c->rlen - len - 1);
      c->rlen -= len + 1;
      return 0;
    }
    if(c->rlen == sizeof(c->rbuf)) {
      c->rlen = 0;  /* absurdly long line; discard it */
    }
    n = recv(c->fd, c->rbuf + c->rlen, sizeof(c->rbuf) - c->rlen, 0);
    if(n > 0) {
      c->rlen += (size_t)n;
      continue;
    }
    if(n < 0 && errno == EINTR) continue;
    if(n == 0) {
      set_error(c, "Connection closed by server");
    } else if(errno == EAGAIN || errno == EWOULDBLOCK) {
      set_error(c, "Connection timed out: the server stopped answering");
    } else {
      set_error(c, "Could not read from socket: %s", strerror(errno));
    }
    drop(c);
    return -1;
  }
}

static int
is_code_line(const char *line) {
  return isdigit((unsigned char)line[0]) && isdigit((unsigned char)line[1]) &&
         isdigit((unsigned char)line[2]) && (line[3] == ' ' || line[3] == '-' ||
                                             line[3] == 0);
}

static int
read_reply(ftp_conn_t *c) {
  char line[2048];
  size_t used = 0;
  int code;

  c->reply[0] = 0;
  c->code = -1;
  if(read_line(c, line, sizeof(line))) return -1;
  if(!c->quiet) log_msg(c->log_id, LOG_RESPONSE, "%s", line);
  if(!strncmp(line, "SSH-", 4)) {
    drop(c);
    return FTP_CODE_SSH;
  }
  if(!is_code_line(line)) {
    set_error(c, "Malformed reply from server");
    drop(c);
    return -1;
  }
  code = atoi(line);
  used = (size_t)snprintf(c->reply, sizeof(c->reply), "%s", line);
  if(line[3] == '-') {
    for(;;) {
      if(read_line(c, line, sizeof(line))) return -1;
      if(!c->quiet) log_msg(c->log_id, LOG_RESPONSE, "%s", line);
      if(used < sizeof(c->reply) - 1) {
        used += (size_t)snprintf(c->reply + used, sizeof(c->reply) - used,
                                 "\n%s", line);
      }
      if(is_code_line(line) && atoi(line) == code && line[3] != '-') break;
    }
  }
  c->code = code;
  c->last_activity = time(NULL);
  return code;
}

int
ftp_command(ftp_conn_t *c, const char *fmt, ...) {
  char cmd[2048];
  va_list args;
  size_t len;
  size_t sent = 0;

  if(c->fd < 0) {
    set_error(c, "Not connected");
    return -1;
  }
  if(c->ssh) {
    set_error(c, "Custom commands are only available on FTP connections");
    return -1;
  }
  va_start(args, fmt);
  vsnprintf(cmd, sizeof(cmd) - 2, fmt, args);
  va_end(args);
  if(strpbrk(cmd, "\r\n")) {
    set_error(c, "Refusing to send a command containing a line break");
    return -1;
  }
  if(!c->quiet) log_msg(c->log_id, LOG_COMMAND, "%s", strncasecmp(cmd, "PASS ", 5) ? cmd : "PASS ****");
  len = strlen(cmd);
  memcpy(cmd + len, "\r\n", 2);
  len += 2;
  while(sent < len) {
    ssize_t n = send(c->fd, cmd + sent, len - sent, MSG_NOSIGNAL_FLAG);

    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) {
      set_error(c, "Could not write to socket: %s", strerror(errno));
      drop(c);
      return -1;
    }
    sent += (size_t)n;
  }
  return read_reply(c);
}

static int
ok2(int code) {
  return code >= 200 && code < 300;
}

static int
ensure_binary(ftp_conn_t *c) {
  int code;

  if(c->binary) return 0;
  if((code = ftp_command(c, "TYPE I")) < 0) return -1;
  if(!ok2(code)) return fail_reply(c, "TYPE I failed");
  c->binary = 1;
  return 0;
}

static void
parse_features(ftp_conn_t *c) {
  const char *p = c->reply;

  while(p && *p) {
    const char *nl = strchr(p, '\n');
    char feat[64];
    size_t n = 0;

    while(*p == ' ') p++;
    while(p[n] && p[n] != '\n' && p[n] != ' ' && n < sizeof(feat) - 1) {
      feat[n] = (char)toupper((unsigned char)p[n]);
      n++;
    }
    feat[n] = 0;
    if(!strcmp(feat, "MLST")) {
      const char *facts = p + n;
      size_t len = 0;

      while(*facts == ' ') facts++;
      while(facts[len] && facts[len] != '\n' && len < sizeof(c->mlst_facts) - 1) len++;
      memcpy(c->mlst_facts, facts, len);
      c->mlst_facts[len] = 0;
    }
    if(!strcmp(feat, "MLSD") || !strcmp(feat, "MLST")) c->feat_mlsd = 1;
    else if(!strcmp(feat, "UTF8")) c->feat_utf8 = 1;
    else if(!strcmp(feat, "MFMT")) c->feat_mfmt = 1;
    p = nl ? nl + 1 : NULL;
  }
}

/* Like FileZilla, ask for Unix permissions and owners in MLSD output when the
   server offers them but doesn't enable them by default. */
static int
request_mlst_facts(ftp_conn_t *c) {
  static const char *wanted[] = {
    "type", "size", "modify", "perm", "UNIX.mode", "UNIX.owner", "UNIX.group",
    "UNIX.ownername", "UNIX.groupname", "UNIX.uid", "UNIX.gid",
  };
  char opts[256] = "";
  size_t used = 0;
  int extra = 0;

  if(!c->mlst_facts[0]) return 0;
  for(size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
    const char *p = c->mlst_facts;
    size_t n = strlen(wanted[i]);

    while(*p) {
      if(!strncasecmp(p, wanted[i], n) && (p[n] == ';' || p[n] == '*')) {
        if(p[n] != '*') extra = 1;  /* available but not enabled */
        used += (size_t)snprintf(opts + used, sizeof(opts) - used, "%s;", wanted[i]);
        break;
      }
      p = strchr(p, ';');
      if(!p) break;
      p++;
    }
  }
  if(!extra) return 0;
  return ftp_command(c, "OPTS MLST %s", opts) < 0 ? -1 : 0;
}

int
ftp_connect_socket(ftp_conn_t *c, unsigned short default_port) {
  const ftp_server_t *server = &c->server;
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  struct addrinfo *ai;
  char portstr[8];
  int err;

  if(!server->host[0]) {
    set_error(c, "No host given");
    return -1;
  }
  snprintf(portstr, sizeof(portstr), "%u", server->port ? server->port : default_port);
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  log_msg(c->log_id, LOG_STATUS, "Resolving address of %s", server->host);
  if((err = getaddrinfo(server->host, portstr, &hints, &res)) || !res) {
    set_error(c, "Could not resolve %s: %s", server->host, gai_strerror(err));
    return -1;
  }
  for(ai = res; ai; ai = ai->ai_next) {
    char addr[INET6_ADDRSTRLEN] = "?";

    if(ai->ai_family == AF_INET) {
      inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, addr, sizeof(addr));
    } else if(ai->ai_family == AF_INET6) {
      inet_ntop(AF_INET6, &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr, addr, sizeof(addr));
    }
    log_msg(c->log_id, LOG_STATUS, "Connecting to %s:%s...", addr, portstr);
    if((c->fd = tcp_connect(c, ai->ai_addr, ai->ai_addrlen)) >= 0) {
      memcpy(&c->peer, ai->ai_addr, ai->ai_addrlen);
      c->peer_len = ai->ai_addrlen;
      break;
    }
    log_msg(c->log_id, LOG_ERROR, "Could not connect to %s: %s", addr, strerror(errno));
  }
  freeaddrinfo(res);
  if(c->fd < 0) {
    set_error(c, canceled(c) ? "Connection canceled" : "Could not connect to server");
    return -1;
  }
  sock_timeouts(c->fd, CONTROL_TIMEOUT_S);
  setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
  return 0;
}

int
ftp_open(ftp_conn_t *c, const ftp_server_t *server) {
  const char *user;
  const char *pass;
  int code;

  drop(c);
  c->server = *server;
  c->feat_mlsd = c->feat_utf8 = c->feat_mfmt = c->no_chmod = 0;
  c->mlst_facts[0] = 0;
  c->no_size = c->no_mdtm = c->epsv_failed = c->pasv_failed = 0;
  c->list_a_failed = 0;
  c->error[0] = 0;
  c->code = 0;

  if(server->protocol == FTP_PROTO_SFTP) return sftpc_open(c);
  if(ftp_connect_socket(c, 21)) return -1;
  log_msg(c->log_id, LOG_STATUS, "Connection established, waiting for welcome message...");

  do {
    code = read_reply(c);
  } while(code == 120);
  if(code == FTP_CODE_SSH) {
    /* Someone typed an SSH port into an FTP connection; just use SFTP. */
    log_msg(c->log_id, LOG_STATUS, "Server speaks SSH; switching to SFTP");
    c->server.protocol = FTP_PROTO_SFTP;
    if(!c->server.port) c->server.port = 22;
    return sftpc_open(c);
  }
  if(code < 0) return -1;
  if(code != 220) {
    fail_reply(c, "Server refused the connection");
    drop(c);
    return -1;
  }

  user = server->user[0] ? server->user : "anonymous";
  pass = server->pass[0] || server->user[0] ? server->pass : "anonymous@";
  if((code = ftp_command(c, "USER %s", user)) < 0) return -1;
  if(code == 331) {
    if((code = ftp_command(c, "PASS %s", pass)) < 0) return -1;
  }
  if(code == 332) {
    set_error(c, "Server requires an account (ACCT), which is not supported");
    drop(c);
    return -1;
  }
  if(code != 230 && code != 202) {
    fail_reply(c, "Authentication failed");
    drop(c);
    return -1;
  }
  log_msg(c->log_id, LOG_STATUS, "Logged in");

  if((code = ftp_command(c, "FEAT")) < 0) return -1;
  if(code == 211) parse_features(c);
  if(c->feat_utf8 && ftp_command(c, "OPTS UTF8 ON") < 0) return -1;
  if(request_mlst_facts(c)) return -1;
  if(ftp_command(c, "SYST") < 0) return -1;
  if(ensure_binary(c)) {
    drop(c);
    return -1;
  }
  return 0;
}

void
ftp_close(ftp_conn_t *c) {
  if(c->ssh) {
    sftpc_close(c);
    return;
  }
  if(c->fd >= 0) {
    struct timeval tv = {2, 0};

    /* QUIT is a courtesy; don't wait long for the reply. */
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ftp_command(c, "QUIT");
    drop(c);
    log_msg(c->log_id, LOG_STATUS, "Disconnected from server");
  }
}

static int
is_unroutable_v4(uint32_t ip) {
  return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 20) == 0xAC1 ||
         (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE || (ip >> 22) == 0x191 ||
         ip == 0;
}

static int
parse_pasv(const char *reply, uint32_t *ip, unsigned short *port) {
  const char *p = reply + 3;
  unsigned int v[6];

  /* The tuple is usually in parentheses, but not always. */
  for(; *p; p++) {
    if(isdigit((unsigned char)*p) &&
       sscanf(p, "%u,%u,%u,%u,%u,%u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
      for(int i = 0; i < 6; i++) {
        if(v[i] > 255) return -1;
      }
      *ip = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
      *port = (unsigned short)(v[4] * 256 + v[5]);
      return 0;
    }
  }
  return -1;
}

static int
parse_epsv(const char *reply, unsigned short *port) {
  const char *p = strchr(reply, '(');
  char d;
  long n;

  if(!p || !p[1]) return -1;
  d = p[1];
  if(p[2] != d || p[3] != d) return -1;
  n = strtol(p + 4, NULL, 10);
  if(n <= 0 || n > 65535) return -1;
  *port = (unsigned short)n;
  return 0;
}

static void
set_port(struct sockaddr_storage *addr, unsigned short port) {
  if(addr->ss_family == AF_INET6) {
    ((struct sockaddr_in6 *)addr)->sin6_port = htons(port);
  } else {
    ((struct sockaddr_in *)addr)->sin_port = htons(port);
  }
}

/* Requests a passive data port and connects to it. */
static int
open_data(ftp_conn_t *c) {
  struct sockaddr_storage addr = c->peer;
  int mode = c->server.pasv_mode;
  int v6 = c->peer.ss_family == AF_INET6;
  int try_epsv = mode == FTP_EPSV_ONLY || v6 ||
                 (mode == FTP_PASV_AUTO && c->pasv_failed);
  unsigned short port = 0;
  int have = 0;
  int code;
  int fd;
  int buf = SOCKET_BUFFER_SIZE;

  for(int attempt = 0; attempt < 2 && !have; attempt++) {
    if(try_epsv && !(c->epsv_failed && mode == FTP_PASV_AUTO && !v6)) {
      if((code = ftp_command(c, "EPSV")) < 0) return -1;
      if(code == 229 && !parse_epsv(c->reply, &port)) {
        have = 1;
      } else if(mode == FTP_EPSV_ONLY || v6) {
        return fail_reply(c, "Failed to enter extended passive mode");
      } else {
        c->epsv_failed = 1;
      }
    } else if(!v6 && mode != FTP_EPSV_ONLY) {
      uint32_t ip;

      if((code = ftp_command(c, "PASV")) < 0) return -1;
      if(code == 227 && !parse_pasv(c->reply, &ip, &port)) {
        uint32_t peer_ip = ntohl(((struct sockaddr_in *)&c->peer)->sin_addr.s_addr);

        if(ip == 0 || (is_unroutable_v4(ip) && !is_unroutable_v4(peer_ip))) {
          log_msg(c->log_id, LOG_STATUS, "Server sent passive reply with unroutable "
                  "address. Using server address instead.");
        } else {
          ((struct sockaddr_in *)&addr)->sin_addr.s_addr = htonl(ip);
        }
        have = 1;
      } else if(mode == FTP_PASV_ONLY) {
        return fail_reply(c, "Failed to enter passive mode");
      } else {
        c->pasv_failed = 1;
      }
    }
    try_epsv = !try_epsv;
  }
  if(!have) {
    return fail_reply(c, "Failed to open a data connection");
  }
  set_port(&addr, port);
  if((fd = tcp_connect(c, (struct sockaddr *)&addr, c->peer_len)) < 0 &&
     !canceled(c) && !v6 &&
     ((struct sockaddr_in *)&addr)->sin_addr.s_addr !=
     ((struct sockaddr_in *)&c->peer)->sin_addr.s_addr) {
    /* Laptops with VPN/VM adapters often advertise the wrong local address. */
    log_msg(c->log_id, LOG_STATUS, "Passive address unreachable, retrying with "
            "the server address");
    addr = c->peer;
    set_port(&addr, port);
    fd = tcp_connect(c, (struct sockaddr *)&addr, c->peer_len);
  }
  if(fd < 0) {
    set_error(c, "Could not open data connection: %s", strerror(errno));
    return -1;
  }
  sock_timeouts(fd, DATA_TIMEOUT_S);
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
  return fd;
}

/* Sends cmd after opening a data channel; returns 1 if the final reply already arrived. */
static int
start_transfer(ftp_conn_t *c, int dfd, const char *cmd, const char *what) {
  int code;

  if((code = ftp_command(c, "%s", cmd)) < 0) return -1;
  if(code >= 100 && code < 200) return 0;
  if(ok2(code)) return 1;
  return fail_reply(c, what);
}

static int
finish_transfer(ftp_conn_t *c, int final_seen, const char *what) {
  int code;

  if(final_seen) return 0;
  if((code = read_reply(c)) < 0) return -1;
  return ok2(code) ? 0 : fail_reply(c, what);
}

/* Aborting by closing the control connection is the most reliable way to stop
   a transfer on every server; the next operation reconnects. */
static int
abort_transfer(ftp_conn_t *c, int dfd, const char *reason) {
  close(dfd);
  drop(c);
  set_error(c, "%s", reason);
  return -1;
}

typedef struct text_sink {
  char *data;
  size_t size;
} text_sink_t;

static int
text_sink(void *ctx, const char *data, size_t size) {
  text_sink_t *t = ctx;
  char *p;

  if(t->size + size > LISTING_MAX || !(p = realloc(t->data, t->size + size + 1))) {
    return -1;
  }
  t->data = p;
  memcpy(t->data + t->size, data, size);
  t->size += size;
  t->data[t->size] = 0;
  return 0;
}

static int
receive(ftp_conn_t *c, int dfd, ftp_sink_cb sink, void *ctx) {
  char *buf = malloc(DATA_BUFFER_SIZE);

  if(!buf) return abort_transfer(c, dfd, "Out of memory");
  for(;;) {
    ssize_t n = recv(dfd, buf, DATA_BUFFER_SIZE, 0);

    if(canceled(c)) {
      free(buf);
      return abort_transfer(c, dfd, "Transfer canceled");
    }
    if(n > 0) {
      if(sink(ctx, buf, (size_t)n)) {
        free(buf);
        return abort_transfer(c, dfd, "Could not write local file");
      }
      continue;
    }
    if(n < 0 && errno == EINTR) continue;
    if(n < 0) {
      free(buf);
      return abort_transfer(c, dfd, errno == EAGAIN || errno == EWOULDBLOCK ?
                            "Data connection timed out" :
                            "Data connection failed");
    }
    break;
  }
  free(buf);
  close(dfd);
  return 0;
}

static int
transfer_text(ftp_conn_t *c, const char *cmd, text_sink_t *out) {
  int final_seen;
  int dfd;

  out->data = NULL;
  out->size = 0;
  if((dfd = open_data(c)) < 0) return -1;
  if((final_seen = start_transfer(c, dfd, cmd, "Directory listing failed")) < 0) {
    close(dfd);
    return -1;
  }
  if(receive(c, dfd, text_sink, out)) {
    free(out->data);
    out->data = NULL;
    return -1;
  }
  if(finish_transfer(c, final_seen, "Directory listing failed")) {
    free(out->data);
    out->data = NULL;
    return -1;
  }
  return 0;
}

static int
bad_path(ftp_conn_t *c, const char *path) {
  if(!path || !*path || strpbrk(path, "\r\n")) {
    set_error(c, "Invalid path");
    return 1;
  }
  return 0;
}

int
ftp_pwd(ftp_conn_t *c, char *out, size_t size) {
  if(c->server.protocol == FTP_PROTO_SFTP) return sftpc_pwd(c, out, size);
  const char *p;
  size_t n = 0;
  int code;

  if((code = ftp_command(c, "PWD")) < 0) return -1;
  if(code != 257 || !(p = strchr(c->reply, '"'))) {
    return fail_reply(c, "Could not read the current directory");
  }
  /* Quotes inside the path are doubled. */
  for(p++; *p && n < size - 1; p++) {
    if(*p == '"') {
      if(p[1] != '"') break;
      p++;
    }
    out[n++] = *p;
  }
  out[n] = 0;
  return 0;
}

int
ftp_cwd(ftp_conn_t *c, const char *path) {
  if(c->server.protocol == FTP_PROTO_SFTP) return sftpc_cwd(c, path);
  int code;

  if(bad_path(c, path)) return -1;
  if((code = ftp_command(c, "CWD %s", path)) < 0) return -1;
  return ok2(code) ? 0 : fail_reply(c, "Failed to change directory");
}

int
ftp_list(ftp_conn_t *c, const char *path, ftp_listing_t *out,
         char *resolved, size_t resolved_size) {
  if(c->server.protocol == FTP_PROTO_SFTP) return sftpc_list(c, path, out, resolved, resolved_size);
  text_sink_t text;
  int mlsd;

  memset(out, 0, sizeof(*out));
  if(path && *path && ftp_cwd(c, path)) return -1;
  if(ftp_pwd(c, resolved, resolved_size)) {
    if(!ftp_is_open(c)) return -1;
    snprintf(resolved, resolved_size, "%s", path && *path ? path : "/");
  }
  if(ensure_binary(c)) return -1;
  log_msg(c->log_id, LOG_STATUS, "Retrieving directory listing of \"%s\"...", resolved);

  /* ftpsrv and other servers list the working directory, so never pass a path. */
  mlsd = c->feat_mlsd;
  if(mlsd && transfer_text(c, "MLSD", &text)) {
    if(!ftp_is_open(c) || (c->code != 500 && c->code != 502 && c->code != 504)) {
      return -1;
    }
    c->feat_mlsd = mlsd = 0;
  }
  if(!mlsd) {
    int hidden = c->server.show_hidden && !c->list_a_failed;

    if(transfer_text(c, hidden ? "LIST -a" : "LIST", &text)) {
      if(!hidden || !ftp_is_open(c) || c->code < 500) return -1;
      log_msg(c->log_id, LOG_STATUS, "Server does not seem to support LIST -a");
      c->list_a_failed = 1;
      if(transfer_text(c, "LIST", &text)) return -1;
    }
  }
  ftp_parse_listing(out, text.data, text.size, mlsd, time(NULL));
  free(text.data);
  log_msg(c->log_id, LOG_STATUS, "Directory listing of \"%s\" successful", resolved);
  return 0;
}

int
ftp_homebrew_path(const char *path) {
#if CHIRAL_HOMEBREW_777
  const char *p = strstr(path, "/data/homebrew");
  return p && (p[14] == 0 || p[14] == '/');
#else
  (void)path;
  return 0;
#endif
}

static int
mkdir_only(ftp_conn_t *c, const char *path) {
  if(c->server.protocol == FTP_PROTO_SFTP) return sftpc_mkdir(c, path);
  int code;

  if(bad_path(c, path)) return -1;
  if((code = ftp_command(c, "MKD %s", path)) < 0) return -1;
  /* ftpsrv answers 226 rather than 257. */
  return ok2(code) ? 0 : fail_reply(c, "Failed to create directory");
}

/* A new folder, and then 0777 if it is a homebrew one. The chmod failing
   doesn't fail the folder; a server that refuses (most Windows ones) isn't
   asked again on this connection. */
int
ftp_mkdir(ftp_conn_t *c, const char *path) {
  if(mkdir_only(c, path)) return -1;
  if(ftp_homebrew_path(path) && !c->no_chmod && ftp_chmod(c, path, 0777) && ftp_is_open(c)) c->no_chmod = 1;
  return 0;
}


int
ftp_rmdir(ftp_conn_t *c, const char *path) {
  if(c->server.protocol == FTP_PROTO_SFTP) return sftpc_rmdir(c, path);
  int code;

  if(bad_path(c, path)) return -1;
  if((code = ftp_command(c, "RMD %s", path)) < 0) return -1;
  return ok2(code) ? 0 : fail_reply(c, "Failed to remove directory");
}

int
ftp_delete(ftp_conn_t *c, const char *path) {
  if(c->server.protocol == FTP_PROTO_SFTP) return sftpc_delete(c, path);
  int code;

  if(bad_path(c, path)) return -1;
  if((code = ftp_command(c, "DELE %s", path)) < 0) return -1;
  return ok2(code) ? 0 : fail_reply(c, "Failed to delete file");
}

int
ftp_rename(ftp_conn_t *c, const char *from, const char *to) {
  if(c->server.protocol == FTP_PROTO_SFTP) return sftpc_rename(c, from, to);
  int code;

  if(bad_path(c, from) || bad_path(c, to)) return -1;
  if((code = ftp_command(c, "RNFR %s", from)) < 0) return -1;
  if(code != 350) return fail_reply(c, "Rename failed");
  if((code = ftp_command(c, "RNTO %s", to)) < 0) return -1;
  return ok2(code) ? 0 : fail_reply(c, "Rename failed");
}

int
ftp_chmod(ftp_conn_t *c, const char *path, unsigned int mode) {
  if(c->server.protocol == FTP_PROTO_SFTP) return sftpc_chmod(c, path, mode);
  int code;

  if(bad_path(c, path)) return -1;
  if((code = ftp_command(c, "SITE CHMOD %03o %s", mode & 07777, path)) < 0) return -1;
  if(ok2(code)) return 0;
  if(code == 500 || code == 502 || code == 504) {
    /* ftpsrv takes CHMOD without the SITE prefix. */
    if((code = ftp_command(c, "CHMOD %o %s", mode & 07777, path)) < 0) return -1;
    if(ok2(code)) return 0;
  }
  return fail_reply(c, "Failed to change permissions");
}

int
ftp_size(ftp_conn_t *c, const char *path, long long *size) {
  if(c->server.protocol == FTP_PROTO_SFTP) return sftpc_size(c, path, size);
  int code;

  if(bad_path(c, path)) return -1;
  if(c->no_size) return -1;
  if(ensure_binary(c)) return -1;
  if((code = ftp_command(c, "SIZE %s", path)) < 0) return -1;
  if(code == 213) {
    *size = strtoll(c->reply + 4, NULL, 10);
    return 0;
  }
  if(code == 550) return 1;
  if(code == 500 || code == 502) c->no_size = 1;
  return -1;
}

int
ftp_set_mtime(ftp_conn_t *c, const char *path, long long mtime) {
  if(c->server.protocol == FTP_PROTO_SFTP) return sftpc_set_mtime(c, path, mtime);
  struct tm tm;
  time_t t = (time_t)mtime;
  char stamp[32];
  int code;

  if(!c->feat_mfmt || mtime < 0 || bad_path(c, path)) return -1;
  gmtime_r(&t, &tm);
  strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &tm);
  if((code = ftp_command(c, "MFMT %s %s", stamp, path)) < 0) return -1;
  return ok2(code) ? 0 : -1;
}

int
ftp_retrieve(ftp_conn_t *c, const char *path, long long offset,
             ftp_sink_cb sink, void *ctx) {
  if(c->server.protocol == FTP_PROTO_SFTP) return sftpc_retrieve(c, path, offset, sink, ctx);
  int final_seen;
  int code;
  int dfd;

  if(bad_path(c, path) || ensure_binary(c)) return -1;
  if((dfd = open_data(c)) < 0) return -1;
  if(offset > 0) {
    if((code = ftp_command(c, "REST %lld", offset)) < 0) {
      close(dfd);
      return -1;
    }
    if(code != 350) {
      close(dfd);
      return fail_reply(c, "Server does not support resume");
    }
  }
  {
    char cmd[1100];

    snprintf(cmd, sizeof(cmd), "RETR %s", path);
    if((final_seen = start_transfer(c, dfd, cmd, "Download failed")) < 0) {
      close(dfd);
      return -1;
    }
  }
  if(receive(c, dfd, sink, ctx)) return -1;
  return finish_transfer(c, final_seen, "Download failed");
}

int
ftp_store(ftp_conn_t *c, const char *path, long long offset,
          ftp_source_cb source, void *ctx) {
  if(c->server.protocol == FTP_PROTO_SFTP) return sftpc_store(c, path, offset, source, ctx);
  char cmd[1100];
  char *buf;
  int final_seen;
  int dfd;

  if(bad_path(c, path) || ensure_binary(c)) return -1;
  if((dfd = open_data(c)) < 0) return -1;
  snprintf(cmd, sizeof(cmd), "%s %s", offset > 0 ? "APPE" : "STOR", path);
  if((final_seen = start_transfer(c, dfd, cmd, "Upload failed")) < 0) {
    close(dfd);
    return -1;
  }
  if(!(buf = malloc(DATA_BUFFER_SIZE))) {
    return abort_transfer(c, dfd, "Out of memory");
  }
  for(;;) {
    ssize_t n = source(ctx, buf, DATA_BUFFER_SIZE);
    size_t sent = 0;

    if(canceled(c)) {
      free(buf);
      return abort_transfer(c, dfd, "Transfer canceled");
    }
    if(n < 0) {
      free(buf);
      return abort_transfer(c, dfd, "Could not read local file");
    }
    if(n == 0) break;
    while(sent < (size_t)n) {
      ssize_t w = send(dfd, buf + sent, (size_t)n - sent, MSG_NOSIGNAL_FLAG);

      if(w < 0 && errno == EINTR) continue;
      if(w <= 0) {
        free(buf);
        return abort_transfer(c, dfd, "Data connection failed");
      }
      sent += (size_t)w;
    }
  }
  free(buf);
  close(dfd);
  return finish_transfer(c, final_seen, "Upload failed");
}

int
ftp_ping(ftp_conn_t *c, int seconds) {
  int alive;

  if(!ftp_is_open(c)) return -1;
  c->quiet = 1;
  if(c->server.protocol == FTP_PROTO_SFTP) {
    alive = !sftpc_ping(c, seconds * 1000);
  } else {
    sock_timeouts(c->fd, seconds);
    alive = ftp_command(c, "NOOP") >= 0;  // even "500 unknown command" is an answer
    if(c->fd >= 0) sock_timeouts(c->fd, CONTROL_TIMEOUT_S);
  }
  c->quiet = 0;
  if(!alive) drop(c);
  return alive ? 0 : -1;
}

int
ftp_noop(ftp_conn_t *c) {
  int code;

  if(c->server.protocol == FTP_PROTO_SFTP) return sftpc_noop(c);
  code = ftp_command(c, "NOOP");

  return code >= 0 && code < 400 ? 0 : -1;
}
