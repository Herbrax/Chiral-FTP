/* Transfer queue. Like FileZilla, every active transfer slot logs in with its
   own control connection, so several files move at once while the browsing
   connection stays free. Folders are expanded into child items on demand. */

#include "queue.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "localfs.h"
#include "log.h"
#include "settings.h"

#define QS_QUEUED   0
#define QS_RUNNING  1
#define QS_DONE     2
#define QS_FAILED   3
#define QS_CANCELED 4
#define QS_SKIPPED  5
#define QS_BACKOFF  6  /* internal: server refused another connection */

#define FINISHED_KEEP     2000
#define IDLE_EXIT_ROUNDS  6     /* x 5 s before an idle worker exits */
#define IDLE_DISCONNECT_S 15

typedef struct server_ref {
  int refs;
  ftp_server_t s;
} server_ref_t;

typedef struct qitem {
  unsigned long id;
  int direction;
  int is_dir;
  int state;
  int attempts;
  int slot;
  char *local;
  char *remote;
  long long size;
  long long done;
  long long mtime;
  double speed;
  time_t finished;
  volatile int cancel;
  char error[200];
  server_ref_t *server;
  struct qitem *next;
} qitem_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static qitem_t *g_head;
static qitem_t *g_tail;
static unsigned long g_next_id = 1;
static int g_workers;
static uint32_t g_slots;  /* bit n set: slot n+1 is taken */
static int g_paused;
static int g_stopping;  // queue_stop(): the app is on its way out
/* Learned from "421 Too many connections"; 0 means no limit. */
static int g_conn_cap;
static server_ref_t *g_last_server;

static void kick_locked(void);

/* The mode an upload leaves on the server: 0777 inside a PS5 homebrew
   folder (ftp_homebrew_path), else this build's own file mode. Folders keep
   whatever the server gives them, outside one; ftp_mkdir sees to the new
   ones inside, this to those already there. */
static void
set_remote_mode(ftp_conn_t *c, const char *remote, int is_dir) {
  const int homebrew = ftp_homebrew_path(remote);
  if(is_dir && !homebrew) return;
  // A server that refuses once (most Windows ones) isn't asked again on this connection.
  if(!c->no_chmod && ftp_chmod(c, remote, homebrew ? 0777 : CHIRAL_FILE_MODE) && ftp_is_open(c)) c->no_chmod = 1;
}

