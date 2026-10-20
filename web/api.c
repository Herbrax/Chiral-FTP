/* JSON API used by the web UI. Requests are GET query strings or
   application/x-www-form-urlencoded POST bodies; replies are JSON. */

#include "api.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "platform_config.h"
#include "form.h"
#include "fsjobs.h"
#include "ftp.h"
#include "localfs.h"
#include "system.h"
#include "log.h"
#include "queue.h"
#include "remote_ops.h"
#include "session.h"
#include "settings.h"
#include "strbuf.h"
#include "websrv_mhd.h"

#define QUEUE_ITEMS_LIMIT 300

static const char *
protocol_name(int protocol) {
  return protocol == FTP_PROTO_SFTP ? "sftp" : "ftp";
}

typedef struct req {
  struct MHD_Connection *conn;
  form_t form;
} req_t;

static const char *
arg(req_t *r, const char *key, const char *def) {
  const char *v = form_get(&r->form, key, NULL);

  if(v) return v;
  v = MHD_lookup_connection_value(r->conn, MHD_GET_ARGUMENT_KIND, key);
  return v ? v : def;
}

static long long
arg_ll(req_t *r, const char *key, long long def) {
  const char *v = arg(r, key, NULL);
  char *end;
  long long n;

  if(!v || !*v) return def;
  n = strtoll(v, &end, 10);
  return *end ? def : n;
}

static enum MHD_Result
send_json(struct MHD_Connection *conn, unsigned int status, strbuf_t *b) {
  struct MHD_Response *resp;
  enum MHD_Result ret;

  resp = MHD_create_response_from_buffer(b->len, b->data, MHD_RESPMEM_MUST_COPY);
  strbuf_free(b);
  if(!resp) return MHD_NO;
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");
  ret = websrv_queue_response(conn, status, resp);
  MHD_destroy_response(resp);
  return ret;
}

enum MHD_Result
api_error(struct MHD_Connection *conn, unsigned int status, const char *message) {
  strbuf_t b = {0};

  strbuf_append(&b, "{\"ok\":false,\"error\":");
  json_str(&b, message);
  strbuf_append(&b, "}");
  return send_json(conn, status, &b);
}

static enum MHD_Result
ok(struct MHD_Connection *conn) {
  strbuf_t b = {0};

  strbuf_append(&b, "{\"ok\":true}");
  return send_json(conn, MHD_HTTP_OK, &b);
}

static enum MHD_Result
fail(struct MHD_Connection *conn, const char *message) {
  return api_error(conn, MHD_HTTP_OK, message);
}

static enum MHD_Result
fail_errno(struct MHD_Connection *conn, const char *what) {
  char msg[256];

  snprintf(msg, sizeof(msg), "%s: %s", what, strerror(errno));
  return fail(conn, msg);
}

static int
valid_path(const char *p) {
  return p && p[0] == '/' && !strpbrk(p, "\r\n");
}

/* ---- state, settings, store ---------------------------------------- */

static enum MHD_Result
api_state(req_t *r) {
  strbuf_t b = {0};
  ftp_server_t s;
  int connected = session_server(&s);

  strbuf_printf(&b, "{\"ok\":true,\"app\":\"%s\",\"version\":\"%s\",\"connected\":%d",
                APP_NAME, VERSION_TAG, connected);
  if(connected) {
    strbuf_append(&b, ",\"server\":{\"host\":");
    json_str(&b, s.host);
    strbuf_printf(&b, ",\"port\":%u,\"protocol\":\"%s\",\"user\":", s.port,
                  protocol_name(s.protocol));
    json_str(&b, s.user);
    strbuf_append(&b, "}");
  }
  strbuf_append(&b, ",\"settings\":");
  settings_json(&b);
  strbuf_printf(&b, ",\"platform\":\"%s\"}", CHIRAL_PLATFORM_ID);
  return send_json(r->conn, MHD_HTTP_OK, &b);
}

