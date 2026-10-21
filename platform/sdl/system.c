/* The desktop's answers to engine/system.h -- Linux, macOS, Windows under
   MSYS2. This is the build most of the UI is developed against. */

#include "system.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
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
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <mach-o/dyld.h>
#endif

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

#ifdef __APPLE__
/* True when running as chiral-ftp.app. Finder starts a bundle in "/", so
   "./data" means nothing there, and the bundle itself is no place to write. */
static int
in_app_bundle(void) {
  char path[1024];
  uint32_t size = sizeof(path);
  return !_NSGetExecutablePath(path, &size) && strstr(path, ".app/Contents/MacOS/");
}
#endif

int
system_data_dir_candidates(const char **out, int max) {
  if(max < 1) return 0;
#ifdef __APPLE__
  {
    static char support[200];
    const char *home = getenv("HOME");

    if(home && *home && in_app_bundle()) {
      snprintf(support, sizeof(support), "%s/Library/Application Support/chiral-ftp", home);
      out[0] = support;
      return 1;
    }
  }
#endif
#if !defined(__APPLE__) && !defined(_WIN32)
  {
    /* Run as an AppImage: the program sits in a read-only mount, so the
       settings go where Linux desktops keep them, $XDG_DATA_HOME
       (~/.local/share) /chiral-ftp. */
    static char xdg[512];
    const char *data_home = getenv("XDG_DATA_HOME"), *home = getenv("HOME");

    if(getenv("APPIMAGE")) {
      if(data_home && *data_home) snprintf(xdg, sizeof(xdg), "%s/chiral-ftp", data_home);
      else if(home && *home) snprintf(xdg, sizeof(xdg), "%s/.local/share/chiral-ftp", home);
      if(xdg[0]) {
        out[0] = xdg;
        return 1;
      }
    }
  }
#endif
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

/* What the system calls an interface: "Wi-Fi", "Ethernet", "Thunderbolt
   Bridge" on a Mac (already in the user's language), "VPN" for a tunnel.
   Empty when there is nothing better than the interface's own name. */
static void
interface_label(const char *name, char *out, size_t size) {
  out[0] = 0;
  if(!strncmp(name, "utun", 4) || !strncmp(name, "ipsec", 5) || !strncmp(name, "ppp", 3) ||
     !strncmp(name, "tun", 3) || !strncmp(name, "wg", 2)) {
    snprintf(out, size, "VPN");
    return;
  }
#ifdef __APPLE__
  {
    CFArrayRef all = SCNetworkInterfaceCopyAll();
    CFIndex count = all ? CFArrayGetCount(all) : 0;

    for(CFIndex i = 0; i < count; i++) {
      SCNetworkInterfaceRef itf = (SCNetworkInterfaceRef)CFArrayGetValueAtIndex(all, i);
      CFStringRef bsd = SCNetworkInterfaceGetBSDName(itf);
      CFStringRef shown = SCNetworkInterfaceGetLocalizedDisplayName(itf);
      char bsd_name[32];

      if(!bsd || !shown || !CFStringGetCString(bsd, bsd_name, sizeof(bsd_name), kCFStringEncodingUTF8)) continue;
      if(strcmp(bsd_name, name)) continue;
      CFStringGetCString(shown, out, (CFIndex)size, kCFStringEncodingUTF8);
      break;
    }
    if(all) CFRelease(all);
  }
#endif
}

void
system_device_name(char *buf, size_t size) {
  /* What the machine is called in its own settings: "Simo's MacBook Pro"
     on a Mac (System Settings > General > About), the computer name on
     Windows, the host name elsewhere. */
  snprintf(buf, size, "%s", "Computer");
#ifdef __APPLE__
  {
    CFStringRef name = SCDynamicStoreCopyComputerName(NULL, NULL);
    if(name) {
      CFStringGetCString(name, buf, (CFIndex)size, kCFStringEncodingUTF8);
      CFRelease(name);
    }
  }
#elif defined(_WIN32)
  if(getenv("COMPUTERNAME")) snprintf(buf, size, "%s", getenv("COMPUTERNAME"));
#else
  {
    char host[256];
    if(!gethostname(host, sizeof(host))) {
      char *dot = strchr(host, '.');
      if(dot && dot != host) *dot = 0;
      snprintf(buf, size, "%s", host);
    }
  }
#endif
}

const char *
system_device_kind(void) {
#ifdef __APPLE__
  return "mac";
#elif defined(_WIN32)
  return "windows";
#else
  return "linux";
#endif
}

int
system_networks(system_net_t *out, int max) {
  struct ifaddrs *list, *i;
  int n = 0;

  if(getifaddrs(&list)) return 0;
  for(i = list; i && n < max; i = i->ifa_next) {
    const struct sockaddr_in *a = (const struct sockaddr_in *)i->ifa_addr;
    unsigned host;

    if(!a || a->sin_family != AF_INET) continue;
    if(!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_RUNNING) || (i->ifa_flags & IFF_LOOPBACK)) continue;
    host = ntohl(a->sin_addr.s_addr);
    if(!host || (host >> 16) == 0xA9FE) continue;  /* none yet, or self-assigned 169.254 */
    memset(&out[n], 0, sizeof(out[n]));
    snprintf(out[n].name, sizeof(out[n].name), "%s", i->ifa_name);
    inet_ntop(AF_INET, &a->sin_addr, out[n].addr, sizeof(out[n].addr));
    interface_label(i->ifa_name, out[n].label, sizeof(out[n].label));
    n++;
  }
  freeifaddrs(list);
  return n;
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

#ifdef __APPLE__
  /* The language chosen in System Settings. An app started from Finder has
     no LANG at all, and a terminal's LANG needn't match the system's. */
  static char preferred[16];
  if(!lang || !*lang) {
    CFArrayRef langs = CFLocaleCopyPreferredLanguages();
    if(langs && CFArrayGetCount(langs) > 0 &&
       CFStringGetCString((CFStringRef)CFArrayGetValueAtIndex(langs, 0), preferred, sizeof(preferred),
                          kCFStringEncodingUTF8)) {
      lang = preferred;
    }
    if(langs) CFRelease(langs);
  }
#endif
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
  /* The person's own folder: /Users/<name> on a Mac, /home/<name> on Linux,
     %USERPROFILE% on Windows. CHIRAL_FTPD_ROOT overrides it (the tests).
     The app's data folder inside it is never served (localfs_private:
     saved passwords, the SFTP server's private key), and sharing the whole
     computer is a separate, deliberate choice (system_ftpd_full_root). */
  const char *root = getenv("CHIRAL_FTPD_ROOT");
  const char *home = getenv("HOME");

  if(root && *root) return root;
#ifdef _WIN32
  if(getenv("USERPROFILE") && *getenv("USERPROFILE")) return getenv("USERPROFILE");
#endif
  if(home && *home) return home;
  return "/tmp";
}

const char *
system_ftpd_full_root(void) {
#ifdef _WIN32
  static char drive[8];
  const char *d = getenv("SystemDrive");
  snprintf(drive, sizeof(drive), "%s/", d && *d ? d : "C:");
  return drive;
#else
  return "/";
#endif
}