static double
now_s(void) {
  struct timeval tv;

  gettimeofday(&tv, NULL);
  return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

static server_ref_t *
server_ref_get_locked(const ftp_server_t *s) {
  server_ref_t *ref;

  if(g_last_server && ftp_same_server(&g_last_server->s, s) &&
     g_last_server->s.pasv_mode == s->pasv_mode) {
    g_last_server->refs++;
    return g_last_server;
  }
  if(!(ref = calloc(1, sizeof(*ref)))) return NULL;
  ref->s = *s;
  ref->refs = 2;  /* the item and g_last_server */
  if(g_last_server && !--g_last_server->refs) free(g_last_server);
  g_last_server = ref;
  return ref;
}

static void
server_ref_put_locked(server_ref_t *ref) {
  if(ref && !--ref->refs) free(ref);
}

static qitem_t *
item_new_locked(int direction, server_ref_t *server, const char *local,
                const char *remote, int is_dir, long long size, long long mtime) {
  qitem_t *it = calloc(1, sizeof(*it));

  if(!it) return NULL;
  if(!(it->local = strdup(local)) || !(it->remote = strdup(remote))) {
    free(it->local);
    free(it);
    return NULL;
  }
  it->id = g_next_id++;
  it->direction = direction;
  it->is_dir = is_dir;
  it->size = size;
  it->mtime = mtime;
  it->server = server;
  server->refs++;
  return it;
}

static void
item_free_locked(qitem_t *it) {
  server_ref_put_locked(it->server);
  free(it->local);
  free(it->remote);
  free(it);
}

static void
append_locked(qitem_t *it) {
  if(g_tail) g_tail->next = it;
  else g_head = it;
  g_tail = it;
}

static void
insert_after_locked(qitem_t *pos, qitem_t *it) {
  it->next = pos->next;
  pos->next = it;
  if(g_tail == pos) g_tail = it;
}

static void
unlink_locked(qitem_t *prev, qitem_t *it) {
  if(prev) prev->next = it->next;
  else g_head = it->next;
  if(g_tail == it) g_tail = prev;
}

static int
is_finished(int state) {
  return state == QS_DONE || state == QS_SKIPPED || state == QS_FAILED ||
         state == QS_CANCELED;
}

static int
in_tab(int state, const char *tab) {
  if(!strcmp(tab, "failed")) return state == QS_FAILED || state == QS_CANCELED;
  if(!strcmp(tab, "done")) return state == QS_DONE || state == QS_SKIPPED;
  return state == QS_QUEUED || state == QS_RUNNING;
}

/* Keeps memory bounded when thousands of files finish. */
static void
trim_finished_locked(void) {
  size_t finished = 0;
  qitem_t *prev = NULL;

  for(qitem_t *it = g_head; it; it = it->next) {
    if(it->state == QS_DONE || it->state == QS_SKIPPED) finished++;
  }
  for(qitem_t *it = g_head; it && finished > FINISHED_KEEP;) {
    qitem_t *next = it->next;

    if(it->state == QS_DONE || it->state == QS_SKIPPED) {
      unlink_locked(prev, it);
      item_free_locked(it);
      finished--;
    } else {
      prev = it;
    }
    it = next;
  }
}

static void
set_error(qitem_t *it, const char *msg) {
  snprintf(it->error, sizeof(it->error), "%s", msg);
}

typedef struct progress {
  qitem_t *it;
  int fd;
  double last_t;
  long long last_done;
} progress_t;

static void
progress_update(progress_t *p, size_t n) {
  double t;

  p->it->done += (long long)n;
  t = now_s();
  if(t - p->last_t >= 0.5) {
    double inst = (double)(p->it->done - p->last_done) / (t - p->last_t);

    p->it->speed = p->it->speed > 0 ? p->it->speed * 0.6 + inst * 0.4 : inst;
    p->last_t = t;
    p->last_done = p->it->done;
  }
}

static int
file_sink(void *ctx, const char *data, size_t size) {
  progress_t *p = ctx;
  size_t done = 0;

  while(done < size) {
    ssize_t n = write(p->fd, data + done, size - done);

    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) return -1;
    done += (size_t)n;
  }
  progress_update(p, size);
  return 0;
}

static ssize_t
file_source(void *ctx, char *buf, size_t size) {
  progress_t *p = ctx;
  ssize_t n;

  do {
    n = read(p->fd, buf, size);
  } while(n < 0 && errno == EINTR);
  if(n > 0) progress_update(p, (size_t)n);
  return n;
}

/* "name.ext" -> "name (1).ext" */
static void
numbered_name(char *out, size_t size, const char *path, int n) {
  const char *base = path_basename(path);
  const char *dot = strrchr(base, '.');

  if(!dot || dot == base) dot = base + strlen(base);
  snprintf(out, size, "%.*s (%d)%s", (int)(dot - path), path, n, dot);
}

static void
replace_path(char **slot, const char *path) {
  char *copy = strdup(path);

  if(!copy) return;
  pthread_mutex_lock(&g_lock);
  free(*slot);
  *slot = copy;
  pthread_mutex_unlock(&g_lock);
}

/* Decides the start offset for a transfer whose target already exists.
   Returns 1 to skip, 0 to transfer from *offset. */
static int
apply_policy(qitem_t *it, int policy, long long target_size, long long *offset) {
  *offset = 0;
  switch(policy) {
  case POLICY_SKIP:
    set_error(it, "Target exists");
    return 1;
  case POLICY_RESUME:
    if(it->size >= 0 && target_size == it->size) {
      set_error(it, "Already complete");
      return 1;
    }
    if(it->size >= 0 && target_size >= 0 && target_size < it->size) {
      *offset = target_size;
    }
    return 0;
  default:
    return 0;
  }
}

