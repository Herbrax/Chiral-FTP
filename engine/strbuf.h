#pragma once

#include <stddef.h>

/* Growable string used to build JSON responses. */
typedef struct strbuf {
  char *data;
  size_t len;
  size_t cap;
} strbuf_t;

int strbuf_reserve(strbuf_t *b, size_t extra);
int strbuf_append(strbuf_t *b, const char *s);
int strbuf_append_n(strbuf_t *b, const char *s, size_t n);
int strbuf_printf(strbuf_t *b, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));
void strbuf_free(strbuf_t *b);

/* Append s as a quoted JSON string. Invalid UTF-8 bytes become U+FFFD. */
int json_str(strbuf_t *b, const char *s);
