#include "form.h"

#include <stdlib.h>
#include <string.h>

static int
hexval(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static char *
url_decode(const char *s, size_t n) {
  char *out = malloc(n + 1);
  size_t j = 0;

  if(!out) return NULL;
  for(size_t i = 0; i < n; i++) {
    if(s[i] == '+') {
      out[j++] = ' ';
    } else if(s[i] == '%' && i + 2 < n && hexval(s[i + 1]) >= 0 &&
              hexval(s[i + 2]) >= 0) {
      out[j++] = (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
      i += 2;
    } else {
      out[j++] = s[i];
    }
  }
  out[j] = 0;
  return out;
}

static int
form_add(form_t *f, char *key, char *value) {
  char **keys;
  char **values;

  if(!key || !value ||
     !(keys = realloc(f->keys, (f->count + 1) * sizeof(char *)))) {
    free(key);
    free(value);
    return -1;
  }
  f->keys = keys;
  if(!(values = realloc(f->values, (f->count + 1) * sizeof(char *)))) {
    free(key);
    free(value);
    return -1;
  }
  f->values = values;
  f->keys[f->count] = key;
  f->values[f->count] = value;
  f->count++;
  return 0;
}

int
form_parse(form_t *f, const char *body, size_t size) {
  size_t pos = 0;

  memset(f, 0, sizeof(*f));
  while(body && pos < size) {
    size_t end = pos;
    size_t eq;

    while(end < size && body[end] != '&') end++;
    eq = pos;
    while(eq < end && body[eq] != '=') eq++;
    if(end > pos) {
      char *key = url_decode(body + pos, eq - pos);
      char *value = eq < end ? url_decode(body + eq + 1, end - eq - 1) :
                               url_decode("", 0);
      if(form_add(f, key, value)) {
        form_free(f);
        return -1;
      }
    }
    pos = end + 1;
  }
  return 0;
}

void
form_free(form_t *f) {
  for(size_t i = 0; i < f->count; i++) {
    free(f->keys[i]);
    free(f->values[i]);
  }
  free(f->keys);
  free(f->values);
  memset(f, 0, sizeof(*f));
}

const char *
form_get(const form_t *f, const char *key, const char *def) {
  for(size_t i = 0; i < f->count; i++) {
    if(!strcmp(f->keys[i], key)) return f->values[i];
  }
  return def;
}

long long
form_get_ll(const form_t *f, const char *key, long long def) {
  const char *v = form_get(f, key, NULL);
  char *end;
  long long n;

  if(!v || !*v) return def;
  n = strtoll(v, &end, 10);
  return *end ? def : n;
}