static int
download_file(ftp_conn_t *c, qitem_t *it, const settings_t *st) {
  struct stat sb;
  long long offset = 0;
  progress_t p;
  char parent[PATH_MAX];
  int ret;

  if(it->size < 0) {
    long long size;
    if(!ftp_size(c, it->remote, &size)) it->size = size;
    if(!ftp_is_open(c)) return -1;
  }
  if(!stat(it->local, &sb)) {
    if(st->policy == POLICY_RENAME) {
      char alt[PATH_MAX];
      int n = 1;

      do {
        numbered_name(alt, sizeof(alt), it->local, n++);
      } while(!stat(alt, &sb) && n < 1000);
      replace_path(&it->local, alt);
    } else if(apply_policy(it, st->policy, (long long)sb.st_size, &offset)) {
      return 1;
    }
  }
  path_parent(parent, sizeof(parent), it->local);
  localfs_mkdir_p(parent);

  memset(&p, 0, sizeof(p));
  p.it = it;
  p.fd = localfs_open_write(it->local, O_WRONLY | O_CREAT | (offset ? 0 : O_TRUNC), CHIRAL_FILE_MODE);
  if(p.fd >= 0) fchmod(p.fd, CHIRAL_FILE_MODE);  // also when replacing a file
  if(p.fd < 0) {
    snprintf(it->error, sizeof(it->error), "Could not open local file: %s", strerror(errno));
    return -2;
  }
  if(offset && lseek(p.fd, (off_t)offset, SEEK_SET) < 0) {
    close(p.fd);
    set_error(it, "Could not seek in local file");
    return -2;
  }
  if(offset) log_msg(c->log_id, LOG_STATUS, "Resuming download at %lld bytes", offset);
  it->done = offset;
  p.last_t = now_s();
  p.last_done = offset;
  ret = ftp_retrieve(c, it->remote, offset, file_sink, &p);
  if(close(p.fd) && !ret) {
    set_error(it, "Could not finish writing local file");
    return -2;
  }
  if(ret) {
    set_error(it, c->error);
    return -1;
  }
  if(st->preserve_mtime && it->mtime >= 0) {
    struct timeval tv[2] = {{(time_t)it->mtime, 0}, {(time_t)it->mtime, 0}};
    utimes(it->local, tv);
  }
  log_msg(c->log_id, LOG_STATUS, "File transfer successful, transferred %lld bytes",
          it->done - offset);
  return 0;
}

static int
upload_file(ftp_conn_t *c, qitem_t *it, const settings_t *st) {
  struct stat sb;
  long long offset = 0;
  long long rsize = -1;
  progress_t p;
  int exists;
  int ret;

  if(stat(it->local, &sb)) {
    snprintf(it->error, sizeof(it->error), "Could not read local file: %s", strerror(errno));
    return -2;
  }
  it->size = (long long)sb.st_size;
  if(it->mtime < 0) it->mtime = (long long)sb.st_mtime;
  exists = ftp_size(c, it->remote, &rsize) == 0;
  if(!ftp_is_open(c)) return -1;
  if(exists) {
    if(st->policy == POLICY_RENAME) {
      char alt[PATH_MAX];
      int n = 1;

      do {
        numbered_name(alt, sizeof(alt), it->remote, n++);
      } while(ftp_size(c, alt, &rsize) == 0 && n < 1000);
      if(!ftp_is_open(c)) return -1;
      replace_path(&it->remote, alt);
    } else if(apply_policy(it, st->policy, rsize, &offset)) {
      return 1;
    }
  }

  memset(&p, 0, sizeof(p));
  p.it = it;
  if((p.fd = open(it->local, O_RDONLY)) < 0) {
    snprintf(it->error, sizeof(it->error), "Could not open local file: %s", strerror(errno));
    return -2;
  }
  if(offset && lseek(p.fd, (off_t)offset, SEEK_SET) < 0) {
    close(p.fd);
    set_error(it, "Could not seek in local file");
    return -2;
  }
  if(offset) log_msg(c->log_id, LOG_STATUS, "Resuming upload at %lld bytes", offset);
  it->done = offset;
  p.last_t = now_s();
  p.last_done = offset;
  ret = ftp_store(c, it->remote, offset, file_source, &p);
  close(p.fd);
  if(ret) {
    set_error(it, c->error);
    return -1;
  }
  if(st->preserve_mtime) ftp_set_mtime(c, it->remote, it->mtime);
  set_remote_mode(c, it->remote, 0);
  log_msg(c->log_id, LOG_STATUS, "File transfer successful, transferred %lld bytes",
          it->done - offset);
  return 0;
}