static enum MHD_Result
api_settings(req_t *r) {
  settings_t s = settings_get();
  strbuf_t b = {0};

  if(arg(r, "connections", NULL)) {
    s.connections = (int)arg_ll(r, "connections", s.connections);
    s.policy = (int)arg_ll(r, "policy", s.policy);
    s.preserve_mtime = (int)arg_ll(r, "preserve_mtime", s.preserve_mtime);
    s.retries = (int)arg_ll(r, "retries", s.retries);
    if(settings_set(&s)) log_msg(0, LOG_ERROR, "Could not save settings");
    queue_kick();
  }
  strbuf_append(&b, "{\"ok\":true,\"settings\":");
  settings_json(&b);
  strbuf_append(&b, "}");
  return send_json(r->conn, MHD_HTTP_OK, &b);
}

static enum MHD_Result
api_store(req_t *r) {
  const char *name = arg(r, "name", "");
  const char *data = form_get(&r->form, "data", NULL);
  strbuf_t b = {0};

  if(data) {
    if(store_write(name, data, strlen(data))) return fail_errno(r->conn, "Could not save");
    return ok(r->conn);
  }
  if(store_read(name, &b)) {
    strbuf_free(&b);
    return fail(r->conn, "Unknown store");
  }
  return send_json(r->conn, MHD_HTTP_OK, &b);
}

/* ---- connection ------------------------------------------------------ */

static void
server_from_args(req_t *r, ftp_server_t *s) {
  memset(s, 0, sizeof(*s));
  s->protocol = !strcmp(arg(r, "protocol", "ftp"), "sftp") ? FTP_PROTO_SFTP : FTP_PROTO_FTP;
  snprintf(s->host, sizeof(s->host), "%s", arg(r, "host", ""));
  snprintf(s->keyfile, sizeof(s->keyfile), "%s", arg(r, "keyfile", ""));
  s->port = (unsigned short)arg_ll(r, "port", 0);
  snprintf(s->user, sizeof(s->user), "%s", arg(r, "user", ""));
  snprintf(s->pass, sizeof(s->pass), "%s", arg(r, "pass", ""));
  s->pasv_mode = (int)arg_ll(r, "pasv_mode", FTP_PASV_AUTO);
  s->show_hidden = (int)arg_ll(r, "show_hidden", 1);
  /* Accept "ftp://host:port" or "sftp://host:port" pasted into the host field. */
  if(!strncmp(s->host, "ftp://", 6)) {
    memmove(s->host, s->host + 6, strlen(s->host + 6) + 1);
    s->protocol = FTP_PROTO_FTP;
  } else if(!strncmp(s->host, "sftp://", 7)) {
    memmove(s->host, s->host + 7, strlen(s->host + 7) + 1);
    s->protocol = FTP_PROTO_SFTP;
  }
  {
    char *slash = strchr(s->host, '/');
    char *colon = strrchr(s->host, ':');

    if(slash) *slash = 0;
    if(colon && strchr(s->host, ':') == colon) {
      s->port = (unsigned short)atoi(colon + 1);
      *colon = 0;
    }
  }
  if(!s->port) s->port = s->protocol == FTP_PROTO_SFTP ? 22 : 21;
}


static enum MHD_Result
api_connect(req_t *r) {
  ftp_server_t s;
  char home[PATH_MAX];
  char err[512];
  strbuf_t b = {0};

  server_from_args(r, &s);
  if(!s.host[0]) return fail(r->conn, "Enter a host name or IP address");
  if(strpbrk(s.user, "\r\n") || strpbrk(s.pass, "\r\n")) {
    return fail(r->conn, "Invalid characters in user or password");
  }
  if(session_connect(&s, home, sizeof(home), err, sizeof(err))) return fail(r->conn, err);
  /* The engine may have switched protocol (an SSH port typed as FTP). */
  session_server(&s);
  strbuf_append(&b, "{\"ok\":true,\"path\":");
  json_str(&b, home);
  strbuf_printf(&b, ",\"protocol\":\"%s\",\"port\":%u}", protocol_name(s.protocol), s.port);
  return send_json(r->conn, MHD_HTTP_OK, &b);
}

