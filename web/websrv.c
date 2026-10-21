/* HTTP front end. The libmicrohttpd setup follows ps5-web-file-manager and
   ps5-payload-dev/websrv, which are known to work with the PS5 network stack.
   In chiral-ftp it runs on a thread of the app: accept() is polled so a stop
   request (from the app or the web UI's Stop button) ends it promptly. */

#include "websrv_mhd.h"

#include "platform_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "api.h"
#include "config.h"
#include "asset.h"
#include "localfs.h"
#include "log.h"
#include "strbuf.h"

#define REQUEST_BODY_MAX (4 * 1024 * 1024)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static volatile int g_stop;
static int g_running;
static int g_listen_fd = -1;
static unsigned short g_port;
static int g_local_only;  /* websrv_set_local_only() */
static time_t g_health_checked;

typedef struct request_context {
  char *body;
  size_t size;
  int too_large;
  /* A file upload from the browser streams to disk instead. */
  int upload;
  int fd;
  int err;
  long long written;
  char tmp[PATH_MAX];
  char final[PATH_MAX];
} request_context_t;

enum MHD_Result
websrv_queue_response(struct MHD_Connection *conn, unsigned int status,
                      struct MHD_Response *resp) {
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
#if CHIRAL_WEB_MHD_OWNS_LISTENER
  /* One request per connection. With keep-alive, MHD holds a connection --
     and, in thread-per-connection mode, its thread -- until the timeout
     expires, and a browser opens half a dozen at once for one page. On
     Horizon that was enough to stall the next request behind an idle one
     for the full timeout: the drive list took two minutes to appear, which
     is exactly what the 120 s timeout used to be. Closing each connection
     costs a handshake per request on a LAN and nothing else. */
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONNECTION, "close");
#endif
  return MHD_queue_response(conn, status, resp);
}

static enum MHD_Result
reply_json(struct MHD_Connection *conn, unsigned int status, strbuf_t *b) {
  struct MHD_Response *resp =
      MHD_create_response_from_buffer(b->len, b->data ? b->data : (char *)"", MHD_RESPMEM_MUST_COPY);
  enum MHD_Result ret;

  strbuf_free(b);
  if(!resp) return MHD_NO;
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");
  ret = websrv_queue_response(conn, status, resp);
  MHD_destroy_response(resp);
  return ret;
}

/* PUT/POST /api/fs/upload?dir=/data/x&name=file.bin[&overwrite=1], with the
   file as the raw body (the web UI's upload button and drag and drop). The
   data goes to "<name>.chiral-upload" and is renamed when complete, so an
   interrupted upload never leaves a half-written file under the real name. */
static void
upload_open(struct MHD_Connection *conn, request_context_t *ctx) {
  const char *dir = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "dir");
  const char *name = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "name");
  const char *overwrite = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "overwrite");
  struct stat st;

  ctx->upload = 1;
  ctx->fd = -1;
  if(!dir || dir[0] != '/' || strpbrk(dir, "\r\n") || !name || !path_is_safe_name(name)) {
    ctx->err = EINVAL;
    return;
  }
  if(stat(dir, &st) || !S_ISDIR(st.st_mode)) {
    ctx->err = errno ? errno : ENOTDIR;
    return;
  }
  if(overwrite && !strcmp(overwrite, "1")) path_join(ctx->final, sizeof(ctx->final), dir, name);
  else if(localfs_free_name(ctx->final, sizeof(ctx->final), dir, name)) {
    ctx->err = EEXIST;
    return;
  }
  if(snprintf(ctx->tmp, sizeof(ctx->tmp), "%s.chiral-upload", ctx->final) >= (int)sizeof(ctx->tmp)) {
    ctx->err = ENAMETOOLONG;
    return;
  }
  if((ctx->fd = open(ctx->tmp, O_WRONLY | O_CREAT | O_TRUNC, CHIRAL_FILE_MODE)) < 0) ctx->err = errno;
  else fchmod(ctx->fd, CHIRAL_FILE_MODE);  // regardless of the umask
}

