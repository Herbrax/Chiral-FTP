/* Local copy/move jobs for the file manager. */

#include "fsjobs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>

#include "config.h"

/* The UI's translation lookup (app/i18n.cpp): activity shown on the TV is
   in the app's language. */
const char *tr(const char *english);
#include "localfs.h"

#define COPY_BUFSIZE (1024 * 1024)
#define MAX_JOBS 256

typedef struct job {
  int move;
  char src[1024];
  char dst_dir[1024];
} job_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static job_t g_jobs[MAX_JOBS];
static int g_head, g_count;
static int g_started;
static volatile int g_cancel;
static fsjob_stats_t g_stats;
static long long g_rate_bytes;
static struct timeval g_rate_start;

/* ---------- progress ---------- */

static void
progress(long long n) {
  pthread_mutex_lock(&g_lock);
  g_stats.done += n;
  g_rate_bytes += n;
  pthread_mutex_unlock(&g_lock);
}

static void
set_current(const char *path) {
  pthread_mutex_lock(&g_lock);
  snprintf(g_stats.current, sizeof(g_stats.current), "%s", path_basename(path));
  pthread_mutex_unlock(&g_lock);
}

/* Bytes under path, for the progress bar. */
static long long
tree_size(const char *path) {
  struct stat st;
  long long total = 0;
  DIR *dir;
  struct dirent *ent;

  if(stat(path, &st)) return 0;
  if(!S_ISDIR(st.st_mode)) return st.st_size;
  if(!(dir = opendir(path))) return 0;
  while(!g_cancel && (ent = readdir(dir))) {
    char child[1024];
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    path_join(child, sizeof(child), path, ent->d_name);
    total += tree_size(child);
  }
  closedir(dir);
  return total;
}

/* ---------- copying ---------- */

static int
copy_file(const char *src, const char *dst, const struct stat *st) {
  int in, out, err = 0;
  char *buf;
  struct utimbuf t;

  if(!(buf = malloc(COPY_BUFSIZE))) return -1;
  if((in = open(src, O_RDONLY)) < 0) {
    free(buf);
    return -1;
  }
  if((out = localfs_open_write(dst, O_WRONLY | O_CREAT | O_EXCL, PS5_FILE_MODE)) < 0) {
    err = errno;
    close(in);
    free(buf);
    errno = err;
    return -1;
  }
  fchmod(out, PS5_FILE_MODE);
  set_current(src);
  for(;;) {
    ssize_t n = read(in, buf, COPY_BUFSIZE);
    if(n < 0 && errno == EINTR) continue;
    if(n < 0) {
      err = errno;
      break;
    }
    if(!n) break;
    if(g_cancel) {
      err = ECANCELED;
      break;
    }
    for(ssize_t done = 0; done < n;) {
      ssize_t w = write(out, buf + done, (size_t)(n - done));
      if(w < 0 && errno == EINTR) continue;
      if(w <= 0) {
        err = w < 0 ? errno : ENOSPC;
        break;
      }
      done += w;
    }
    if(err) break;
    progress(n);
  }
  if(close(out) && !err) err = errno;
  close(in);
  free(buf);
  if(err) {
    unlink(dst);  // no half-copied files
    errno = err;
    return -1;
  }
  t.actime = st->st_atime;
  t.modtime = st->st_mtime;
  utime(dst, &t);
  return 0;
}

static int
copy_tree(const char *src, const char *dst) {
  struct stat st;
  DIR *dir;
  struct dirent *ent;
  int err = 0;

  if(g_cancel) {
    errno = ECANCELED;
    return -1;
  }
  if(stat(src, &st)) return -1;
  if(!S_ISDIR(st.st_mode)) return copy_file(src, dst, &st);
  if(localfs_mkdir(dst) && errno != EEXIST) return -1;
  if(!(dir = opendir(src))) return -1;
  while(!err && (ent = readdir(dir))) {
    char s[1024], d[1024];
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    path_join(s, sizeof(s), src, ent->d_name);
    path_join(d, sizeof(d), dst, ent->d_name);
    if(copy_tree(s, d)) err = errno ? errno : EIO;
  }
  closedir(dir);
  if(err) {
    errno = err;
    return -1;
  }
  return 0;
}

