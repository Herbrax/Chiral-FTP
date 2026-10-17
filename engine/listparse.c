/* Directory listing parser for MLSD, Unix "ls -l" and DOS/IIS formats.
   Modeled on the formats handled by FileZilla's directorylistingparser.cpp. */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ftp.h"

#define MAX_TOKENS 16

static const char *g_months[12] = {
  "jan", "feb", "mar", "apr", "may", "jun",
  "jul", "aug", "sep", "oct", "nov", "dec",
};

/* Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant). */
static long long
days_from_civil(int y, int m, int d) {
  long long era;
  unsigned yoe, doy, doe;

  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long long)doe - 719468;
}

static long long
make_time(int y, int mon, int d, int h, int min, int s) {
  if(mon < 1 || mon > 12 || d < 1 || d > 31 || h < 0 || h > 23 || min < 0 ||
     min > 59 || s < 0 || s > 60) {
    return -1;
  }
  return days_from_civil(y, mon, d) * 86400 + h * 3600 + min * 60 + s;
}

static int
civil_year(time_t now) {
  long long days = (long long)now / 86400;
  int y = 1970;

  for(;;) {
    long long len = days_from_civil(y + 1, 1, 1) - days_from_civil(y, 1, 1);
    if(days < len) return y;
    days -= len;
    y++;
  }
}

int
ftp_listing_add(ftp_listing_t *l, const ftp_entry_t *e) {
  if(l->count == l->cap) {
    size_t cap = l->cap ? l->cap * 2 : 64;
    ftp_entry_t *p = realloc(l->entries, cap * sizeof(*p));

    if(!p) return -1;
    l->entries = p;
    l->cap = cap;
  }
  l->entries[l->count++] = *e;
  return 0;
}

void
ftp_listing_free(ftp_listing_t *l) {
  free(l->entries);
  memset(l, 0, sizeof(*l));
}

static int
month_index(const char *s) {
  for(int i = 0; i < 12; i++) {
    if(!strncasecmp(s, g_months[i], 3) && (!s[3] || strlen(s) <= 4)) return i + 1;
  }
  return 0;
}

static int
all_digits(const char *s) {
  if(!*s) return 0;
  for(; *s; s++) {
    if(!isdigit((unsigned char)*s)) return 0;
  }
  return 1;
}

static int
skip_name(const char *name) {
  return !name[0] || !strcmp(name, ".") || !strcmp(name, "..");
}

static void
copy(char *dst, size_t size, const char *src) {
  snprintf(dst, size, "%s", src);
}

static void
init_entry(ftp_entry_t *e) {
  memset(e, 0, sizeof(*e));
  e->size = -1;
  e->mtime = -1;
}