static void
add_child_locked(qitem_t **after, qitem_t *parent, const char *name, int is_dir,
                 long long size, long long mtime) {
  char local[PATH_MAX];
  char remote[PATH_MAX];
  qitem_t *child;

  if(!path_is_safe_name(name)) return;
  path_join(local, sizeof(local), parent->local, name);
  path_join(remote, sizeof(remote), parent->remote, name);
  if(!(child = item_new_locked(parent->direction, parent->server, local, remote,
                               is_dir, size, mtime))) {
    return;
  }
  insert_after_locked(*after, child);
  *after = child;
}

static int
expand_download(ftp_conn_t *c, qitem_t *it) {
  ftp_listing_t l;
  char resolved[PATH_MAX];
  qitem_t *after = it;

  if(ftp_list(c, it->remote, &l, resolved, sizeof(resolved))) {
    set_error(it, c->error);
    return -1;
  }
  if(localfs_mkdir_p(it->local)) {
    snprintf(it->error, sizeof(it->error), "Could not create local folder: %s", strerror(errno));
    ftp_listing_free(&l);
    return -2;
  }
  pthread_mutex_lock(&g_lock);
  /* Files first, then subfolders, like FileZilla. */
  for(int pass = 0; pass < 2; pass++) {
    for(size_t i = 0; i < l.count; i++) {
      ftp_entry_t *e = &l.entries[i];
      if(e->is_dir != pass) continue;
      add_child_locked(&after, it, e->name, e->is_dir, e->is_dir ? -1 : e->size, e->mtime);
    }
  }
  kick_locked();
  pthread_mutex_unlock(&g_lock);
  ftp_listing_free(&l);
  return 0;
}

static int
expand_upload(ftp_conn_t *c, qitem_t *it) {
  DIR *dir;
  struct dirent *ent;
  qitem_t *after = it;

  if(!(dir = opendir(it->local))) {
    snprintf(it->error, sizeof(it->error), "Could not open local folder: %s", strerror(errno));
    return -2;
  }
  /* An existing folder makes MKD fail; that's fine as long as we stay connected. */
  if(ftp_mkdir(c, it->remote)) {
    if(!ftp_is_open(c)) {
      closedir(dir);
      set_error(it, c->error);
      return -1;
    }
    set_remote_mode(c, it->remote, 1);  // already there
  }
  pthread_mutex_lock(&g_lock);
  while((ent = readdir(dir))) {
    char full[PATH_MAX];
    struct stat sb;

    if(!path_is_safe_name(ent->d_name)) continue;
    path_join(full, sizeof(full), it->local, ent->d_name);
    if(stat(full, &sb)) continue;
    add_child_locked(&after, it, ent->d_name, S_ISDIR(sb.st_mode) ? 1 : 0,
                     S_ISDIR(sb.st_mode) ? -1 : (long long)sb.st_size,
                     (long long)sb.st_mtime);
  }
  kick_locked();
  pthread_mutex_unlock(&g_lock);
  closedir(dir);
  return 0;
}

/* Returns the final state, or QS_QUEUED to retry. */
static int
process(ftp_conn_t *c, qitem_t *it, const settings_t *st) {
  int ret;

  c->cancel = &it->cancel;
  if(!ftp_is_open(c) || !ftp_same_server(&c->server, &it->server->s)) {
    ftp_close(c);
    if(ftp_open(c, &it->server->s)) {
      set_error(it, c->error);
      /* Per-IP connection limits answer 421; back off instead of failing. */
      if(c->code == 421 && !it->cancel) {
        c->cancel = NULL;
        return QS_BACKOFF;
      }
      ret = -1;
      goto done;
    }
  }
  log_msg(c->log_id, LOG_STATUS, "Starting %s of %s", it->direction == QUEUE_DOWNLOAD ?
          "download" : "upload", it->direction == QUEUE_DOWNLOAD ? it->remote : it->local);
  if(it->is_dir) {
    ret = it->direction == QUEUE_DOWNLOAD ? expand_download(c, it) : expand_upload(c, it);
  } else {
    ret = it->direction == QUEUE_DOWNLOAD ? download_file(c, it, st) : upload_file(c, it, st);
  }

done:
  c->cancel = NULL;
  if(it->cancel) {
    set_error(it, "Canceled");
    return QS_CANCELED;
  }
  if(ret == 0) return QS_DONE;
  if(ret == 1) return QS_SKIPPED;
  /* -1 with a dropped connection is transient; server refusals are final. */
  if(ret == -1 && !ftp_is_open(c) && it->attempts < st->retries) {
    it->attempts++;
    log_msg(c->log_id, LOG_STATUS, "Retrying (%d of %d)", it->attempts, st->retries);
    return QS_QUEUED;
  }
  log_msg(c->log_id, LOG_ERROR, "Transfer failed: %s", it->error);
  return QS_FAILED;
}