/* ---- remote ---------------------------------------------------------- */

typedef struct list_ctx {
  const char *path;
  ftp_listing_t listing;
  char resolved[PATH_MAX];
} list_ctx_t;

static int
do_list(ftp_conn_t *c, void *p) {
  list_ctx_t *ctx = p;

  ftp_listing_free(&ctx->listing);
  return ftp_list(c, ctx->path, &ctx->listing, ctx->resolved, sizeof(ctx->resolved));
}

static enum MHD_Result
api_remote_list(req_t *r) {
  list_ctx_t ctx;
  char err[512];
  strbuf_t b = {0};

  memset(&ctx, 0, sizeof(ctx));
  ctx.path = arg(r, "path", "");
  if(session_run(do_list, &ctx, err, sizeof(err))) {
    ftp_listing_free(&ctx.listing);
    return fail(r->conn, err);
  }
  strbuf_append(&b, "{\"ok\":true,\"path\":");
  json_str(&b, ctx.resolved);
  strbuf_append(&b, ",\"entries\":[");
  for(size_t i = 0; i < ctx.listing.count; i++) {
    ftp_entry_t *e = &ctx.listing.entries[i];

    strbuf_printf(&b, "%s{\"name\":", i ? "," : "");
    json_str(&b, e->name);
    strbuf_printf(&b, ",\"dir\":%d,\"link\":%d,\"size\":%lld,\"mtime\":%lld,\"perm\":",
                  e->is_dir, e->is_link, e->size, e->mtime);
    json_str(&b, e->perm);
    strbuf_append(&b, ",\"owner\":");
    json_str(&b, e->owner);
    strbuf_append(&b, ",\"group\":");
    json_str(&b, e->group);
    if(e->target[0]) {
      strbuf_append(&b, ",\"target\":");
      json_str(&b, e->target);
    }
    strbuf_append(&b, "}");
  }
  strbuf_append(&b, "]}");
  ftp_listing_free(&ctx.listing);
  return send_json(r->conn, MHD_HTTP_OK, &b);
}

typedef struct path_ctx {
  const char *a;
  const char *b;
  unsigned int mode;
  int (*op)(ftp_conn_t *, const char *);
} path_ctx_t;

static int
do_path_op(ftp_conn_t *c, void *p) {
  path_ctx_t *ctx = p;
  return ctx->op(c, ctx->a);
}

static int
do_rename(ftp_conn_t *c, void *p) {
  path_ctx_t *ctx = p;
  return ftp_rename(c, ctx->a, ctx->b);
}

static int
do_chmod(ftp_conn_t *c, void *p) {
  path_ctx_t *ctx = p;
  return ftp_chmod(c, ctx->a, ctx->mode);
}

static enum MHD_Result
run_path(req_t *r, session_fn fn, path_ctx_t *ctx) {
  char err[512];

  if(!valid_path(ctx->a) || (ctx->b && !valid_path(ctx->b))) {
    return fail(r->conn, "Invalid path");
  }
  if(session_run(fn, ctx, err, sizeof(err))) return fail(r->conn, err);
  return ok(r->conn);
}

static enum MHD_Result
api_remote_mkdir(req_t *r) {
  path_ctx_t ctx = {arg(r, "path", NULL), NULL, 0, ftp_mkdir};
  return run_path(r, do_path_op, &ctx);
}

static enum MHD_Result
api_remote_rename(req_t *r) {
  path_ctx_t ctx = {arg(r, "from", NULL), arg(r, "to", NULL), 0, NULL};
  return run_path(r, do_rename, &ctx);
}

