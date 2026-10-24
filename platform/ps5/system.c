/* The PS5's answers to engine/system.h. */

#include "system.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* A notification, through the kernel's own request path. */
typedef struct notify_request {
  char unused[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int device, notify_request_t *request, size_t size,
                                     int unused);

/* A font file mapped read-only: a 3-8 MB face stays out of the app's heap. */
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
  /* /data when ShadowMountPlus has given the title a writable one;
     otherwise the app's own download storage. settings.c takes the first
     that it can create a file in and list. */
  if(max < 2) return 0;
  out[0] = "/data/chiral-ftp";
  out[1] = "/download0/chiral-ftp";
  return 2;
}

int
system_storage_roots(system_root_t *out, int max) {
  static system_root_t roots[16];
  static char usb_path[8][16], usb_label[8][16];
  int n = 0;

  roots[n++] = (system_root_t){"/data", "Internal storage", 0};
  roots[n++] = (system_root_t){"/data/homebrew", "Homebrew", 0};
  for(int i = 0; i < 8 && n < (int)(sizeof(roots) / sizeof(roots[0])); i++) {
    snprintf(usb_path[i], sizeof(usb_path[i]), "/mnt/usb%d", i);
    snprintf(usb_label[i], sizeof(usb_label[i]), "USB drive %d", i);
    roots[n++] = (system_root_t){usb_path[i], usb_label[i], 1};
  }
  roots[n++] = (system_root_t){"/mnt/ext0", "Extended storage", 1};
  roots[n++] = (system_root_t){"/mnt/ext1", "Extended storage 2", 1};
  roots[n++] = (system_root_t){"/mnt/disc", "Disc", 1};
  roots[n++] = (system_root_t){"/user", "User data", 0};
  roots[n++] = (system_root_t){"/", "Root", 0};

  if(n > max) n = max;
  memcpy(out, roots, (size_t)n * sizeof(*out));
  return n;
}

long long
system_free_space(const char *path, long long *total) {
  /* fstatvfs on the folder, opened -- never statvfs(path). Both come from
     libSceLibcInternal, but statvfs kills a title on any path at all (it
     was what stopped the app at "listing drives"), while fstatvfs answered
     every folder in under 0.1 ms, measured on the console (docs/AGENT-NOTES.md 3.3).
     (statfs is only in libkernel_sys, for payloads, and libc.a's is a raw
     system call a title may not make.)

     An empty slot -- /mnt/usb3 with nothing in it, /mnt/ext0 without an
     M.2 drive -- is a placeholder of a few megabytes: unknown, not a
     nearly-full drive. */
  struct statvfs v;
  long long unit, size;
  int fd = open(path, O_RDONLY | O_DIRECTORY);

  if(total) *total = -1;
  if(fd < 0) return -1;
  if(fstatvfs(fd, &v)) {
    close(fd);
    return -1;
  }
  close(fd);
  unit = v.f_frsize ? (long long)v.f_frsize : (long long)v.f_bsize;
  size = (long long)v.f_blocks * unit;
  if(size <= 16LL * 1024 * 1024) return -1;
  if(total) *total = size;
  return (long long)v.f_bavail * unit;
}

void
system_local_ip(char *buf, size_t size) {
  /* The source address the kernel picks for an outbound route; a UDP
     connect sends nothing. */
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
system_device_name(char *buf, size_t size) {
  snprintf(buf, size, "PS5");
}

const char *
system_device_kind(void) {
  return "ps5";
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
  notify_request_t req;

  memset(&req, 0, sizeof(req));
  snprintf(req.message, sizeof(req.message), "%s", message);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

time_t
system_timegm(const struct tm *tm) {
  struct tm copy = *tm;
  return timegm(&copy);
}

/* ---------- language and fonts ---------- */

int sceSystemServiceParamGetInt(int param, int *value);

const char *
system_language(void) {
  int id = -1;

  if(sceSystemServiceParamGetInt(1 /* SCE_SYSTEM_SERVICE_PARAM_ID_LANG */, &id) == 0) {
    switch(id) {
    case 0: return "ja";
    case 2: case 22: return "fr";   /* French, French (Canada) */
    case 3: case 20: return "es";   /* Spanish, Spanish (Latin America) */
    case 5: return "it";
    case 8: return "ru";
    case 9: return "ko";
    case 10: case 11: return "zh";  /* Chinese traditional, simplified */
    case 21: return "ar";
    default: return "en";
    }
  }
  return "en";
}

int
system_renderable_languages(const char **out, int max) {
  /* All of them: the console's own faces below cover every script. */
  (void)out;
  (void)max;
  return 0;
}

int
system_script_fonts(system_font_t *out, int max, const char *current_lang) {
  /* The console's fonts, as ProsperoEden reads them, mapped read-only. The
     language in use goes first so Han characters that Chinese and Japanese
     share take its forms. */
  static const struct { const char *lang; const char *files[3]; } faces[] = {
    {"ja", {"SSTJpPro-Regular.otf", "SIE-RDC-Pr5N-M-JPN.otf", "SCE-RDC-R-JPN.otf"}},
    {"ko", {"YoonGothicProSIE760.otf", "SCEPS4Yoongd-Medium.otf", NULL}},
    {"zh", {"DFHEI5-SONY.ttf", NULL, NULL}},
    {"ar", {"SSTArabic-Medium.otf", "SSTArabic-Roman.otf", NULL}},
  };
  int n = 0;

  for(int pass = 0; pass < 2 && n < max; pass++) {
    for(size_t i = 0; i < sizeof(faces) / sizeof(faces[0]) && n < max; i++) {
      const int matches = !strcmp(faces[i].lang, current_lang);
      if(pass == 0 ? !matches : matches) continue;
      for(int k = 0; k < 3 && faces[i].files[k]; k++) {
        char path[512];
        system_font_t f;
        snprintf(path, sizeof(path), "/preinst/common/font/%s", faces[i].files[k]);
        if(map_font(path, &f)) {
          out[n++] = f;
          break;  /* best available for this script */
        }
      }
    }
  }
  return n;
}

const char *
system_ftpd_default_root(void) {
  return "/";  /* the whole console, which is the point of it */
}

const char *
system_ftpd_full_root(void) {
  return "/";
}
