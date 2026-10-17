#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
strbuf_reserve(strbuf_t *b, size_t extra) {
  size_t need = b->len + extra + 1;
  size_t cap;
  char *data;

  if(need <= b->cap) {
    return 0;
  }
  cap = b->cap ? b->cap : 256;
  while(cap < need) {
    cap *= 2;
  }
  if(!(data = realloc(b->data, cap))) {
    return -1;
  }
  b->data = data;
  b->cap = cap;
  return 0;
}

int
strbuf_append_n(strbuf_t *b, const char *s, size_t n) {
  if(strbuf_reserve(b, n)) {
    return -1;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = 0;
  return 0;
}

int
strbuf_append(strbuf_t *b, const char *s) {
  return strbuf_append_n(b, s, strlen(s));
}

int
strbuf_printf(strbuf_t *b, const char *fmt, ...) {
  va_list args;
  int n;

  va_start(args, fmt);
  n = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if(n < 0 || strbuf_reserve(b, (size_t)n)) {
    return -1;
  }
  va_start(args, fmt);
  vsnprintf(b->data + b->len, (size_t)n + 1, fmt, args);
  va_end(args);
  b->len += (size_t)n;
  return 0;
}

void
strbuf_free(strbuf_t *b) {
  free(b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

/* Length of a valid UTF-8 sequence at s, or 0 if it is invalid. */
static size_t
utf8_seq_len(const unsigned char *s) {
  size_t n;
  size_t i;

  if(s[0] < 0x80) return 1;
  if(s[0] >= 0xC2 && s[0] <= 0xDF) n = 2;
  else if(s[0] >= 0xE0 && s[0] <= 0xEF) n = 3;
  else if(s[0] >= 0xF0 && s[0] <= 0xF4) n = 4;
  else return 0;
  for(i = 1; i < n; i++) {
    if((s[i] & 0xC0) != 0x80) return 0;
  }
  return n;
}

int
json_str(strbuf_t *b, const char *s) {
  const unsigned char *p = (const unsigned char *)(s ? s : "");

  if(strbuf_append(b, "\"")) return -1;
  while(*p) {
    size_t n;

    switch(*p) {
    case '"': if(strbuf_append(b, "\\\"")) return -1; p++; continue;
    case '\\': if(strbuf_append(b, "\\\\")) return -1; p++; continue;
    case '\n': if(strbuf_append(b, "\\n")) return -1; p++; continue;
    case '\r': if(strbuf_append(b, "\\r")) return -1; p++; continue;
    case '\t': if(strbuf_append(b, "\\t")) return -1; p++; continue;
    default: break;
    }
    if(*p < 0x20) {
      if(strbuf_printf(b, "\\u%04x", *p)) return -1;
      p++;
      continue;
    }
    if(!(n = utf8_seq_len(p))) {
      if(strbuf_append(b, "\xEF\xBF\xBD")) return -1;
      p++;
      continue;
    }
    if(strbuf_append_n(b, (const char *)p, n)) return -1;
    p += n;
  }
  return strbuf_append(b, "\"");
}
