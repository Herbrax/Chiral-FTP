/* SFTP backend on libssh2 (mbedTLS crypto). It plugs into the same ftp_conn_t
   as FTP, so the queue, browsing and UI work unchanged; ftp.c dispatches here
   when server.protocol is FTP_PROTO_SFTP.

   Like FileZilla: password, keyboard-interactive and key-file logins; host keys
   are remembered on first connect and a changed key is refused. */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include "config.h"
#include "ftp_internal.h"
#include "log.h"

#define SFTP_BUFFER_SIZE (512 * 1024)
#define SSH_TIMEOUT_MS   30000

#define set_error ftp_set_error

static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_hosts_lock = PTHREAD_MUTEX_INITIALIZER;

/* Entropy: devkitPro's switch-mbedtls is patched to implement
   mbedtls_hardware_poll on top of csrngGetRandomBytes, so unlike the PS5
   build there is nothing to supply here. */

static void
global_init(void) {
  libssh2_init(0);
}

static LIBSSH2_SESSION *
session(ftp_conn_t *c) {
  return (LIBSSH2_SESSION *)c->ssh;
}

static LIBSSH2_SFTP *
channel(ftp_conn_t *c) {
  return (LIBSSH2_SFTP *)c->sftp;
}

void
sftpc_drop(ftp_conn_t *c) {
  if(c->ssh) {
    /* The link may be dead; don't wait for replies while tearing down. */
    libssh2_session_set_blocking(session(c), 0);
    if(c->sftp) libssh2_sftp_shutdown(channel(c));
    libssh2_session_free(session(c));
  }
  c->sftp = NULL;
  c->ssh = NULL;
}

static void
kill_conn(ftp_conn_t *c) {
  sftpc_drop(c);
  if(c->fd >= 0) {
    close(c->fd);
    c->fd = -1;
  }
}

static const char *
fx_text(unsigned long fx) {
  switch(fx) {
  case LIBSSH2_FX_NO_SUCH_FILE: return "No such file or directory";
  case LIBSSH2_FX_PERMISSION_DENIED: return "Permission denied";
  case LIBSSH2_FX_FILE_ALREADY_EXISTS: return "File already exists";
  case LIBSSH2_FX_DIR_NOT_EMPTY: return "Directory not empty";
  case LIBSSH2_FX_NOT_A_DIRECTORY: return "Not a directory";
  case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM: return "No space left on the server";
  case LIBSSH2_FX_QUOTA_EXCEEDED: return "Quota exceeded";
  case LIBSSH2_FX_WRITE_PROTECT: return "Write protected";
  case LIBSSH2_FX_OP_UNSUPPORTED: return "Operation not supported by the server";
  default: return "Failure";
  }
}

/* Records the last libssh2 error. SFTP status errors keep the connection;
   anything else (socket, timeout, protocol) drops it so callers reconnect. */
static int
fail(ftp_conn_t *c, const char *what) {
  char *msg = NULL;
  int rc;

  if(!c->ssh) {
    set_error(c, "%s: not connected", what);
    return -1;
  }
  rc = libssh2_session_last_error(session(c), &msg, NULL, 0);
  if(rc == LIBSSH2_ERROR_SFTP_PROTOCOL && c->sftp) {
    unsigned long fx = libssh2_sftp_last_error(channel(c));

    c->code = (int)fx;
    set_error(c, "%s: %s", what, fx_text(fx));
    return -1;
  }
  c->code = -1;
  set_error(c, "%s: %s", what, rc == LIBSSH2_ERROR_TIMEOUT ? "timed out" :
            msg && *msg ? msg : "connection lost");
  kill_conn(c);
  return -1;
}

static int
canceled(const ftp_conn_t *c) {
  return c->cancel && *c->cancel;
}

