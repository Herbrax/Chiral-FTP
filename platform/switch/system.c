/* The Switch's answers to engine/system.h. */

#include "system.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>

#include "log.h"

int
system_data_dir_candidates(const char **out, int max) {
  /* Beside the other homebrew on the SD card, and unprefixed: libnx makes
     sdmc the default device, so "/switch/chiral-ftp" is
     "sdmc:/switch/chiral-ftp". Keeping every path in the app unprefixed is
     what lets path_join, parent-walking and the FTP server's root behave
     exactly as they do on a PC. */
  if(max < 1) return 0;
  out[0] = "/switch/chiral-ftp";
  return 1;
}

int
system_storage_roots(system_root_t *out, int max) {
  static const system_root_t roots[] = {
    {"/", "SD card", 0},
    {"/switch", "Homebrew", 0},
    {"/Nintendo", "Nintendo", 0},
    /* USB mass storage needs libusbhsfs, which mounts drives as "ums0:"
       and friends -- device-prefixed paths, which is exactly what the rule
       above avoids. See docs/PORTING.md. */
  };
  int n = (int)(sizeof(roots) / sizeof(roots[0]));
  if(n > max) n = max;
  memcpy(out, roots, (size_t)n * sizeof(*out));
  return n;
}

long long
system_free_space(const char *path, long long *total) {
  /* Always -1, on purpose -- do not "fix" this. libnx implements statvfs
     perfectly well, but answering it on FAT32 or exFAT means counting every
     free cluster on the card, and for the three roots the drive list shows
     that took minutes and read as a hang. Callers treat -1 as unknown and
     omit the figure. */
  (void)path;
  if(total) *total = -1;
  return -1;
}

void
system_local_ip(char *buf, size_t size) {
  /* libnx asks nifm. The usual trick -- connect a UDP socket somewhere and
     read back the source address -- does not work here: Horizon leaves it
     unset on a datagram socket nothing has been sent on. */
  struct in_addr host;

  snprintf(buf, size, "unknown");
  host.s_addr = (in_addr_t)gethostid();
  if(host.s_addr && host.s_addr != htonl(INADDR_LOOPBACK)) {
    inet_ntop(AF_INET, &host, buf, (socklen_t)size);
  }
}

void
system_notify(const char *message) {
  /* Horizon has no notification service homebrew may post to, so this goes
     to the app's own message log, which the server screens already show. */
  log_msg(0, LOG_STATUS, "%s", message);
}

time_t
system_timegm(const struct tm *tm) {
  /* newlib has no timegm, and mktime is wrong: the caller's tm is UTC and
     mktime would read it as the console's local time. Days-from-civil, then
     seconds -- no time zone involved either way. */
  int year = tm->tm_year + 1900;
  unsigned month = (unsigned)tm->tm_mon + 1;
  unsigned day = (unsigned)tm->tm_mday;
  int era;
  unsigned year_of_era, day_of_year, day_of_era;
  long long days;

  year -= month <= 2; /* March-based year */
  era = (year >= 0 ? year : year - 399) / 400;
  year_of_era = (unsigned)(year - era * 400);
  day_of_year = (153u * (month + (month > 2 ? -3u : 9u)) + 2u) / 5u + day - 1u;
  day_of_era = year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;
  days = (long long)era * 146097 + (long long)day_of_era - 719468; /* to 1970-01-01 */
  return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

/* ---------- language and fonts ---------- */

const char *
system_language(void) {
  /* set:sys is open from platform/switch/init.c. */
  u64 code = 0;
  SetLanguage language = SetLanguage_ENUS;

  if(R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &language))) {
    switch(language) {
    case SetLanguage_FR: case SetLanguage_FRCA: return "fr";
    case SetLanguage_ES: case SetLanguage_ES419: return "es";
    case SetLanguage_IT: return "it";
    case SetLanguage_RU: return "ru";
    /* Japanese, Korean and Chinese fall through to English: the
       translations exist, but this build has no font for them (below). */
    default: return "en";
    }
  }
  return "en";
}

int
system_renderable_languages(const char **out, int max) {
  /* What the bundled Noto Sans covers, Cyrillic included. Japanese, Korean,
     Chinese and Arabic are translated and stay in the catalogue, but the
     console's own faces come out of plGetSharedFontByType inside Nintendo's
     BFTTF container -- header and body XOR-obfuscated -- which the renderer
     reads as not-a-font. Offering them would mean a screen of empty boxes.
     Decoding BFTTF (a magic check, then a word-wise XOR) brings them back;
     docs/PORTING.md has the detail. */
  static const char *const codes[] = {"en", "es", "fr", "it", "ru"};
  int n = (int)(sizeof(codes) / sizeof(codes[0]));

  if(n > max) n = max;
  for(int i = 0; i < n; i++) out[i] = codes[i];
  return n;
}

int
system_script_fonts(system_font_t *out, int max, const char *current_lang) {
  /* Nothing to merge until BFTTF is decoded: see above. */
  (void)out;
  (void)max;
  (void)current_lang;
  return 0;
}

const char *
system_ftpd_default_root(void) {
  return "/";  /* the whole SD card */
}