static enum MHD_Result
api_remote_chmod(req_t *r) {
  path_ctx_t ctx = {arg(r, "path", NULL), NULL,
                    (unsigned int)strtoul(arg(r, "mode", "0"), NULL, 8), NULL};
  return run_path(r, do_chmod, &ctx);
}

typedef struct delete_ctx {
  char *paths;
} delete_ctx_t;

/* paths: lines of "d:/path" or "f:/path" */
static int
do_delete(ftp_conn_t *c, void *p) {
  delete_ctx_t *ctx = p;
  char *save = NULL;
  char *copy = strdup(ctx->paths);
  int ret = 0;

  if(!copy) return -1;
  for(char *line = strtok_r(copy, "\n", &save); line && !ret;
      line = strtok_r(NULL, "\n", &save)) {
    if(strlen(line) < 3 || line[1] != ':' || !valid_path(line + 2) || !strcmp(line + 2, "/")) {
      continue;
    }
    ret = line[0] == 'd' ? remote_delete_tree(c, line + 2) : ftp_delete(c, line + 2);
  }
  free(copy);
  return ret;
}

static enum MHD_Result
api_remote_delete(req_t *r) {
  delete_ctx_t ctx = {(char *)arg(r, "paths", "")};
  char err[512];

  if(session_run(do_delete, &ctx, err, sizeof(err))) return fail(r->conn, err);
  return ok(r->conn);
}

typedef struct raw_ctx {
  const char *cmd;
  int code;
  char reply[4096];
} raw_ctx_t;

static int
do_raw(ftp_conn_t *c, void *p) {
  raw_ctx_t *ctx = p;

  if((ctx->code = ftp_command(c, "%s", ctx->cmd)) < 0) return -1;
  snprintf(ctx->reply, sizeof(ctx->reply), "%s", c->reply);
  return 0;
}

static enum MHD_Result
api_remote_command(req_t *r) {
  raw_ctx_t ctx;
  char err[512];
  strbuf_t b = {0};

  memset(&ctx, 0, sizeof(ctx));
  ctx.cmd = arg(r, "cmd", "");
  if(!ctx.cmd[0]) return fail(r->conn, "Empty command");
  if(session_run(do_raw, &ctx, err, sizeof(err))) return fail(r->conn, err);
  strbuf_printf(&b, "{\"ok\":true,\"code\":%d,\"reply\":", ctx.code);
  json_str(&b, ctx.reply);
  strbuf_append(&b, "}");
  return send_json(r->conn, MHD_HTTP_OK, &b);
}

/* ---- local ----------------------------------------------------------- */

static enum MHD_Result
api_local_list(req_t *r) {
  const char *path = arg(r, "path", "/");
  char real[PATH_MAX];
  strbuf_t b = {0};

  if(!valid_path(path)) return fail(r->conn, "Invalid path");
  /* Not plain realpath(): on the Switch that comes back with a device
     prefix ("sdmc:/..."), the browser stores it as its current folder, and
     every path built from it afterwards fails valid_path(). */
  localfs_realpath(path, real, sizeof(real));
  strbuf_append(&b, "{\"ok\":true,\"path\":");
  json_str(&b, real);
  /* -1 where the platform won't say (see system_free_space); the page
     leaves the figure out then. */
  strbuf_printf(&b, ",\"free\":%lld", system_free_space(real, NULL));
  strbuf_append(&b, ",\"entries\":");
  if(localfs_list_json(&b, real)) {
    strbuf_free(&b);
    return fail_errno(r->conn, "Could not open folder");
  }
  strbuf_append(&b, "}");
  return send_json(r->conn, MHD_HTTP_OK, &b);
}

static enum MHD_Result
api_local_roots(req_t *r) {
  strbuf_t b = {0};

  strbuf_append(&b, "{\"ok\":true,\"roots\":");
  localfs_roots_json(&b);
  strbuf_append(&b, "}");
  return send_json(r->conn, MHD_HTTP_OK, &b);
}