static void
abspath(ftp_conn_t *c, const char *path, char *out, size_t size) {
  if(!path || !*path) {
    snprintf(out, size, "%s", c->cwd[0] ? c->cwd : "/");
  } else if(path[0] == '/') {
    snprintf(out, size, "%s", path);
  } else {
    size_t n = strlen(c->cwd);
    snprintf(out, size, "%s%s%s", c->cwd, n && c->cwd[n - 1] == '/' ? "" : "/", path);
  }
}

static void
base64(const unsigned char *in, size_t len, char *out, size_t size) {
  static const char tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t j = 0;

  for(size_t i = 0; i < len && j + 4 < size; i += 3) {
    unsigned v = (unsigned)in[i] << 16 | (i + 1 < len ? in[i + 1] << 8 : 0) |
                 (i + 2 < len ? in[i + 2] : 0);
    out[j++] = tbl[v >> 18 & 63];
    out[j++] = tbl[v >> 12 & 63];
    if(i + 1 < len) out[j++] = tbl[v >> 6 & 63];
    if(i + 2 < len) out[j++] = tbl[v & 63];
  }
  out[j] = 0;
}

static int
knownhost_keytype(int hostkey_type) {
  switch(hostkey_type) {
  case LIBSSH2_HOSTKEY_TYPE_RSA: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
  case LIBSSH2_HOSTKEY_TYPE_DSS: return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
  case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
  case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
  case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
  case LIBSSH2_HOSTKEY_TYPE_ED25519: return LIBSSH2_KNOWNHOST_KEY_ED25519;
  default: return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
  }
}

/* Trust on first use, refuse a changed key (like FileZilla, minus the prompt). */
static int
verify_host_key(ftp_conn_t *c) {
  LIBSSH2_KNOWNHOSTS *hosts;
  struct libssh2_knownhost *found = NULL;
  const unsigned char *hash;
  const char *key;
  char fp[64] = "?";
  size_t len;
  int type;
  int mask;
  int check;
  int port = c->server.port ? c->server.port : 22;
  int ret = 0;

  if(!(key = libssh2_session_hostkey(session(c), &len, &type))) {
    return fail(c, "Could not read the server's host key");
  }
  if((hash = (const unsigned char *)libssh2_hostkey_hash(session(c), LIBSSH2_HOSTKEY_HASH_SHA256))) {
    base64(hash, 32, fp, sizeof(fp));
  }
  mask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownhost_keytype(type);

  pthread_mutex_lock(&g_hosts_lock);
  if(!(hosts = libssh2_knownhost_init(session(c)))) {
    pthread_mutex_unlock(&g_hosts_lock);
    return fail(c, "Could not check the host key");
  }
  char kh[600];

  data_path(kh, sizeof(kh), KNOWN_HOSTS_NAME);
  libssh2_knownhost_readfile(hosts, kh, LIBSSH2_KNOWNHOST_FILE_OPENSSH);
  check = libssh2_knownhost_checkp(hosts, c->server.host, port, key, len, mask, &found);
  if(check == LIBSSH2_KNOWNHOST_CHECK_MATCH) {
    log_msg(c->log_id, LOG_STATUS, "Host key SHA256:%s matches the saved key", fp);
  } else if(check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
    char name[300];

    if(port == 22) snprintf(name, sizeof(name), "%s", c->server.host);
    else snprintf(name, sizeof(name), "[%s]:%d", c->server.host, port);
    libssh2_knownhost_addc(hosts, name, NULL, key, len, NULL, 0, mask, NULL);
    if(libssh2_knownhost_writefile(hosts, kh, LIBSSH2_KNOWNHOST_FILE_OPENSSH)) {
      log_msg(c->log_id, LOG_ERROR, "Could not save the host key");
    }
    log_msg(c->log_id, LOG_STATUS, "New server: trusting host key SHA256:%s", fp);
  } else if(check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
    set_error(c, "The server's host key has CHANGED (now SHA256:%s). This can mean someone "
              "is intercepting the connection. If you reinstalled the server, use "
              "Settings > Forget saved host keys, then connect again", fp);
    ret = -1;
  } else {
    set_error(c, "Could not check the host key");
    ret = -1;
  }
  libssh2_knownhost_free(hosts);
  pthread_mutex_unlock(&g_hosts_lock);
  return ret;
}