static void
upload_write(request_context_t *ctx, const char *data, size_t n) {
  while(n && !ctx->err && ctx->fd >= 0) {
    ssize_t w = write(ctx->fd, data, n);
    if(w < 0 && errno == EINTR) continue;
    if(w <= 0) {
      ctx->err = w < 0 ? errno : ENOSPC;
      break;
    }
    data += w;
    n -= (size_t)w;
    ctx->written += w;
  }
}

static enum MHD_Result
upload_finish(struct MHD_Connection *conn, request_context_t *ctx) {
  strbuf_t b = {0};

  if(ctx->fd >= 0 && close(ctx->fd) && !ctx->err) ctx->err = errno;
  ctx->fd = -1;
  if(!ctx->err && rename(ctx->tmp, ctx->final)) ctx->err = errno;
  if(ctx->err) {
    if(ctx->tmp[0]) unlink(ctx->tmp);
    log_msg(0, LOG_ERROR, "Upload from the browser failed: %s", strerror(ctx->err));
    strbuf_append(&b, "{\"ok\":false,\"error\":");
    json_str(&b, ctx->err == EINVAL ? "Invalid folder or file name" : strerror(ctx->err));
    strbuf_append(&b, "}");
    return reply_json(conn, ctx->err == EINVAL ? 400 : 500, &b);
  }
  log_msg(0, LOG_STATUS, "Uploaded from the browser: %s (%lld bytes)", ctx->final, ctx->written);
  strbuf_append(&b, "{\"ok\":true,\"path\":");
  json_str(&b, ctx->final);
  strbuf_printf(&b, ",\"size\":%lld}", ctx->written);
  return reply_json(conn, 200, &b);
}

static enum MHD_Result
on_request(void *cls, struct MHD_Connection *conn, const char *url,
           const char *method, const char *version, const char *upload_data,
           size_t *upload_data_size, void **con_cls) {
  request_context_t *ctx = *con_cls;

  (void)cls;
  (void)version;
  if(strcmp(method, MHD_HTTP_METHOD_GET) && strcmp(method, MHD_HTTP_METHOD_POST) &&
     strcmp(method, MHD_HTTP_METHOD_HEAD) && strcmp(method, MHD_HTTP_METHOD_PUT)) {
    return MHD_NO;
  }
  if(!ctx) {
    if(!(ctx = calloc(1, sizeof(*ctx)))) return MHD_NO;
    ctx->fd = -1;
    *con_cls = ctx;
    if(!strcmp(url, "/api/fs/upload")) upload_open(conn, ctx);
    return MHD_YES;
  }
  if(ctx->upload) {
    if(*upload_data_size) {
      upload_write(ctx, upload_data, *upload_data_size);
      *upload_data_size = 0;
      return MHD_YES;
    }
    return upload_finish(conn, ctx);
  }
  if(*upload_data_size) {
    size_t n = *upload_data_size;

    if(n > REQUEST_BODY_MAX - ctx->size) {
      ctx->too_large = 1;
    } else if(!ctx->too_large) {
      char *body = realloc(ctx->body, ctx->size + n + 1);

      if(!body) return MHD_NO;
      ctx->body = body;
      memcpy(ctx->body + ctx->size, upload_data, n);
      ctx->size += n;
      ctx->body[ctx->size] = 0;
    }
    *upload_data_size = 0;
    return MHD_YES;
  }
  if(!strncmp(url, "/api/", 5)) {
    if(ctx->too_large) return api_error(conn, 413, "Request too large");
    return api_request(conn, url, ctx->body, ctx->size);
  }
  /* The PS5 file manager opens first; the FTP client is index.html. */
  if(!strcmp(url, "/") || !url[0]) return asset_request(conn, "/files.html");
  /* The brand mark doubles as the favicon; the PS5 build served its
     dashboard icon0.png, which an .nro hasn't got. */
  if(!strcmp(url, "/favicon.ico")) return asset_request(conn, "/mark.svg");
  return asset_request(conn, url);
}

static void
on_completed(void *cls, struct MHD_Connection *conn, void **con_cls,
             enum MHD_RequestTerminationCode toe) {
  request_context_t *ctx = *con_cls;

  (void)cls;
  (void)conn;
  (void)toe;
  if(ctx) {
    if(ctx->fd >= 0) {  // the browser went away mid-upload
      close(ctx->fd);
      unlink(ctx->tmp);
    }
    free(ctx->body);
    free(ctx);
  }
  *con_cls = NULL;
}