static enum MHD_Result
api_local_mkdir(req_t *r) {
  const char *path = arg(r, "path", NULL);

  if(!valid_path(path)) return fail(r->conn, "Invalid path");
  if(localfs_mkdir(path)) return fail_errno(r->conn, "Could not create folder");
  return ok(r->conn);
}

static enum MHD_Result
api_local_rename(req_t *r) {
  const char *from = arg(r, "from", NULL);
  const char *to = arg(r, "to", NULL);

  if(!valid_path(from) || !valid_path(to)) return fail(r->conn, "Invalid path");
  if(localfs_rename(from, to)) return fail_errno(r->conn, "Could not rename");
  return ok(r->conn);
}

static enum MHD_Result
api_local_delete(req_t *r) {
  char *copy = strdup(arg(r, "paths", ""));
  char *save = NULL;
  int failed = 0;

  if(!copy) return fail(r->conn, "Out of memory");
  for(char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    if(!valid_path(line) || !strcmp(line, "/")) continue;
    if(localfs_delete(line)) failed = 1;
  }
  free(copy);
  return failed ? fail_errno(r->conn, "Some items could not be deleted") : ok(r->conn);
}

static enum MHD_Result
api_local_chmod(req_t *r) {
  const char *path = arg(r, "path", NULL);

  if(!valid_path(path)) return fail(r->conn, "Invalid path");
  if(chmod(path, (mode_t)strtoul(arg(r, "mode", "0"), NULL, 8))) {
    return fail_errno(r->conn, "Could not change permissions");
  }
  return ok(r->conn);
}

/* ---- file manager ----------------------------------------------------- */

/* Sends a PS5 file to the browser as a download. */
static enum MHD_Result
api_fs_download(req_t *r) {
  const char *path = arg(r, "path", NULL);
  const char *base;
  struct MHD_Response *resp;
  struct stat st;
  enum MHD_Result ret;
  strbuf_t cd = {0};
  int fd;

  if(!valid_path(path)) return fail(r->conn, "Invalid path");
  if((fd = open(path, O_RDONLY)) < 0) return fail_errno(r->conn, "Could not open the file");
  if(fstat(fd, &st) || S_ISDIR(st.st_mode)) {
    close(fd);
    return fail(r->conn, "Not a file");
  }
  if(!(resp = MHD_create_response_from_fd64((uint64_t)st.st_size, fd))) {
    close(fd);
    return api_error(r->conn, 500, "Out of memory");
  }
  /* filename*= carries any name; the plain filename is an ASCII fallback. */
  base = path_basename(path);
  strbuf_append(&cd, "attachment; filename=\"");
  for(const char *c = base; *c; c++) strbuf_printf(&cd, "%c", (*c < 32 || *c > 126 || *c == '"' || *c == '\\') ? '_' : *c);
  strbuf_append(&cd, "\"; filename*=UTF-8''");
  for(const unsigned char *c = (const unsigned char *)base; *c; c++) {
    if((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || strchr("-._~", *c)) {
      strbuf_printf(&cd, "%c", *c);
    } else {
      strbuf_printf(&cd, "%%%02X", *c);
    }
  }
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "application/octet-stream");
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_DISPOSITION, cd.data ? cd.data : "attachment");
  strbuf_free(&cd);
  ret = websrv_queue_response(r->conn, MHD_HTTP_OK, resp);
  MHD_destroy_response(resp);
  return ret;
}