int
ftp_forget_host_keys(void) {
  int ret;

  pthread_mutex_lock(&g_hosts_lock);
  char kh[600];

  ret = unlink(data_path(kh, sizeof(kh), KNOWN_HOSTS_NAME)) && errno != ENOENT ? -1 : 0;
  pthread_mutex_unlock(&g_hosts_lock);
  return ret;
}

/* Answers every keyboard-interactive prompt (macOS asks "Password:") with the
   password. */
static void
kbd_callback(const char *name, int name_len, const char *instruction,
             int instruction_len, int num_prompts,
             const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
             LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void **abstract) {
  ftp_conn_t *c = *abstract;

  (void)name;
  (void)name_len;
  (void)instruction;
  (void)instruction_len;
  (void)prompts;
  for(int i = 0; i < num_prompts; i++) {
    responses[i].text = strdup(c->server.pass);
    responses[i].length = responses[i].text ? (unsigned int)strlen(c->server.pass) : 0;
  }
}

static int
authenticate(ftp_conn_t *c) {
  const char *user = c->server.user[0] ? c->server.user : "anonymous";
  char *methods;
  char tried[128] = "";
  char pub[300];

  methods = libssh2_userauth_list(session(c), user, (unsigned int)strlen(user));
  if(!methods) {
    if(libssh2_userauth_authenticated(session(c))) return 0;
    return fail(c, "Authentication failed");
  }
  log_msg(c->log_id, LOG_STATUS, "Server offers: %s", methods);

  if(c->server.keyfile[0] && strstr(methods, "publickey")) {
    strcat(tried, "key file ");
    log_msg(c->log_id, LOG_STATUS, "Trying key file %s", c->server.keyfile);
    /* The mbedTLS backend can't derive the public key, so use <key>.pub. */
    snprintf(pub, sizeof(pub), "%s.pub", c->server.keyfile);
    if(access(pub, R_OK)) {
      log_msg(c->log_id, LOG_ERROR, "Key file needs its public key next to it: %s", pub);
    } else if(!libssh2_userauth_publickey_fromfile(session(c), user, pub, c->server.keyfile,
                                                   c->server.pass)) {
      return 0;
    } else {
      log_msg(c->log_id, LOG_ERROR, "The server did not accept the key file");
    }
  }
  if(strstr(methods, "password")) {
    strcat(tried, "password ");
    log_msg(c->log_id, LOG_STATUS, "Trying password");
    if(!libssh2_userauth_password(session(c), user, c->server.pass)) return 0;
  }
  if(strstr(methods, "keyboard-interactive")) {
    strcat(tried, "keyboard-interactive ");
    log_msg(c->log_id, LOG_STATUS, "Trying keyboard-interactive");
    if(!libssh2_userauth_keyboard_interactive(session(c), user, kbd_callback)) return 0;
  }
  if(!tried[0]) {
    set_error(c, "Authentication failed: the server only allows %s", methods);
  } else {
    set_error(c, "Authentication failed: the server rejected the %s",
              strstr(tried, "password") ? "username or password" : "key file");
  }
  kill_conn(c);
  return -1;
}