/* libmicrohttpd's own complaints, into the app's message log. MHD says a
   lot more about why it is unhappy than its return value does. */
static void
mhd_log(void *cls, const char *fmt, va_list args) {
  char line[400];

  (void)cls;
  vsnprintf(line, sizeof(line), fmt, args);
  /* MHD ends its messages with a newline; the log adds its own. */
  for(char *p = line + strlen(line); p > line && (p[-1] == '\n' || p[-1] == '\r'); p--) p[-1] = 0;
  if(line[0]) log_msg(0, LOG_ERROR, "Web server: %s", line);
}

static void *
serve(void *arg) {
  struct MHD_Daemon *httpd;
  int srvfd = (int)(long)arg;

#if CHIRAL_WEB_MHD_OWNS_LISTENER
  /* Let libmicrohttpd own the listen socket.
   *
   * The alternative, below, is to accept connections ourselves and hand
   * them over with MHD_add_connection -- which the PS5 needs, because a
   * sandboxed title has to set the socket up its own way. That combination
   * (MHD_USE_NO_LISTEN_SOCKET plus an external accept loop) is a corner of
   * libmicrohttpd very little code exercises, and on Horizon
   * MHD_start_daemon died inside it. Where we can, we run it the way
   * everyone else does: MHD binds, listens, accepts and spawns a thread
   * per connection, and this thread only waits for the stop.
   *
   * MHD_USE_POLL because Horizon has poll() and MHD's default is select(),
   * whose fd_set is a fixed-size bitmap this many connections could walk
   * off the end of.
   *
   * MHD_USE_ITC is not passed, but MHD builds an inter-thread channel
   * anyway -- its polling thread has no other way to be woken -- so
   * engine/nx_net.c's socketpair has to work regardless. */
  (void)srvfd;
  httpd = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION | MHD_USE_INTERNAL_POLLING_THREAD |
                               MHD_USE_POLL | MHD_USE_ERROR_LOG,
                           g_port, NULL, NULL, &on_request, NULL,
                           /* First, or MHD says so and logs its early
                              messages somewhere we cannot see them. */
                           MHD_OPTION_EXTERNAL_LOGGER, &mhd_log, NULL,
                           /* Short, because a stuck connection should not be
                              able to hold the page up for long. */
                           MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)15,
                           /* Said outright: MHD's own default comes from
                              FD_SETSIZE, which is not a number worth
                              inheriting on a platform it was not written
                              for. A browser wants about six at a time. */
                           MHD_OPTION_CONNECTION_LIMIT, (unsigned int)32,
                           MHD_OPTION_NOTIFY_COMPLETED, &on_completed, NULL,
                           /* Say the stack size outright rather than inherit
                              whatever newlib hands a pthread: MHD runs one
                              thread per connection, and api.c's handlers are
                              not shy with locals. */
                           MHD_OPTION_THREAD_STACK_SIZE, (size_t)(128 * 1024),
                           MHD_OPTION_END);
  if(!httpd) {
    log_msg(0, LOG_ERROR, "Web server: could not start the HTTP daemon");
    pthread_mutex_lock(&g_lock);
    g_listen_fd = -1;
    g_running = 0;
    pthread_mutex_unlock(&g_lock);
    return NULL;
  }
  log_msg(0, LOG_STATUS, "Web server started on port %u", g_port);
  /* MHD does the accepting; this thread exists only to notice the stop. */
  while(!g_stop) usleep(200 * 1000);
  MHD_stop_daemon(httpd);
  log_msg(0, LOG_STATUS, "Web server stopped");
