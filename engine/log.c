#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#include "config.h"

#define LOG_CAPACITY 1000
#define LOG_TEXT_MAX 512

typedef struct log_entry {
  unsigned long seq;
  time_t time;
  int conn;
  log_kind_t kind;
  char text[LOG_TEXT_MAX];
} log_entry_t;

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static log_entry_t g_log[LOG_CAPACITY];
static unsigned long g_log_seq;
static unsigned long g_log_cleared;

static const char *
kind_name(log_kind_t kind) {
  switch(kind) {
  case LOG_COMMAND: return "command";
  case LOG_RESPONSE: return "response";
  case LOG_ERROR: return "error";
  default: return "status";
  }
}

void
log_msg(int conn, log_kind_t kind, const char *fmt, ...) {
  log_entry_t *e;
  va_list args;

  pthread_mutex_lock(&g_log_lock);
  e = &g_log[g_log_seq % LOG_CAPACITY];
  e->seq = ++g_log_seq;
  e->time = time(NULL);
  e->conn = conn;
  e->kind = kind;
  va_start(args, fmt);
  vsnprintf(e->text, sizeof(e->text), fmt, args);
  va_end(args);
  printf("[%d] %s: %s\n", conn, kind_name(kind), e->text);
  pthread_mutex_unlock(&g_log_lock);
}

unsigned long
log_json(strbuf_t *b, unsigned long since) {
  unsigned long first;
  unsigned long last;
  int comma = 0;

  pthread_mutex_lock(&g_log_lock);
  last = g_log_seq;
  first = last > LOG_CAPACITY ? last - LOG_CAPACITY + 1 : 1;
  if(since + 1 > first) first = since + 1;
  if(g_log_cleared + 1 > first) first = g_log_cleared + 1;
  strbuf_append(b, "[");
  for(unsigned long seq = first; seq <= last; seq++) {
    log_entry_t *e = &g_log[(seq - 1) % LOG_CAPACITY];
    strbuf_printf(b, "%s{\"seq\":%lu,\"time\":%lld,\"conn\":%d,\"kind\":\"%s\",\"text\":",
                  comma ? "," : "", e->seq, (long long)e->time, e->conn,
                  kind_name(e->kind));
    json_str(b, e->text);
    strbuf_append(b, "}");
    comma = 1;
  }
  strbuf_append(b, "]");
  pthread_mutex_unlock(&g_log_lock);
  return last;
}

void
startup_trace(const char *fmt, ...) {
  char line[512];
  va_list args;
  int n;
  int fd;

  int stamp;

  stamp = n = snprintf(line, sizeof(line), "[%lld] ", (long long)time(NULL));
  va_start(args, fmt);
  n += vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, args);
  va_end(args);
  if(n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
  line[n++] = '\n';
  char path[600];

  if((fd = open(data_path(path, sizeof(path), STARTUP_NAME), O_WRONLY | O_CREAT | O_APPEND, 0666)) >= 0) {
    write(fd, line, (size_t)n);
    fsync(fd);
    close(fd);
  }
  log_msg(0, LOG_STATUS, "%.*s", n - 1 - stamp, line + stamp);
}

unsigned long
log_each(unsigned long since, log_cb cb, void *ctx) {
  unsigned long first;
  unsigned long last;

  pthread_mutex_lock(&g_log_lock);
  last = g_log_seq;
  first = last > LOG_CAPACITY ? last - LOG_CAPACITY + 1 : 1;
  if(since + 1 > first) first = since + 1;
  if(g_log_cleared + 1 > first) first = g_log_cleared + 1;
  for(unsigned long seq = first; seq <= last; seq++) {
    log_entry_t *e = &g_log[(seq - 1) % LOG_CAPACITY];
    cb(ctx, e->seq, (long long)e->time, e->conn, e->kind, e->text);
  }
  pthread_mutex_unlock(&g_log_lock);
  return last;
}

void
log_clear(void) {
  pthread_mutex_lock(&g_log_lock);
  g_log_cleared = g_log_seq;
  pthread_mutex_unlock(&g_log_lock);
}