int
sftpc_open(ftp_conn_t *c) {
  LIBSSH2_SESSION *s;
  const char *banner;
  char home[1024];
  int rc;

  pthread_once(&g_init_once, global_init);
  if(ftp_connect_socket(c, 22)) return -1;
  log_msg(c->log_id, LOG_STATUS, "Connection established, starting SSH handshake...");

  if(!(s = libssh2_session_init_ex(NULL, NULL, NULL, c))) {
    set_error(c, "Out of memory");
    kill_conn(c);
    return -1;
  }
  c->ssh = s;
  libssh2_session_set_blocking(s, 1);
  libssh2_session_set_timeout(s, SSH_TIMEOUT_MS);
  if((rc = libssh2_session_handshake(s, c->fd))) {
    /* OpenSSH's MaxStartups drops extra simultaneous handshakes; treat that
       like FTP's "421 Too many connections" so the queue backs off. */
    int dropped = rc == LIBSSH2_ERROR_SOCKET_DISCONNECT || rc == LIBSSH2_ERROR_SOCKET_RECV ||
                  rc == LIBSSH2_ERROR_SOCKET_SEND || rc == LIBSSH2_ERROR_BANNER_RECV ||
                  rc == LIBSSH2_ERROR_KEX_FAILURE;
    fail(c, "SSH handshake failed");
    if(dropped) c->code = 421;
    return -1;
  }
  if((banner = libssh2_session_banner_get(s))) {
    log_msg(c->log_id, LOG_RESPONSE, "%s", banner);
  }
  if(verify_host_key(c)) {
    kill_conn(c);
    return -1;
  }
  if(authenticate(c)) return -1;
  log_msg(c->log_id, LOG_STATUS, "Logged in");
  /* Only now: a keepalive during key exchange breaks OpenSSH >= 9.6 strict KEX. */
  libssh2_keepalive_config(s, 1, 30);

  if(!(c->sftp = libssh2_sftp_init(s))) {
    return fail(c, "The server refused the SFTP subsystem");
  }
  if(libssh2_sftp_realpath(channel(c), ".", home, sizeof(home)) <= 0) {
    snprintf(home, sizeof(home), "/");
  }
  snprintf(c->cwd, sizeof(c->cwd), "%s", home);
  c->last_activity = time(NULL);
  return 0;
}

void
sftpc_close(ftp_conn_t *c) {
  if(!c->ssh) return;
  libssh2_session_set_timeout(session(c), 2000);
  if(c->sftp) libssh2_sftp_shutdown(channel(c));
  c->sftp = NULL;
  libssh2_session_disconnect(session(c), "Goodbye");
  libssh2_session_free(session(c));
  c->ssh = NULL;
  if(c->fd >= 0) {
    close(c->fd);
    c->fd = -1;
  }
  log_msg(c->log_id, LOG_STATUS, "Disconnected from server");
}

int
sftpc_pwd(ftp_conn_t *c, char *out, size_t size) {
  snprintf(out, size, "%s", c->cwd[0] ? c->cwd : "/");
  return 0;
}