static qitem_t *
next_queued_locked(void) {
  for(qitem_t *it = g_head; it; it = it->next) {
    if(it->state == QS_QUEUED) return it;
  }
  return NULL;
}

static void *
worker_main(void *arg) {
  int slot = (int)(intptr_t)arg;
  ftp_conn_t *c = malloc(sizeof(*c));
  int idle_rounds = 0;

  if(!c) {
    pthread_mutex_lock(&g_lock);
    g_workers--;
    g_slots &= ~(1u << (slot - 1));
    pthread_mutex_unlock(&g_lock);
    return NULL;
  }
  ftp_init(c, slot);
  pthread_mutex_lock(&g_lock);
  for(;;) {
    settings_t st = settings_get();
    qitem_t *it;
    int state;

    if(g_stopping) break;
    if(slot > st.connections) break;
    if(g_conn_cap && g_workers > g_conn_cap) break;
    it = g_paused ? NULL : next_queued_locked();
    if(!it) {
      struct timespec ts;

      if(++idle_rounds > IDLE_EXIT_ROUNDS) break;
      if(ftp_is_open(c) && time(NULL) - c->last_activity >= IDLE_DISCONNECT_S) {
        pthread_mutex_unlock(&g_lock);
        ftp_close(c);
        pthread_mutex_lock(&g_lock);
      }
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 5;
      pthread_cond_timedwait(&g_cond, &g_lock, &ts);
      continue;
    }
    idle_rounds = 0;
    it->state = QS_RUNNING;
    it->slot = slot;
    it->cancel = 0;
    it->speed = 0;
    it->error[0] = 0;
    pthread_mutex_unlock(&g_lock);

    state = process(c, it, &st);
    if(state == QS_QUEUED && !g_stopping) sleep(1);

    pthread_mutex_lock(&g_lock);
    if(state == QS_BACKOFF) {
      int others = g_workers - 1;

      it->state = QS_QUEUED;
      it->slot = 0;
      it->error[0] = 0;
      if(others > 0) {
        g_conn_cap = others;
        log_msg(slot, LOG_STATUS, "Server allows no more connections; continuing with %d", others);
        pthread_cond_broadcast(&g_cond);
        break;
      }
      /* Even one connection was refused: wait and retry like a normal failure. */
      if(it->attempts++ >= st.retries) {
        it->state = QS_FAILED;
        it->finished = time(NULL);
        snprintf(it->error, sizeof(it->error), "%s", c->error);
      }
      pthread_mutex_unlock(&g_lock);
      if(!g_stopping) sleep(5);
      pthread_mutex_lock(&g_lock);
      continue;
    }
    it->state = state;
    it->speed = 0;
    it->slot = 0;
    if(is_finished(state)) {
      it->finished = time(NULL);
      trim_finished_locked();
    }
    pthread_cond_broadcast(&g_cond);
  }
  g_workers--;
  g_slots &= ~(1u << (slot - 1));
  pthread_mutex_unlock(&g_lock);
  ftp_close(c);
  free(c);
  return NULL;
}

static void
kick_locked(void) {
  settings_t st = settings_get();
  int pending = 0;
  int want;

  for(qitem_t *it = g_head; it; it = it->next) {
    if(it->state == QS_QUEUED || it->state == QS_RUNNING) pending++;
    if(pending >= st.connections) break;
  }
  if(!pending) g_conn_cap = 0;
  want = g_paused || g_stopping ? 0 : (pending < st.connections ? pending : st.connections);
  if(g_conn_cap && want > g_conn_cap) want = g_conn_cap;
  while(g_workers < want) {
    pthread_attr_t attr;
    pthread_t thread;
    int slot = 0;

    for(int i = 0; i < st.connections; i++) {
      if(!(g_slots & (1u << i))) {
        slot = i + 1;
        break;
      }
    }
    if(!slot) break;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if(pthread_create(&thread, &attr, worker_main, (void *)(intptr_t)slot)) {
      pthread_attr_destroy(&attr);
      break;
    }
    pthread_attr_destroy(&attr);
    g_slots |= 1u << (slot - 1);
    g_workers++;
  }
  pthread_cond_broadcast(&g_cond);
}

