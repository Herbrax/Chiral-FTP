#pragma once

#include "strbuf.h"

#define POLICY_RESUME    0  /* skip identical size, resume smaller, else overwrite */
#define POLICY_OVERWRITE 1
#define POLICY_SKIP      2
#define POLICY_RENAME    3

typedef struct settings {
  int connections;     /* 1..MAX_TRANSFER_CONNECTIONS */
  int policy;
  int preserve_mtime;
  int retries;
} settings_t;

void settings_load(void);
settings_t settings_get(void);
int settings_set(const settings_t *s);
void settings_json(strbuf_t *b);

/* Opaque JSON documents saved by the UI (sites, history). */
int store_read(const char *name, strbuf_t *out);
int store_write(const char *name, const char *data, size_t size);

int ensure_data_dir(void);
