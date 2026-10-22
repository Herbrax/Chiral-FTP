#pragma once

#include "ftp.h"
#include "strbuf.h"

#define QUEUE_DOWNLOAD 0
#define QUEUE_UPLOAD   1

/* Adds a file or folder transfer. Folders are expanded by the worker. */
int queue_add(int direction, const ftp_server_t *server, const char *local,
              const char *remote, int is_dir, long long size, long long mtime);

/* tab is "queued", "failed" or "done"; limit caps the items returned. */
void queue_json(strbuf_t *b, const char *tab, int limit);

/* id 0 applies to every matching item. */
int queue_cancel(unsigned long id);
int queue_retry(unsigned long id);
int queue_remove(unsigned long id);
void queue_clear(const char *tab);
void queue_set_paused(int paused);

/* Plain-struct view of the queue for the native UI. */
typedef struct queue_view {
  unsigned long id;
  int upload;
  int is_dir;
  int slot;
  int attempts;
  char state[12];
  long long size;
  long long done;
  double speed;
  char local[512];
  char remote[512];
  char error[200];
} queue_view_t;

typedef struct queue_stats {
  int queued;
  int failed;
  int done;
  int running;
  int workers;
  int paused;
  double speed;
  long long queued_bytes;
} queue_stats_t;

/* Copies up to max items of tab (running first) and the counters. */
int queue_snapshot(const char *tab, queue_view_t *out, int max, queue_stats_t *stats);

/* Starts workers up to the configured connection count. */
void queue_kick(void);

/* For leaving the app: nothing new starts, running transfers are canceled
   and idle workers leave now instead of half a minute from now. Only asks;
   queue_workers() turns 0 when the last one is gone. */
void queue_stop(void);
int queue_workers(void);