/* "type=file;size=12;modify=20240101120000; name" */
static int
parse_mlsd(ftp_listing_t *l, char *line) {
  char *sp = strchr(line, ' ');
  char *fact;
  char *save = NULL;
  ftp_entry_t e;

  if(!sp || !memchr(line, '=', (size_t)(sp - line))) return -1;
  *sp = 0;
  init_entry(&e);
  copy(e.name, sizeof(e.name), sp + 1);
  for(fact = strtok_r(line, ";", &save); fact; fact = strtok_r(NULL, ";", &save)) {
    char *eq = strchr(fact, '=');
    char *val;

    if(!eq) continue;
    *eq = 0;
    val = eq + 1;
    if(!strcasecmp(fact, "type")) {
      if(!strcasecmp(val, "cdir") || !strcasecmp(val, "pdir")) return 0;
      if(!strcasecmp(val, "dir")) e.is_dir = 1;
      if(!strncasecmp(val, "OS.unix=slink", 13) || !strncasecmp(val, "OS.unix=symlink", 15)) {
        char *target = strchr(val + 8, ':');
        e.is_link = 1;
        if(target) copy(e.target, sizeof(e.target), target + 1);
      }
    } else if(!strcasecmp(fact, "size") || !strcasecmp(fact, "sizd")) {
      e.size = strtoll(val, NULL, 10);
    } else if(!strcasecmp(fact, "modify")) {
      int y, mo, d, h, mi, s;
      if(sscanf(val, "%4d%2d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) == 6) {
        e.mtime = make_time(y, mo, d, h, mi, s);
      }
    } else if(!strcasecmp(fact, "UNIX.mode")) {
      /* Python servers (pyftpdlib) write the mode as "0o755". */
      if(val[0] == '0' && (val[1] == 'o' || val[1] == 'O')) val += 2;
      unsigned mode = (unsigned)strtoul(val, NULL, 8);
      snprintf(e.perm, sizeof(e.perm), "%04o", mode & 07777);
    } else if(!strcasecmp(fact, "UNIX.owner") || !strcasecmp(fact, "UNIX.ownername") ||
              !strcasecmp(fact, "UNIX.uid")) {
      copy(e.owner, sizeof(e.owner), val);
    } else if(!strcasecmp(fact, "UNIX.group") || !strcasecmp(fact, "UNIX.groupname") ||
              !strcasecmp(fact, "UNIX.gid")) {
      copy(e.group, sizeof(e.group), val);
    } else if(!strcasecmp(fact, "perm") && !e.perm[0]) {
      copy(e.perm, sizeof(e.perm), val);
    }
  }
  if(skip_name(e.name)) return 0;
  return ftp_listing_add(l, &e);
}

typedef struct tokens {
  int count;
  char *tok[MAX_TOKENS];
  size_t end[MAX_TOKENS];  /* offset just past each token in the line */
} tokens_t;

static void
tokenize(const char *line, char *buf, tokens_t *t) {
  size_t i = 0;
  size_t j = 0;

  t->count = 0;
  while(line[i] && t->count < MAX_TOKENS) {
    while(line[i] == ' ' || line[i] == '\t') i++;
    if(!line[i]) break;
    t->tok[t->count] = buf + j;
    while(line[i] && line[i] != ' ' && line[i] != '\t') buf[j++] = line[i++];
    buf[j++] = 0;
    t->end[t->count] = i;
    t->count++;
  }
}

static const char *
rest_after(const char *line, const tokens_t *t, int idx) {
  const char *p = line + t->end[idx];

  /* Exactly one separator: names may start with spaces. */
  return *p ? p + 1 : p;
}

/* Assigns only on a full match; sscanf may store partial results otherwise. */
static int
parse_hhmm(const char *s, int *h, int *m) {
  int hh, mm;

  if(sscanf(s, "%d:%d", &hh, &mm) != 2) return 0;
  *h = hh;
  *m = mm;
  return 1;
}

static int
parse_iso_date(const char *s, int *y, int *mo, int *d) {
  int yy, mm, dd;

  if(sscanf(s, "%4d-%2d-%2d", &yy, &mm, &dd) != 3) return 0;
  *y = yy;
  *mo = mm;
  *d = dd;
  return 1;
}

static int
is_unix_perm(const char *s) {
  return strlen(s) >= 10 && strchr("-dlbcpsDn", s[0]) &&
         strchr("r-", s[1]) && strchr("w-", s[2]);
}

/* "drwxr-xr-x 2 user group 4096 Jan  1 12:00 name", with or without group or
   link count, and with "2024-01-01 12:00" style dates. */
static int
parse_unix(ftp_listing_t *l, const char *line, time_t now) {
  char buf[2048];
  tokens_t t;
  ftp_entry_t e;
  const char *name = NULL;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0;
  int size_idx = -1;

  if(strlen(line) >= sizeof(buf)) return -1;
  tokenize(line, buf, &t);
  if(t.count < 6 || !is_unix_perm(t.tok[0])) return -1;

  for(int i = 2; i + 2 < t.count && !name; i++) {
    int m = month_index(t.tok[i]);

    if(m && all_digits(t.tok[i - 1]) && all_digits(t.tok[i + 1])) {
      mo = m;
      d = atoi(t.tok[i + 1]);
      if(parse_hhmm(t.tok[i + 2], &h, &mi)) {
        int cur = civil_year(now);
        y = cur;
        if(make_time(y, mo, d, h, mi, 0) > (long long)now + 86400) y = cur - 1;
      } else if(all_digits(t.tok[i + 2])) {
        y = atoi(t.tok[i + 2]);
        h = mi = 0;
      } else {
        continue;
      }
      size_idx = i - 1;
      name = rest_after(line, &t, i + 2);
    } else if(parse_iso_date(t.tok[i], &y, &mo, &d) &&
              all_digits(t.tok[i - 1]) && parse_hhmm(t.tok[i + 1], &h, &mi)) {
      size_idx = i - 1;
      name = rest_after(line, &t, i + 1);
    }
  }
  if(!name || size_idx < 1) return -1;

  init_entry(&e);
  e.is_dir = t.tok[0][0] == 'd' || t.tok[0][0] == 'D';
  e.is_link = t.tok[0][0] == 'l';
  e.size = strtoll(t.tok[size_idx], NULL, 10);
  e.mtime = make_time(y, mo, d, h, mi, 0);
  copy(e.perm, sizeof(e.perm), t.tok[0]);
  e.perm[10] = 0;
  {
    /* Fields between the permissions and the size: [links] owner [group]. */
    int n = size_idx - 1;
    int first = 1;

    if(n >= 2 && all_digits(t.tok[1])) {
      first = 2;
      n--;
    }
    if(n >= 1) copy(e.owner, sizeof(e.owner), t.tok[first]);
    if(n >= 2) copy(e.group, sizeof(e.group), t.tok[first + 1]);
  }
  if(e.is_link) {
    const char *arrow = strstr(name, " -> ");
    if(arrow) {
      copy(e.target, sizeof(e.target), arrow + 4);
      snprintf(e.name, sizeof(e.name), "%.*s", (int)(arrow - name), name);
    } else {
      copy(e.name, sizeof(e.name), name);
    }
  } else {
    copy(e.name, sizeof(e.name), name);
  }
  if(skip_name(e.name)) return 0;
  return ftp_listing_add(l, &e);
}

/* "01-31-24  09:15PM       <DIR>          name" */
static int
parse_dos(ftp_listing_t *l, const char *line) {
  char buf[2048];
  tokens_t t;
  ftp_entry_t e;
  int mo, d, y, h, mi;
  char ampm[3] = "";

  if(strlen(line) >= sizeof(buf)) return -1;
  tokenize(line, buf, &t);
  if(t.count < 4) return -1;
  if(sscanf(t.tok[0], "%d-%d-%d", &mo, &d, &y) != 3 &&
     sscanf(t.tok[0], "%d/%d/%d", &mo, &d, &y) != 3) {
    return -1;
  }
  if(sscanf(t.tok[1], "%d:%d%2s", &h, &mi, ampm) < 2) return -1;
  if(y < 70) y += 2000;
  else if(y < 100) y += 1900;
  if(!strcasecmp(ampm, "PM") && h < 12) h += 12;
  if(!strcasecmp(ampm, "AM") && h == 12) h = 0;

  init_entry(&e);
  if(!strcasecmp(t.tok[2], "<DIR>")) {
    e.is_dir = 1;
  } else if(all_digits(t.tok[2])) {
    e.size = strtoll(t.tok[2], NULL, 10);
  } else {
    return -1;
  }
  e.mtime = make_time(y, mo, d, h, mi, 0);
  copy(e.name, sizeof(e.name), rest_after(line, &t, 2));
  /* DOS listings pad with several spaces before the name. */
  {
    char *p = e.name;
    while(*p == ' ') p++;
    memmove(e.name, p, strlen(p) + 1);
  }
  if(skip_name(e.name)) return 0;
  return ftp_listing_add(l, &e);
}

void
ftp_parse_listing(ftp_listing_t *l, char *data, size_t size, int mlsd, time_t now) {
  size_t pos = 0;

  while(data && pos < size) {
    char *line = data + pos;
    char *nl = memchr(line, '\n', size - pos);
    size_t len = nl ? (size_t)(nl - line) : size - pos;

    pos += len + 1;
    if(len && line[len - 1] == '\r') len--;
    line[len] = 0;
    if(!len) continue;
    if(!strncasecmp(line, "total ", 6)) continue;
    if(mlsd) {
      if(!parse_mlsd(l, line)) continue;
    }
    if(!parse_unix(l, line, now)) continue;
    if(!parse_dos(l, line)) continue;
  }
}