#else
  unsigned int flags = MHD_USE_THREAD_PER_CONNECTION | MHD_USE_NO_LISTEN_SOCKET |
                       MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG | MHD_USE_ITC;

  if(!(httpd = MHD_start_daemon(flags,
                                0, NULL, NULL, &on_request, NULL,
                                /* First, or MHD warns that it isn't. */
                                MHD_OPTION_EXTERNAL_LOGGER, &mhd_log, NULL,
                                MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)120,
                                MHD_OPTION_NOTIFY_COMPLETED, &on_completed, NULL,
                                MHD_OPTION_END))) {
    log_msg(0, LOG_ERROR, "Web server: could not start the HTTP daemon");
    close(srvfd);
    pthread_mutex_lock(&g_lock);
    g_listen_fd = -1;
    g_running = 0;
    pthread_mutex_unlock(&g_lock);
    return NULL;
  }
  log_msg(0, LOG_STATUS, "Web server started on port %u", g_port);
  while(!g_stop) {
    struct pollfd pfd = {srvfd, POLLIN, 0};
    struct sockaddr_in client;
    socklen_t len = sizeof(client);
    int fd;

    if(poll(&pfd, 1, 300) <= 0) continue;
    if((fd = accept(srvfd, (struct sockaddr *)&client, &len)) < 0) continue;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
    if(MHD_add_connection(httpd, fd, (struct sockaddr *)&client, len) != MHD_YES) close(fd);
  }
  MHD_stop_daemon(httpd);
  close(srvfd);
  log_msg(0, LOG_STATUS, "Web server stopped");
#endif
  pthread_mutex_lock(&g_lock);
  g_listen_fd = -1;
  g_running = 0;
  pthread_mutex_unlock(&g_lock);
  return NULL;
}

void
websrv_set_local_only(int on) {
  pthread_mutex_lock(&g_lock);
  g_local_only = on;
  pthread_mutex_unlock(&g_lock);
}

int
websrv_start(unsigned short port) {
  struct sockaddr_in addr;
  int srvfd, err;

  signal(SIGPIPE, SIG_IGN);
  pthread_mutex_lock(&g_lock);
  if(g_running) {
    pthread_mutex_unlock(&g_lock);
    return 0;
  }
#if CHIRAL_WEB_MHD_OWNS_LISTENER
  /* libmicrohttpd binds and listens for us (see serve()), so there is no
     socket to make here. A port already in use shows up as a failed
     MHD_start_daemon instead of a failed bind. */
  (void)addr;
  srvfd = -1;
#else
  if((srvfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    err = errno;
    pthread_mutex_unlock(&g_lock);
    errno = err;
    return -1;
  }
  setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(g_local_only ? INADDR_LOOPBACK : INADDR_ANY);
  addr.sin_port = htons(port);
  if(bind(srvfd, (struct sockaddr *)&addr, sizeof(addr)) || listen(srvfd, 16)) {
    err = errno;
    close(srvfd);
    pthread_mutex_unlock(&g_lock);
    errno = err;
    return -1;
  }
#endif
  g_stop = 0;
  g_health_checked = time(NULL);
  g_port = port;
  g_listen_fd = srvfd;
  if((err = pthread_create(&g_thread, NULL, serve, (void *)(long)srvfd))) {
    if(srvfd >= 0) close(srvfd);
    g_listen_fd = -1;
    pthread_mutex_unlock(&g_lock);
    errno = err;
    return -1;
  }
  pthread_detach(g_thread);
  g_running = 1;
  pthread_mutex_unlock(&g_lock);
  return 0;
}

/* Also called by the web UI's Stop button (on a server thread), so this only
   asks; the accept loop notices within 300 ms. */
void
websrv_stop(void) {
  g_stop = 1;
}

/* Is the listening socket still real? Same reasoning as the FTP server's
   health check in engine/ftpd.c: a console that has slept comes back with
   dead sockets and nothing says so, leaving the screen advertising a web
   app that answers nothing. */
#define WEBSRV_HEALTH_SECONDS 20

int
websrv_running(void) {
  int r;

  pthread_mutex_lock(&g_lock);
  if(g_running && g_listen_fd >= 0) {
    const time_t now = time(NULL);
    if(now - g_health_checked >= WEBSRV_HEALTH_SECONDS || now < g_health_checked) {
      struct sockaddr_in addr;
      socklen_t len = sizeof(addr);
      g_health_checked = now;
      if(getsockname(g_listen_fd, (struct sockaddr *)&addr, &len)) {
        log_msg(0, LOG_ERROR, "The web server's socket is gone (the console may have slept); stopping");
        g_stop = 1;
        g_running = 0;
      }
    }
  }
  r = g_running;
  pthread_mutex_unlock(&g_lock);
  return r;
}

unsigned short
websrv_port(void) {
  return g_port;
}
