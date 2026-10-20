#include "settings.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "form.h"
#include "system.h"

#define STORE_MAX (2 * 1024 * 1024)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static settings_t g_settings = {
  DEFAULT_TRANSFER_CONNECTIONS, POLICY_RESUME, 1, 2,
};

static int
clamp(int v, int lo, int hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

static char g_data_dir[256] = "./data";

const char *
data_dir(void) {
  return g_data_dir;
}

const char *
data_path(char *buf, unsigned long size, const char *name) {
  snprintf(buf, size, "%s/%s", g_data_dir, name);
  return buf;
}

/* A folder we can create a file in and list. */
static int
usable_dir(const char *dir) {
  char probe[300];
  DIR *d;
  int fd;

  if(mkdir(dir, 0777) && errno != EEXIST) return 0;
  snprintf(probe, sizeof(probe), "%s/.probe", dir);
  if((fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) return 0;
  close(fd);
  unlink(probe);
  if(!(d = opendir(dir))) return 0;
  closedir(d);
  return 1;
}

int
data_dir_init(void) {
  const char *candidates[4];
  int n = system_data_dir_candidates(candidates, (int)(sizeof(candidates) / sizeof(candidates[0])));

  for(int i = 0; i < n; i++) {
    if(usable_dir(candidates[i])) {
      snprintf(g_data_dir, sizeof(g_data_dir), "%s", candidates[i]);
      return 0;
    }
  }
  return -1;
}

int
ensure_data_dir(void) {
  if(mkdir(g_data_dir, 0777) && errno != EEXIST) return -1;
  return 0;
}

static int
read_file(const char *path, strbuf_t *out) {
  char buf[8192];
  int fd = open(path, O_RDONLY);
  ssize_t n;

  if(fd < 0) return -1;
  while((n = read(fd, buf, sizeof(buf))) > 0) {
    if(out->len + (size_t)n > STORE_MAX || strbuf_append_n(out, buf, (size_t)n)) {
      close(fd);
      return -1;
    }
  }
  close(fd);
  return n < 0 ? -1 : 0;
}

/* Writes through a temporary file so a crash never leaves a torn file. */
static int
write_file(const char *path, const char *data, size_t size) {
  char tmp[512];
  int fd;
  size_t done = 0;

  if(ensure_data_dir()) return -1;
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  if((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) return -1;
  while(done < size) {
    ssize_t n = write(fd, data + done, size - done);
    if(n <= 0) {
      close(fd);
      unlink(tmp);
      return -1;
    }
    done += (size_t)n;
  }
  if(close(fd)) {
    unlink(tmp);
    return -1;
  }
  if(rename(tmp, path)) {
    /* FAT and exFAT have no atomic replace, and libnx's rename refuses a
       destination that already exists -- so every save after the first one
       failed, and the Switch reported "could not save" while the setting
       had plainly changed. Retry with the old file out of the way. The
       rename is tried first so platforms that do replace atomically keep
       doing so. */
    unlink(path);
    if(rename(tmp, path)) {
      unlink(tmp);
      return -1;
    }
  }
  return 0;
}

void
settings_load(void) {
  strbuf_t b = {0};
  form_t f;

  char path[600];

  if(read_file(data_path(path, sizeof(path), SETTINGS_NAME), &b) || form_parse(&f, b.data, b.len)) {
    strbuf_free(&b);
    return;
  }
  pthread_mutex_lock(&g_lock);
  g_settings.connections = clamp((int)form_get_ll(&f, "connections", g_settings.connections),
                                 1, MAX_TRANSFER_CONNECTIONS);
  g_settings.policy = clamp((int)form_get_ll(&f, "policy", g_settings.policy), 0, 3);
  g_settings.preserve_mtime = !!form_get_ll(&f, "preserve_mtime", g_settings.preserve_mtime);
  g_settings.retries = clamp((int)form_get_ll(&f, "retries", g_settings.retries), 0, 10);
  pthread_mutex_unlock(&g_lock);
  form_free(&f);
  strbuf_free(&b);
}

settings_t
settings_get(void) {
  settings_t s;

  pthread_mutex_lock(&g_lock);
  s = g_settings;
  pthread_mutex_unlock(&g_lock);
  return s;
}

int
settings_set(const settings_t *s) {
  char buf[256];
  int n;

  pthread_mutex_lock(&g_lock);
  g_settings.connections = clamp(s->connections, 1, MAX_TRANSFER_CONNECTIONS);
  g_settings.policy = clamp(s->policy, 0, 3);
  g_settings.preserve_mtime = !!s->preserve_mtime;
  g_settings.retries = clamp(s->retries, 0, 10);
  n = snprintf(buf, sizeof(buf), "connections=%d&policy=%d&preserve_mtime=%d&retries=%d",
               g_settings.connections, g_settings.policy,
               g_settings.preserve_mtime, g_settings.retries);
  pthread_mutex_unlock(&g_lock);
  {
    char path[600];
    return write_file(data_path(path, sizeof(path), SETTINGS_NAME), buf, (size_t)n);
  }
}

void
settings_json(strbuf_t *b) {
  settings_t s = settings_get();

  strbuf_printf(b, "{\"connections\":%d,\"max_connections\":%d,\"policy\":%d,"
                "\"preserve_mtime\":%d,\"retries\":%d}",
                s.connections, MAX_TRANSFER_CONNECTIONS, s.policy,
                s.preserve_mtime, s.retries);
}

static const char *
store_path(const char *name) {
  static char path[600];
  if(!strcmp(name, "sites")) return data_path(path, sizeof(path), SITES_NAME);
  if(!strcmp(name, "history")) return data_path(path, sizeof(path), HISTORY_NAME);
  if(!strcmp(name, "prefs")) return data_path(path, sizeof(path), PREFS_NAME);
  if(!strcmp(name, "servers")) return data_path(path, sizeof(path), SERVERS_NAME);
  return NULL;
}

int
store_read(const char *name, strbuf_t *out) {
  const char *path = store_path(name);

  if(!path) return -1;
  if(read_file(path, out) || !out->len) {
    out->len = 0;
    return strbuf_append(out, "[]");
  }
  return 0;
}

int
store_write(const char *name, const char *data, size_t size) {
  const char *path = store_path(name);

  if(!path || size > STORE_MAX) return -1;
  return write_file(path, data, size);
}
