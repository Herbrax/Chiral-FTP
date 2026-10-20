#pragma once

#include "strbuf.h"

/* Message log shown in the top pane, like FileZilla's. */
typedef enum log_kind {
  LOG_STATUS,
  LOG_COMMAND,
  LOG_RESPONSE,
  LOG_ERROR,
} log_kind_t;

/* conn is 0 for the browsing connection, 1..N for transfer slots. */
void log_msg(int conn, log_kind_t kind, const char *fmt, ...)
  __attribute__((format(printf, 3, 4)));

/* Appends a JSON array of messages newer than since; returns the last seq. */
unsigned long log_json(strbuf_t *b, unsigned long since);

void log_clear(void);

typedef void (*log_cb)(void *ctx, unsigned long seq, long long time, int conn,
                       log_kind_t kind, const char *text);
/* Calls cb for each message newer than since; returns the last seq. */
unsigned long log_each(unsigned long since, log_cb cb, void *ctx);

/* Appends a line to STARTUP_LOG and syncs it, so a failed launch
   leaves a trace of how far it got. */
void startup_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
