#pragma once

#include <stddef.h>

/* Decoded application/x-www-form-urlencoded body. */
typedef struct form {
  size_t count;
  char **keys;
  char **values;
} form_t;

int form_parse(form_t *f, const char *body, size_t size);
void form_free(form_t *f);

/* Returns the first value for key, or def when absent. */
const char *form_get(const form_t *f, const char *key, const char *def);
long long form_get_ll(const form_t *f, const char *key, long long def);