/* paths: one per line; each is copied (or moved) into the folder target. */
static enum MHD_Result
api_fs_copy(req_t *r) {
  const char *target = arg(r, "target", NULL);
  int move = arg_ll(r, "move", 0) == 1;
  char *copy = strdup(arg(r, "paths", ""));
  char *save = NULL;
  int added = 0;
  struct stat st;

  if(!copy) return fail(r->conn, "Out of memory");
  if(!valid_path(target) || stat(target, &st) || !S_ISDIR(st.st_mode)) {
    free(copy);
    return fail(r->conn, "Choose a folder to copy into");
  }
  for(char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    if(!valid_path(line) || !strcmp(line, "/")) continue;
    if(!fsjob_add(move, line, target)) added++;
  }
  free(copy);
  if(!added) return fail(r->conn, "Nothing to copy");
  return ok(r->conn);
}

static enum MHD_Result
api_fs_jobs(req_t *r) {
  fsjob_stats_t st;
  strbuf_t b = {0};

  fsjob_get_stats(&st);
  strbuf_printf(&b, "{\"ok\":true,\"running\":%d,\"queued\":%d,\"move\":%d,\"done\":%lld,\"total\":%lld,"
                "\"speed\":%.0f,\"finished\":%lu,\"failed\":%d,\"current\":",
                st.running, st.queued, st.move, st.done, st.total, st.speed, st.finished, st.failed);
  json_str(&b, st.current);
  strbuf_append(&b, ",\"target\":");
  json_str(&b, st.target);
  strbuf_append(&b, ",\"error\":");
  json_str(&b, st.error);
  strbuf_append(&b, "}");
  return send_json(r->conn, MHD_HTTP_OK, &b);
}

/* ---- queue ----------------------------------------------------------- */

/* items: lines of "<d|f>\t<source path>\t<size>\t<mtime>"; each source is
   copied into target, the destination folder on the other side. */
static enum MHD_Result
api_queue_add(req_t *r) {
  int upload = !strcmp(arg(r, "direction", ""), "upload");
  const char *target = arg(r, "target", NULL);
  char *copy;
  char *save = NULL;
  ftp_server_t s;
  int added = 0;
  strbuf_t b = {0};

  if(!session_server(&s)) return fail(r->conn, "Not connected");
  if(!valid_path(target)) return fail(r->conn, "Invalid target folder");
  if(!(copy = strdup(arg(r, "items", "")))) return fail(r->conn, "Out of memory");
  for(char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    char *fields[4] = {0};
    char *fsave = NULL;
    char dest[PATH_MAX];
    int n = 0;

    for(char *f = strtok_r(line, "\t", &fsave); f && n < 4; f = strtok_r(NULL, "\t", &fsave)) {
      fields[n++] = f;
    }
    if(n < 2 || !valid_path(fields[1])) continue;
    if(!path_is_safe_name(path_basename(fields[1]))) continue;
    path_join(dest, sizeof(dest), target, path_basename(fields[1]));
    if(!queue_add(upload ? QUEUE_UPLOAD : QUEUE_DOWNLOAD, &s,
                  upload ? fields[1] : dest, upload ? dest : fields[1],
                  fields[0][0] == 'd', n > 2 ? strtoll(fields[2], NULL, 10) : -1,
                  n > 3 ? strtoll(fields[3], NULL, 10) : -1)) {
      added++;
    }
  }
  free(copy);
  strbuf_printf(&b, "{\"ok\":true,\"added\":%d}", added);
  return send_json(r->conn, MHD_HTTP_OK, &b);
}

static enum MHD_Result
api_poll(req_t *r) {
  unsigned long since = (unsigned long)arg_ll(r, "log", 0);
  const char *tab = arg(r, "tab", "queued");
  ftp_server_t s;
  strbuf_t b = {0};
  unsigned long last;

  strbuf_printf(&b, "{\"ok\":true,\"connected\":%d,\"log\":", session_server(&s));
  last = log_json(&b, since);
  strbuf_printf(&b, ",\"log_seq\":%lu,\"queue\":", last);
  queue_json(&b, tab, QUEUE_ITEMS_LIMIT);
  strbuf_append(&b, "}");
  return send_json(r->conn, MHD_HTTP_OK, &b);
}