static int
run(const job_t *j) {
  char dst[1024];
  size_t sl = strlen(j->src);

  /* A folder can't go inside itself. */
  if(!strncmp(j->dst_dir, j->src, sl) && (j->dst_dir[sl] == '/' || !j->dst_dir[sl])) {
    errno = EINVAL;
    return -1;
  }
  if(localfs_free_name(dst, sizeof(dst), j->dst_dir, path_basename(j->src))) {
    errno = EEXIST;
    return -1;
  }
  if(j->move) {
    if(!localfs_rename(j->src, dst)) {  // same drive: instant
      pthread_mutex_lock(&g_lock);
      g_stats.done = g_stats.total;
      pthread_mutex_unlock(&g_lock);
      return 0;
    }
    if(errno != EXDEV) return -1;
  }
  if(copy_tree(j->src, dst)) {
    int err = errno;
    localfs_delete(dst);  // remove what was copied of a failed folder
    errno = err;
    return -1;
  }
  return j->move ? localfs_delete(j->src) : 0;
}

static void *
worker(void *arg) {
  (void)arg;
  for(;;) {
    job_t j;
    long long total;

    pthread_mutex_lock(&g_lock);
    while(!g_count) pthread_cond_wait(&g_cond, &g_lock);
    j = g_jobs[g_head];
    g_head = (g_head + 1) % MAX_JOBS;
    g_count--;
    g_cancel = 0;
    g_stats.running = 1;
    g_stats.queued = g_count;
    g_stats.move = j.move;
    g_stats.done = g_stats.total = 0;
    snprintf(g_stats.current, sizeof(g_stats.current), "%s", path_basename(j.src));
    snprintf(g_stats.target, sizeof(g_stats.target), "%s", j.dst_dir);
    pthread_mutex_unlock(&g_lock);

    total = tree_size(j.src);
    pthread_mutex_lock(&g_lock);
    g_stats.total = total;
    pthread_mutex_unlock(&g_lock);

    errno = 0;
    int failed = run(&j);
    int err = errno;
    pthread_mutex_lock(&g_lock);
    g_stats.running = 0;
    g_stats.finished++;
    if(failed) {
      g_stats.failed++;
      snprintf(g_stats.error, sizeof(g_stats.error), j.move ? tr("Moving %s: %s") : tr("Copying %s: %s"),
               path_basename(j.src), err == ECANCELED ? tr("canceled") : err == EINVAL ? tr("a folder can't go inside itself")
                                                                        : strerror(err ? err : EIO));
    }
    pthread_mutex_unlock(&g_lock);
  }
  return NULL;
}

int
fsjob_add(int move, const char *src, const char *dst_dir) {
  pthread_mutex_lock(&g_lock);
  if(!g_started) {
    pthread_t t;
    if(pthread_create(&t, NULL, worker, NULL)) {
      pthread_mutex_unlock(&g_lock);
      return -1;
    }
    pthread_detach(t);
    g_started = 1;
    gettimeofday(&g_rate_start, NULL);
  }
  if(g_count >= MAX_JOBS || strlen(src) >= sizeof(g_jobs[0].src) || strlen(dst_dir) >= sizeof(g_jobs[0].dst_dir)) {
    pthread_mutex_unlock(&g_lock);
    return -1;
  }
  job_t *j = &g_jobs[(g_head + g_count) % MAX_JOBS];
  j->move = move;
  snprintf(j->src, sizeof(j->src), "%s", src);
  snprintf(j->dst_dir, sizeof(j->dst_dir), "%s", dst_dir);
  g_count++;
  g_stats.queued = g_count;
  pthread_cond_signal(&g_cond);
  pthread_mutex_unlock(&g_lock);
  return 0;
}

void
fsjob_cancel_all(void) {
  pthread_mutex_lock(&g_lock);
  g_count = 0;
  g_stats.queued = 0;
  g_cancel = 1;
  pthread_mutex_unlock(&g_lock);
}

void
fsjob_get_stats(fsjob_stats_t *out) {
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
