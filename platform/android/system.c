/* Android's answers to engine/system.h: phones, tablets, foldables and
   Android TV. Paths are the plain ones Android shows apps: internal storage
   is /storage/emulated/0, an SD card or USB drive /storage/XXXX-XXXX. */

#include "system.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "SDL.h"

#include "config.h"
#include "log.h"

#define INTERNAL "/storage/emulated/0"

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
  /* The app's private folder (/data/user/0/<id>/files), never shared
     storage: saved passwords and the server's login live here. */
  static char dir[512];
  const char *priv = SDL_AndroidGetInternalStoragePath();

  if(max < 1) return 0;
  snprintf(dir, sizeof(dir), "%s/data", priv && *priv ? priv : ".");
  out[0] = dir;
  return 1;
}

int
system_storage_roots(system_root_t *out, int max) {
  /* Internal storage first, then each card or drive Android has mounted
     under /storage by its volume id. Which is an SD card and which a USB
     drive the paths don't say; asking Android's storage list (StorageManager)
     is still to come. */
  static system_root_t roots[16];
  static char names[16][64], labels[16][96];
  int n = 0;
  DIR *d;
  struct dirent *e;

  roots[n++] = (system_root_t){INTERNAL, "Internal storage", 0};
  if((d = opendir("/storage"))) {
    while((e = readdir(d)) && n < 16) {
      struct stat st;
      if(e->d_name[0] == '.' || !strcmp(e->d_name, "emulated") || !strcmp(e->d_name, "self")) continue;
      snprintf(names[n], sizeof(names[n]), "/storage/%s", e->d_name);
      if(stat(names[n], &st) || !S_ISDIR(st.st_mode)) continue;
      snprintf(labels[n], sizeof(labels[n]), "SD card or USB drive (%s)", e->d_name);
      roots[n] = (system_root_t){names[n], labels[n], 1};
      n++;
    }
    closedir(d);
  }
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

/* Each network's broadcast address, from its address and prefix length;
   0 for a /31 or /32, which has none. */
static unsigned
broadcast_of(unsigned host, unsigned prefix) {
  if(prefix >= 31) return 0;
  return host | (prefix ? ~(0xFFFFFFFFu << (32 - prefix)) : 0xFFFFFFFFu);
}

/* What a person calls an interface: Wi-Fi, the phone's own hotspot,
   Ethernet on a TV. The mobile network's (rmnet, ccmni) is left out
   altogether: nobody on the Wi-Fi can reach it. */
static int
interface_label(const char *name, char *out, size_t size) {
  out[0] = 0;
  if(!strncmp(name, "rmnet", 5) || !strncmp(name, "ccmni", 5) || !strncmp(name, "v4-rmnet", 8) ||
     !strncmp(name, "dummy", 5)) {
    return 0;
  }
  if(!strncmp(name, "wlan", 4)) snprintf(out, size, "Wi-Fi");
  else if(!strncmp(name, "ap", 2) || !strncmp(name, "swlan", 5)) snprintf(out, size, "Hotspot");
  else if(!strncmp(name, "eth", 3)) snprintf(out, size, "Ethernet");
  else if(!strncmp(name, "tun", 3) || !strncmp(name, "wg", 2) || !strncmp(name, "ppp", 3)) snprintf(out, size, "VPN");
  return 1;
}

/* The networks this device is on, Wi-Fi first, and when bcast is given each
   one's broadcast address (host order) beside it. */
static int
list_networks(system_net_t *out, unsigned *bcast, int max) {
  struct ifaddrs *list, *i;
  int n = 0;

  if(getifaddrs(&list)) return 0;
  for(int pass = 0; pass < 2; pass++) {
    for(i = list; i && n < max; i = i->ifa_next) {
      const struct sockaddr_in *a = (const struct sockaddr_in *)i->ifa_addr;
      unsigned host;
      char label[64];

      if(!a || a->sin_family != AF_INET) continue;
      if(!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
      if((pass == 0) != !strncmp(i->ifa_name, "wlan", 4)) continue;
      if(!interface_label(i->ifa_name, label, sizeof(label))) continue;
      host = ntohl(a->sin_addr.s_addr);
      if(!host || (host >> 16) == 0xA9FE) continue;  /* none yet, or self-assigned 169.254 */
      memset(&out[n], 0, sizeof(out[n]));
      snprintf(out[n].name, sizeof(out[n].name), "%s", i->ifa_name);
      snprintf(out[n].label, sizeof(out[n].label), "%s", label);
      inet_ntop(AF_INET, &a->sin_addr, out[n].addr, sizeof(out[n].addr));
      if(bcast) {
        const struct sockaddr_in *m = (const struct sockaddr_in *)i->ifa_netmask;
        const unsigned mask = m && m->sin_family == AF_INET ? ntohl(m->sin_addr.s_addr) : 0;
        unsigned prefix = 0;
        while(prefix < 32 && (mask & (0x80000000u >> prefix))) prefix++;
        bcast[n] = (i->ifa_flags & IFF_BROADCAST) && mask ? broadcast_of(host, prefix) : 0;
      }
      n++;
    }
  }
  freeifaddrs(list);
  return n;
}

void
system_local_ip(char *buf, size_t size) {
  /* Wi-Fi's address if there is one, then the hotspot's or Ethernet's.
     Never the mobile network's: list_networks leaves it out. */
  system_net_t nets[8];
  int n = list_networks(nets, NULL, 8);

  snprintf(buf, size, "%s", n > 0 ? nets[0].addr : "unknown");
}

int
system_networks(system_net_t *out, int max) {
  return list_networks(out, NULL, max);
}

int
system_broadcasts(char (*out)[16], int max) {
  system_net_t nets[16];
  unsigned bcast[16];
  int n = list_networks(nets, bcast, 16), count = 0;

  for(int i = 0; i < n && count < max; i++) {
    struct in_addr a;

    if(!bcast[i]) continue;
    a.s_addr = htonl(bcast[i]);
    inet_ntop(AF_INET, &a, out[count], 16);
    count++;
  }
  return count;
}

void
system_device_name(char *buf, size_t size) {
  /* The model as the phone names itself: "Pixel 9 Pro Fold". The name the
     person gave it in Settings needs Java (Settings.Global), still to come. */
  char model[PROP_VALUE_MAX] = "";

  __system_property_get("ro.product.model", model);
  snprintf(buf, size, "%s", model[0] ? model : "Android");
}

const char *
system_device_kind(void) {
  return SDL_IsAndroidTV() ? "androidtv" : "android";
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
  static char code[3] = "en";
  SDL_Locale *list = SDL_GetPreferredLocales();

  if(list && list[0].language && list[0].language[0]) {
    code[0] = list[0].language[0];
    code[1] = list[0].language[1];
    code[2] = 0;
  }
  SDL_free(list);
  return code;
}

int
system_renderable_languages(const char **out, int max) {
  /* All of them: Android has CJK and Arabic faces (system_script_fonts). */
  (void)out;
  (void)max;
  return 0;
}

int
system_script_fonts(system_font_t *out, int max, const char *current_lang) {
  /* The system's own faces. Their file names vary a little by vendor and
     version, so each script tries the usual ones in turn. */
  static const char *const cjk[] = {"/system/fonts/NotoSansCJK-Regular.ttc", "/system/fonts/NotoSansSC-Regular.otf",
                                    "/system/fonts/DroidSansFallback.ttf", NULL};
  static const char *const arabic[] = {"/system/fonts/NotoNaskhArabic-Regular.ttf",
                                       "/system/fonts/NotoNaskhArabicUI-Regular.ttf", NULL};
  const char *const *scripts[2] = {cjk, arabic};
  int n = 0;

  /* The script in use first, so shared Han characters take its forms. */
  const int arabic_first = current_lang && !strcmp(current_lang, "ar");
  for(int k = 0; k < 2 && n < max; k++) {
    const char *const *files = scripts[arabic_first ? 1 - k : k];
    for(int i = 0; files[i]; i++) {
      system_font_t f;
      if(map_font(files[i], &f)) {
        out[n++] = f;
        break;
      }
    }
  }
  return n;
}

const char *
system_ftpd_default_root(void) {
  /* Internal storage, where the person's photos, downloads and documents
     are. The app's private folder isn't in it. */
  return INTERNAL;
}

const char *
system_ftpd_full_root(void) {
  /* Every card and drive as well. */
  return "/storage";
}