static enum MHD_Result
api_queue_action(req_t *r, const char *action) {
  unsigned long id = (unsigned long)arg_ll(r, "id", 0);

  if(!strcmp(action, "cancel")) queue_cancel(id);
  else if(!strcmp(action, "retry")) queue_retry(id);
  else if(!strcmp(action, "remove")) queue_remove(id);
  else if(!strcmp(action, "clear")) queue_clear(arg(r, "tab", "done"));
  else if(!strcmp(action, "pause")) queue_set_paused((int)arg_ll(r, "paused", 1));
  else return fail(r->conn, "Unknown queue action");
  return ok(r->conn);
}

/* ---- dispatch -------------------------------------------------------- */

enum MHD_Result
api_request(struct MHD_Connection *conn, const char *url, const char *body,
            size_t size) {
  enum MHD_Result ret;
  req_t r;

  r.conn = conn;
  if(form_parse(&r.form, body, size)) return api_error(conn, 500, "Out of memory");

  if(!strcmp(url, "/api/state")) ret = api_state(&r);
  else if(!strcmp(url, "/api/poll")) ret = api_poll(&r);
  else if(!strcmp(url, "/api/settings")) ret = api_settings(&r);
  else if(!strcmp(url, "/api/store")) ret = api_store(&r);
  else if(!strcmp(url, "/api/connect")) ret = api_connect(&r);
  else if(!strcmp(url, "/api/disconnect")) {
    session_disconnect();
    ret = ok(conn);
  }
  else if(!strcmp(url, "/api/remote/list")) ret = api_remote_list(&r);
  else if(!strcmp(url, "/api/remote/mkdir")) ret = api_remote_mkdir(&r);
  else if(!strcmp(url, "/api/remote/rename")) ret = api_remote_rename(&r);
  else if(!strcmp(url, "/api/remote/delete")) ret = api_remote_delete(&r);
  else if(!strcmp(url, "/api/remote/chmod")) ret = api_remote_chmod(&r);
  else if(!strcmp(url, "/api/remote/command")) ret = api_remote_command(&r);
  else if(!strcmp(url, "/api/local/list")) ret = api_local_list(&r);
  else if(!strcmp(url, "/api/local/roots")) ret = api_local_roots(&r);
  else if(!strcmp(url, "/api/local/mkdir")) ret = api_local_mkdir(&r);
  else if(!strcmp(url, "/api/local/rename")) ret = api_local_rename(&r);
  else if(!strcmp(url, "/api/local/delete")) ret = api_local_delete(&r);
  else if(!strcmp(url, "/api/local/chmod")) ret = api_local_chmod(&r);
  else if(!strcmp(url, "/api/fs/download")) ret = api_fs_download(&r);
  else if(!strcmp(url, "/api/fs/copy")) ret = api_fs_copy(&r);
  else if(!strcmp(url, "/api/fs/jobs")) ret = api_fs_jobs(&r);
  else if(!strcmp(url, "/api/fs/cancel")) {
    fsjob_cancel_all();
    ret = ok(conn);
  }
  else if(!strcmp(url, "/api/queue/add")) ret = api_queue_add(&r);
  else if(!strncmp(url, "/api/queue/", 11)) ret = api_queue_action(&r, url + 11);
  else if(!strcmp(url, "/api/hostkeys/forget")) {
    ret = ftp_forget_host_keys() ? fail_errno(conn, "Could not remove saved host keys") : ok(conn);
  }
  else if(!strcmp(url, "/api/log/clear")) {
    log_clear();
    ret = ok(conn);
  }
  else if(!strcmp(url, "/api/exit")) {
    ret = ok(conn);
    log_msg(0, LOG_STATUS, "Exit requested");
    websrv_stop();
  }
  else ret = api_error(conn, MHD_HTTP_NOT_FOUND, "Unknown API");

  form_free(&r.form);
  return ret;
}