static int
resolve_dir(ftp_conn_t *c, const char *path, char *out, size_t size) {
  LIBSSH2_SFTP_ATTRIBUTES attrs;
  char abs[1024];

  abspath(c, path, abs, sizeof(abs));
  if(libssh2_sftp_realpath(channel(c), abs, out, (unsigned int)size) <= 0) {
    return fail(c, "Failed to open directory");
  }
  if(libssh2_sftp_stat(channel(c), out, &attrs)) return fail(c, "Failed to open directory");
  if((attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && !LIBSSH2_SFTP_S_ISDIR(attrs.permissions)) {
    set_error(c, "Failed to open directory: Not a directory");
    return -1;
  }
  return 0;
}

int
sftpc_cwd(ftp_conn_t *c, const char *path) {
  char resolved[1024];

  if(!c->sftp) return fail(c, "Failed to change directory");
  if(resolve_dir(c, path, resolved, sizeof(resolved))) return -1;
  snprintf(c->cwd, sizeof(c->cwd), "%s", resolved);
  return 0;
}

static void
fill_entry(ftp_entry_t *e, const LIBSSH2_SFTP_ATTRIBUTES *a) {
  if(a->flags & LIBSSH2_SFTP_ATTR_SIZE) e->size = (long long)a->filesize;
  if(a->flags & LIBSSH2_SFTP_ATTR_ACMODTIME) e->mtime = (long long)a->mtime;
  if(a->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
    e->is_dir = LIBSSH2_SFTP_S_ISDIR(a->permissions) ? 1 : 0;
    snprintf(e->perm, sizeof(e->perm), "%04lo", a->permissions & 07777);
  }
}

int
sftpc_list(ftp_conn_t *c, const char *path, ftp_listing_t *out,
          char *resolved, size_t resolved_size) {
  LIBSSH2_SFTP_HANDLE *dir;
  char name[1024];
  char longentry[2048];
  time_t now = time(NULL);

  memset(out, 0, sizeof(*out));
  if(!c->sftp) return fail(c, "Directory listing failed");
  if(resolve_dir(c, path, resolved, resolved_size)) return -1;
  snprintf(c->cwd, sizeof(c->cwd), "%s", resolved);
  log_msg(c->log_id, LOG_STATUS, "Retrieving directory listing of \"%s\"...", resolved);
  if(!(dir = libssh2_sftp_opendir(channel(c), resolved))) {
    return fail(c, "Directory listing failed");
  }
  for(;;) {
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    ftp_entry_t e;
    int n = libssh2_sftp_readdir_ex(dir, name, sizeof(name), longentry, sizeof(longentry), &attrs);

    if(n < 0) {
      libssh2_sftp_closedir(dir);
      ftp_listing_free(out);
      return fail(c, "Directory listing failed");
    }
    if(n == 0) break;
    if(!strcmp(name, ".") || !strcmp(name, "..")) continue;

    memset(&e, 0, sizeof(e));
    e.size = e.mtime = -1;
    snprintf(e.name, sizeof(e.name), "%s", name);
    fill_entry(&e, &attrs);
    if((attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISLNK(attrs.permissions)) {
      /* Follow links so linked folders open like folders. */
      char full[2048];
      LIBSSH2_SFTP_ATTRIBUTES target;
      int len;

      e.is_link = 1;
      snprintf(full, sizeof(full), "%s%s%s", resolved, strcmp(resolved, "/") ? "/" : "", name);
      if(!libssh2_sftp_stat(channel(c), full, &target)) {
        e.is_dir = (target.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                   LIBSSH2_SFTP_S_ISDIR(target.permissions);
        if(!e.is_dir && (target.flags & LIBSSH2_SFTP_ATTR_SIZE)) e.size = (long long)target.filesize;
      }
      if((len = libssh2_sftp_readlink(channel(c), full, e.target, sizeof(e.target) - 1)) > 0) {
        e.target[len] = 0;
      }
    }
    /* The "ls -l" style long entry carries the owner and group names. */
    if(longentry[0]) {
      ftp_listing_t one = {0};

      ftp_parse_listing(&one, longentry, strlen(longentry), 0, now);
      if(one.count) {
        snprintf(e.owner, sizeof(e.owner), "%s", one.entries[0].owner);
        snprintf(e.group, sizeof(e.group), "%s", one.entries[0].group);
      }
      ftp_listing_free(&one);
    }
    if(e.is_dir) e.size = -1;
    if(ftp_listing_add(out, &e)) break;
  }
  libssh2_sftp_closedir(dir);
  c->last_activity = time(NULL);
  log_msg(c->log_id, LOG_STATUS, "Directory listing of \"%s\" successful", resolved);
  return 0;
}

int
sftpc_mkdir(ftp_conn_t *c, const char *path) {
  char abs[1024];

  abspath(c, path, abs, sizeof(abs));
  log_msg(c->log_id, LOG_COMMAND, "mkdir \"%s\"", abs);
  if(!c->sftp || libssh2_sftp_mkdir(channel(c), abs, 0755)) return fail(c, "Failed to create directory");
  return 0;
}

int
sftpc_rmdir(ftp_conn_t *c, const char *path) {
  char abs[1024];

  abspath(c, path, abs, sizeof(abs));
  log_msg(c->log_id, LOG_COMMAND, "rmdir \"%s\"", abs);
  if(!c->sftp || libssh2_sftp_rmdir(channel(c), abs)) return fail(c, "Failed to remove directory");
  return 0;
}

int
sftpc_delete(ftp_conn_t *c, const char *path) {
  char abs[1024];

  abspath(c, path, abs, sizeof(abs));
  log_msg(c->log_id, LOG_COMMAND, "rm \"%s\"", abs);
  if(!c->sftp || libssh2_sftp_unlink(channel(c), abs)) return fail(c, "Failed to delete file");
  return 0;
}

int
sftpc_rename(ftp_conn_t *c, const char *from, const char *to) {
  char a[1024];
  char b[1024];

  abspath(c, from, a, sizeof(a));
  abspath(c, to, b, sizeof(b));
  log_msg(c->log_id, LOG_COMMAND, "rename \"%s\" \"%s\"", a, b);
  if(!c->sftp || libssh2_sftp_rename(channel(c), a, b)) return fail(c, "Rename failed");
  return 0;
}

int
sftpc_chmod(ftp_conn_t *c, const char *path, unsigned int mode) {
  LIBSSH2_SFTP_ATTRIBUTES attrs;
  char abs[1024];

  abspath(c, path, abs, sizeof(abs));
  log_msg(c->log_id, LOG_COMMAND, "chmod %03o \"%s\"", mode & 07777, abs);
  memset(&attrs, 0, sizeof(attrs));
  attrs.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
  attrs.permissions = mode & 07777;
  if(!c->sftp || libssh2_sftp_setstat(channel(c), abs, &attrs)) {
    return fail(c, "Failed to change permissions");
  }
  return 0;
}

int
sftpc_size(ftp_conn_t *c, const char *path, long long *size) {
  LIBSSH2_SFTP_ATTRIBUTES attrs;
  char abs[1024];

  abspath(c, path, abs, sizeof(abs));
  if(!c->sftp) return fail(c, "Could not read file size");
  if(libssh2_sftp_stat(channel(c), abs, &attrs)) {
    /* A missing file is the normal answer before an upload; don't log it. */
    if(libssh2_session_last_errno(session(c)) == LIBSSH2_ERROR_SFTP_PROTOCOL &&
       libssh2_sftp_last_error(channel(c)) == LIBSSH2_FX_NO_SUCH_FILE) {
      return 1;
    }
    fail(c, "Could not read file size");
    return -1;
  }
  *size = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) ? (long long)attrs.filesize : -1;
  return 0;
}

int
sftpc_set_mtime(ftp_conn_t *c, const char *path, long long mtime) {
  LIBSSH2_SFTP_ATTRIBUTES attrs;
  char abs[1024];

  if(mtime < 0 || !c->sftp) return -1;
  abspath(c, path, abs, sizeof(abs));
  memset(&attrs, 0, sizeof(attrs));
  attrs.flags = LIBSSH2_SFTP_ATTR_ACMODTIME;
  attrs.atime = attrs.mtime = (unsigned long)mtime;
  return libssh2_sftp_setstat(channel(c), abs, &attrs) ? -1 : 0;
}

/* Like FTP, a cancel tears the connection down; the next item reconnects. */
static int
abort_transfer(ftp_conn_t *c, LIBSSH2_SFTP_HANDLE *h, const char *reason) {
  (void)h;
  kill_conn(c);
  set_error(c, "%s", reason);
  return -1;
}

int
sftpc_retrieve(ftp_conn_t *c, const char *path, long long offset,
              ftp_sink_cb sink, void *ctx) {
  LIBSSH2_SFTP_HANDLE *h;
  char abs[1024];
  char *buf;

  abspath(c, path, abs, sizeof(abs));
  if(!c->sftp) return fail(c, "Download failed");
  log_msg(c->log_id, LOG_COMMAND, "get \"%s\"%s", abs, offset ? " (resume)" : "");
  if(!(h = libssh2_sftp_open(channel(c), abs, LIBSSH2_FXF_READ, 0))) return fail(c, "Download failed");
  if(offset > 0) libssh2_sftp_seek64(h, (libssh2_uint64_t)offset);
  if(!(buf = malloc(SFTP_BUFFER_SIZE))) {
    libssh2_sftp_close(h);
    set_error(c, "Out of memory");
    return -1;
  }
  for(;;) {
    ssize_t n = libssh2_sftp_read(h, buf, SFTP_BUFFER_SIZE);

    if(canceled(c)) {
      free(buf);
      return abort_transfer(c, h, "Transfer canceled");
    }
    if(n < 0) {
      free(buf);
      return fail(c, "Download failed");
    }
    if(n == 0) break;
    if(sink(ctx, buf, (size_t)n)) {
      free(buf);
      return abort_transfer(c, h, "Could not write local file");
    }
  }
  free(buf);
  libssh2_sftp_close(h);
  c->last_activity = time(NULL);
  return 0;
}

int
sftpc_store(ftp_conn_t *c, const char *path, long long offset,
           ftp_source_cb source, void *ctx) {
  LIBSSH2_SFTP_HANDLE *h;
  unsigned long flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT;
  char abs[1024];
  char *buf;

  abspath(c, path, abs, sizeof(abs));
  if(!c->sftp) return fail(c, "Upload failed");
  log_msg(c->log_id, LOG_COMMAND, "put \"%s\"%s", abs, offset ? " (resume)" : "");
  if(!offset) flags |= LIBSSH2_FXF_TRUNC;
  if(!(h = libssh2_sftp_open(channel(c), abs, flags, 0644))) return fail(c, "Upload failed");
  if(offset > 0) libssh2_sftp_seek64(h, (libssh2_uint64_t)offset);
  if(!(buf = malloc(SFTP_BUFFER_SIZE))) {
    libssh2_sftp_close(h);
    set_error(c, "Out of memory");
    return -1;
  }
  for(;;) {
    ssize_t n = source(ctx, buf, SFTP_BUFFER_SIZE);
    size_t sent = 0;

    if(canceled(c)) {
      free(buf);
      return abort_transfer(c, h, "Transfer canceled");
    }
    if(n < 0) {
      free(buf);
      libssh2_sftp_close(h);
      set_error(c, "Could not read local file");
      return -1;
    }
    if(n == 0) break;
    while(sent < (size_t)n) {
      ssize_t w = libssh2_sftp_write(h, buf + sent, (size_t)n - sent);

      if(w < 0) {
        free(buf);
        return fail(c, "Upload failed");
      }
      sent += (size_t)w;
    }
  }
  free(buf);
  if(libssh2_sftp_close(h)) return fail(c, "Upload failed");
  c->last_activity = time(NULL);
  return 0;
}

/* A request the server has to answer (unlike a keepalive, which only goes
   out), with its own timeout. A refusal is still an answer. */
int
sftpc_ping(ftp_conn_t *c, int timeout_ms) {
  char out[1024];
  int n;

  if(!c->ssh || !c->sftp) return -1;
  libssh2_session_set_timeout(session(c), timeout_ms);
  n = libssh2_sftp_realpath(channel(c), ".", out, sizeof(out));
  libssh2_session_set_timeout(session(c), SSH_TIMEOUT_MS);
  if(n > 0 || n == LIBSSH2_ERROR_SFTP_PROTOCOL) {
    c->last_activity = time(NULL);
    return 0;
  }
  return fail(c, "The server stopped answering");
}

int
sftpc_noop(ftp_conn_t *c) {
  int next;

  if(!c->ssh) return -1;
  if(libssh2_keepalive_send(session(c), &next)) return fail(c, "Keepalive failed");
  c->last_activity = time(NULL);
  return 0;
}