void
queue_kick(void) {
  pthread_mutex_lock(&g_lock);
  g_conn_cap = 0;  /* settings changed: probe the server limit again */
  kick_locked();
  pthread_mutex_unlock(&g_lock);
}

int
queue_add(int direction, const ftp_server_t *server, const char *local,
          const char *remote, int is_dir, long long size, long long mtime) {
  server_ref_t *ref;
  qitem_t *it;

  pthread_mutex_lock(&g_lock);
  if(!(ref = server_ref_get_locked(server))) {
    pthread_mutex_unlock(&g_lock);
    return -1;
  }
  it = item_new_locked(direction, ref, local, remote, is_dir, size, mtime);
  server_ref_put_locked(ref);
  if(!it) {
    pthread_mutex_unlock(&g_lock);
    return -1;
  }
  append_locked(it);
  kick_locked();
  pthread_mutex_unlock(&g_lock);
  return 0;
}

static const char *
state_name(int state) {
  switch(state) {
  case QS_RUNNING: return "running";
  case QS_DONE: return "done";
  case QS_FAILED: return "failed";
  case QS_CANCELED: return "canceled";
  case QS_SKIPPED: return "skipped";
  default: return "queued";
  }
}

void
queue_json(strbuf_t *b, const char *tab, int limit) {
  size_t queued = 0, failed = 0, done = 0, running = 0;
  long long queued_bytes = 0;
  double speed = 0;
  int shown = 0;
  int comma = 0;

  pthread_mutex_lock(&g_lock);
  strbuf_append(b, "{\"items\":[");
  /* Running items first so they are always visible. */
  for(int pass = 0; pass < 2; pass++) {
    for(qitem_t *it = g_head; it; it = it->next) {
      if(pass == 0 && it->state != QS_RUNNING) continue;
      if(pass == 1 && it->state == QS_RUNNING) continue;
      if(!in_tab(it->state, tab) || shown >= limit) continue;
      strbuf_printf(b, "%s{\"id\":%lu,\"direction\":\"%s\",\"dir\":%d,\"state\":\"%s\","
                    "\"size\":%lld,\"done\":%lld,\"speed\":%.0f,\"slot\":%d,"
                    "\"attempts\":%d,\"finished\":%lld,\"local\":",
                    comma ? "," : "", it->id,
                    it->direction == QUEUE_DOWNLOAD ? "download" : "upload",
                    it->is_dir, state_name(it->state), it->size, it->done,
                    it->speed, it->slot, it->attempts, (long long)it->finished);
      json_str(b, it->local);
      strbuf_append(b, ",\"remote\":");
      json_str(b, it->remote);
      strbuf_append(b, ",\"host\":");
      json_str(b, it->server->s.host);
      strbuf_append(b, ",\"error\":");
      json_str(b, it->error);
      strbuf_append(b, "}");
      comma = 1;
      shown++;
    }
  }
  for(qitem_t *it = g_head; it; it = it->next) {
    if(in_tab(it->state, "queued")) {
      queued++;
      if(!it->is_dir && it->size > 0) queued_bytes += it->size - it->done;
    } else if(in_tab(it->state, "failed")) {
      failed++;
    } else {
      done++;
    }
    if(it->state == QS_RUNNING) {
      running++;
      speed += it->speed;
    }
  }
  strbuf_printf(b, "],\"queued\":%zu,\"failed\":%zu,\"done\":%zu,\"running\":%zu,"
                "\"workers\":%d,\"speed\":%.0f,\"queued_bytes\":%lld,\"paused\":%d}",
                queued, failed, done, running, g_workers, speed, queued_bytes, g_paused);
  pthread_mutex_unlock(&g_lock);
}

