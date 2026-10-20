/* The desktop's answers to engine/system.h -- Linux, macOS, Windows under
   MSYS2. This is the build most of the UI is developed against. */

#include "system.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "config.h"
#include "log.h"

/* A font file mapped read-only, so a large face never goes near the heap.
   False when it can't be read, and then that script simply falls back. */
static int
map_font(const char *path, system_font_t *out) {
  struct stat st;
  void *p;
  int fd = open(path, O_RDONLY);

  if(fd < 0) return 0;
  if(fstat(fd, &st) || st.st_size < 1024 || st.st_size > 64L * 1024 * 1024) {
    close(fd);
    return 0;
  }
  p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if(p == MAP_FAILED) return 0;
  out->data = p;
  out->size = (size_t)st.st_size;
  return 1;
}

int
system_data_dir_candidates(const char **out, int max) {
  if(max < 1) return 0;
  out[0] = "./data";  /* beside the binary, so a checkout stays self-contained */
  return 1;
}

int
system_storage_roots(system_root_t *out, int max) {
  static system_root_t roots[4];
  int n = 0;
  const char *test = getenv("CHIRAL_TEST_ROOT");  /* the automated UI tests */
  const char *home = getenv("HOME");

  if(test) roots[n++] = (system_root_t){test, "Test drive", 0};
  if(home) roots[n++] = (system_root_t){home, "Home", 0};
  roots[n++] = (system_root_t){"/tmp", "/tmp", 0};
  roots[n++] = (system_root_t){"/", "Root", 0};

  if(n > max) n = max;
  memcpy(out, roots, (size_t)n * sizeof(*out));
  return n;
}

long long
system_free_space(const char *path, long long *total) {
  struct statvfs vfs;

  if(total) *total = -1;
  if(statvfs(path, &vfs)) return -1;
  if(total) *total = (long long)vfs.f_blocks * (long long)vfs.f_frsize;
  return (long long)vfs.f_bavail * (long long)vfs.f_frsize;
}

void
system_local_ip(char *buf, size_t size) {
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);
  int fd = socket(AF_INET, SOCK_DGRAM, 0);

  snprintf(buf, size, "unknown");
  if(fd < 0) return;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(53);
  inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);
  if(!connect(fd, (struct sockaddr *)&addr, sizeof(addr)) &&
     !getsockname(fd, (struct sockaddr *)&addr, &len) && addr.sin_addr.s_addr) {
    inet_ntop(AF_INET, &addr.sin_addr, buf, (socklen_t)size);
  }
  close(fd);
}

void
system_notify(const char *message) {
  log_msg(0, LOG_STATUS, "%s", message);
}

time_t
system_timegm(const struct tm *tm) {
  struct tm copy = *tm;
  return timegm(&copy);
}

/* ---------- language and fonts ---------- */

const char *
system_language(void) {
  const char *lang = getenv("CHIRAL_LANG");
  static char code[3];

  if(!lang || !*lang) lang = getenv("LANG");
  if(!lang) return "en";
  code[0] = lang[0];
  code[1] = lang[0] ? lang[1] : 0;
  code[2] = 0;
  return code;
}

int
system_renderable_languages(const char **out, int max) {
  /* No restriction: the font below covers every script the app uses. */
  (void)out;
  (void)max;
  return 0;
}

int
system_script_fonts(system_font_t *out, int max, const char *current_lang) {
  /* One font with everything in it, mapped read-only so a 3-8 MB face
     stays out of the app's heap. CHIRAL_FONT_DIR points at a folder of
     per-script faces instead, for testing what a console would load. */
  static const char *const wide[] = {
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    NULL,
  };
  static const struct { const char *lang, *file; } per_script[] = {
    {"ja", "NotoSansCJKjp-Regular.otf"},
    {"ko", "NotoSansCJKkr-Regular.otf"},
    {"zh", "NotoSansCJKsc-Regular.otf"},
    {"ar", "NotoSansArabic-Regular.ttf"},
  };
  const char *dir = getenv("CHIRAL_FONT_DIR");
  int n = 0;

  if(!dir) {
    for(int i = 0; wide[i] && n < max; i++) {
      system_font_t f;
      if(map_font(wide[i], &f)) out[n++] = f;
    }
    return n;
  }
  /* The language in use first, so shared Han characters take its forms. */
  for(int pass = 0; pass < 2 && n < max; pass++) {
    for(size_t i = 0; i < sizeof(per_script) / sizeof(per_script[0]) && n < max; i++) {
      const int matches = !strcmp(per_script[i].lang, current_lang);
      char path[1024];
      system_font_t f;

      if(pass == 0 ? !matches : matches) continue;
      snprintf(path, sizeof(path), "%s/%s", dir, per_script[i].file);
      if(map_font(path, &f)) out[n++] = f;
    }
  }
  return n;
}

const char *
system_ftpd_default_root(void) {
  /* Never "/" on a computer: a development build should not offer the whole
     filesystem to the network. CHIRAL_FTPD_ROOT, or the app's data folder. */
  const char *root = getenv("CHIRAL_FTPD_ROOT");
  return root && *root ? root : data_dir();
}
