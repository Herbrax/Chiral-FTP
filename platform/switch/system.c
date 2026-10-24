/* The Switch's answers to engine/system.h. */

#include "system.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>

#include "log.h"
#include "usb_drives.h"

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
  static const system_root_t sd[] = {
    {"/", "SD card", 0},
    {"/switch", "Homebrew", 0},
    {"/Nintendo", "Nintendo", 0},
  };
  /* Then the USB drives mounted right now, as /usb0, /usb1... -- unprefixed,
     like every path here; usb_drives.c routes them to libusbhsfs's
     "ums0:" devices. */
  static usb_drive_t usb[8];
  int n = 0;
  for(int i = 0; i < (int)(sizeof(sd) / sizeof(sd[0])) && n < max; i++) out[n++] = sd[i];
  const int drives = usb_drives_list(usb, 8);
  for(int i = 0; i < drives && n < max; i++) out[n++] = (system_root_t){usb[i].path, usb[i].label, 1};
  return n;
}

/* Size and free space: one answer for the SD card -- every root but the
   USB drives is on it -- asked of its filesystem (fsFsGetTotalSpace/
   GetFreeSpace, what statvfs does underneath), and one per USB drive
   (statvfs through the router), each on a thread of its own and kept for
   ten seconds.

   Never on the caller's thread: free space on FAT32 can mean counting every
   free cluster on the card, and once took minutes and read as a hang
   (docs/AGENT-NOTES.md, 4.5). It measured 0.5 ms on
   firmware 21.1.0 with an exFAT card, but a fuller FAT32 card may still be
   slow; until the first answer, callers get -1, unknown, as before. */
#define SPACE_SLOTS 9  // the SD card, then /usb0 to /usb7

static pthread_mutex_t g_space_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
  long long free, total;
  u64 at;      // armGetSystemTick() of the answer, 0 for none yet
  int asking;
} g_space[SPACE_SLOTS] = {[0 ... SPACE_SLOTS - 1] = {-1, -1, 0, 0}};

static void *
space_main(void *arg) {
  const int slot = (int)(intptr_t)arg;
  s64 free_b = -1, total_b = -1;
  if(slot == 0) {
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    if(sd && (R_FAILED(fsFsGetTotalSpace(sd, "/", &total_b)) || R_FAILED(fsFsGetFreeSpace(sd, "/", &free_b)))) {
      free_b = total_b = -1;
    }
  } else {
    char root[16];
    struct statvfs v;
    snprintf(root, sizeof(root), "/usb%d/", slot - 1);
    if(!statvfs(root, &v)) {
      const s64 unit = v.f_frsize ? (s64)v.f_frsize : (s64)v.f_bsize;
      total_b = (s64)v.f_blocks * unit;
      free_b = (s64)v.f_bavail * unit;
    }
  }
  pthread_mutex_lock(&g_space_lock);
  g_space[slot].free = free_b;
  g_space[slot].total = total_b;
  g_space[slot].at = armGetSystemTick();
  g_space[slot].asking = 0;
  pthread_mutex_unlock(&g_space_lock);
  return NULL;
}

long long
system_free_space(const char *path, long long *total) {
  const int usb = usb_drives_index(path);
  const int slot = usb < 0 ? 0 : usb + 1;
  long long free_b;
  int ask;

  if(slot >= SPACE_SLOTS) {
    if(total) *total = -1;
    return -1;
  }
  pthread_mutex_lock(&g_space_lock);
  ask = !g_space[slot].asking &&
        (!g_space[slot].at || armTicksToNs(armGetSystemTick() - g_space[slot].at) > 10000000000ull);
  if(ask) g_space[slot].asking = 1;
  free_b = g_space[slot].free;
  if(total) *total = g_space[slot].total;
  pthread_mutex_unlock(&g_space_lock);
  if(ask) {
    pthread_t t;
    if(pthread_create(&t, NULL, space_main, (void *)(intptr_t)slot)) {
      pthread_mutex_lock(&g_space_lock);
      g_space[slot].asking = 0;
      pthread_mutex_unlock(&g_space_lock);
    } else {
      pthread_detach(t);
    }
  }
  return free_b;
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
system_device_name(char *buf, size_t size) {
  snprintf(buf, size, "Nintendo Switch");
}

const char *
system_device_kind(void) {
  return "switch";
}

int
system_networks(system_net_t *out, int max) {
  /* One network, the one system_local_ip() reports: nothing to choose. */
  (void)out;
  (void)max;
  return 0;
}

int
system_broadcasts(char (*out)[16], int max) {
  /* 255.255.255.255 alone: the one network has no rival to take it. */
  (void)out;
  (void)max;
  return 0;
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

const char *
system_ftpd_full_root(void) {
  return "/";
}