int
queue_snapshot(const char *tab, queue_view_t *out, int max, queue_stats_t *stats) {
  int shown = 0;

  memset(stats, 0, sizeof(*stats));
  pthread_mutex_lock(&g_lock);
  for(int pass = 0; pass < 2; pass++) {
    for(qitem_t *it = g_head; it && shown < max; it = it->next) {
      queue_view_t *v;

      if((pass == 0) != (it->state == QS_RUNNING) || !in_tab(it->state, tab)) continue;
      v = &out[shown++];
      v->id = it->id;
      v->upload = it->direction == QUEUE_UPLOAD;
      v->is_dir = it->is_dir;
      v->slot = it->slot;
      v->attempts = it->attempts;
      snprintf(v->state, sizeof(v->state), "%s", state_name(it->state));
      v->size = it->size;
      v->done = it->done;
      v->speed = it->speed;
      snprintf(v->local, sizeof(v->local), "%s", it->local);
      snprintf(v->remote, sizeof(v->remote), "%s", it->remote);
      snprintf(v->error, sizeof(v->error), "%s", it->error);
    }
  }
  for(qitem_t *it = g_head; it; it = it->next) {
    if(in_tab(it->state, "queued")) {
      stats->queued++;
      if(!it->is_dir && it->size > 0) stats->queued_bytes += it->size - it->done;
    } else if(in_tab(it->state, "failed")) {
      stats->failed++;
    } else {
      stats->done++;
    }
    if(it->state == QS_RUNNING) {
      stats->running++;
      stats->speed += it->speed;
    }
  }
  stats->workers = g_workers;
  stats->paused = g_paused;
  pthread_mutex_unlock(&g_lock);
  return shown;
}

int
queue_cancel(unsigned long id) {
  int found = 0;

  pthread_mutex_lock(&g_lock);
  for(qitem_t *it = g_head; it; it = it->next) {
    if(id && it->id != id) continue;
    if(it->state == QS_QUEUED) {
      it->state = QS_CANCELED;
      it->finished = time(NULL);
      snprintf(it->error, sizeof(it->error), "Canceled");
      found = 1;
    } else if(it->state == QS_RUNNING) {
      it->cancel = 1;
      found = 1;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return found ? 0 : -1;
}

int
queue_retry(unsigned long id) {
  int found = 0;

  pthread_mutex_lock(&g_lock);
  for(qitem_t *it = g_head; it; it = it->next) {
    if(id && it->id != id) continue;
    if(it->state == QS_FAILED || it->state == QS_CANCELED) {
      it->state = QS_QUEUED;
      it->attempts = 0;
      it->error[0] = 0;
      found = 1;
    }
  }
  kick_locked();
  pthread_mutex_unlock(&g_lock);
  return found ? 0 : -1;
}

int
queue_remove(unsigned long id) {
  qitem_t *prev = NULL;
  int ret = -1;

  pthread_mutex_lock(&g_lock);
  for(qitem_t *it = g_head; it; prev = it, it = it->next) {
    if(it->id != id) continue;
    if(it->state != QS_RUNNING) {
      unlink_locked(prev, it);
      item_free_locked(it);
      ret = 0;
    }
    break;
  }
  pthread_mutex_unlock(&g_lock);
  return ret;
}

void
queue_clear(const char *tab) {
  qitem_t *prev = NULL;

  pthread_mutex_lock(&g_lock);
  for(qitem_t *it = g_head; it;) {
    qitem_t *next = it->next;

    if(in_tab(it->state, tab) && it->state != QS_RUNNING) {
      unlink_locked(prev, it);
      item_free_locked(it);
    } else {
      prev = it;
    }
    it = next;
  }
  pthread_mutex_unlock(&g_lock);
}

void
queue_set_paused(int paused) {
  pthread_mutex_lock(&g_lock);
  g_paused = paused;
  kick_locked();
  pthread_mutex_unlock(&g_lock);
}

void
queue_stop(void) {
  pthread_mutex_lock(&g_lock);
  g_stopping = 1;
  for(qitem_t *it = g_head; it; it = it->next) {
    if(it->state == QS_RUNNING) it->cancel = 1;  // the transfer notices within a read
  }
  pthread_cond_broadcast(&g_cond);  // the idle ones, out of their wait
  pthread_mutex_unlock(&g_lock);
}

int
queue_workers(void) {
  int n;
  pthread_mutex_lock(&g_lock);
  n = g_workers;
  pthread_mutex_unlock(&g_lock);
  return n;
}
