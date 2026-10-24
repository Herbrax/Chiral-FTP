/* The desktop's answers to engine/system.h -- Linux, macOS, Windows under
   MSYS2. This is the build most of the UI is developed against. On Windows,
   paths are the POSIX ones of win32/posix.h: "/c/Users", and "/" for the
   drives. */

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
#include <unistd.h>
#ifdef _WIN32
#include <iphlpapi.h>
#include <windows.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/statvfs.h>
#endif
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

#ifdef _WIN32
/* The folder holding chiral-ftp.exe, as a POSIX path. */
static void
exe_dir(char *out, size_t size) {
  wchar_t path[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(NULL, path, sizeof(path) / sizeof(path[0]));
  wchar_t *slash;

  out[0] = 0;
  if(!n || n >= sizeof(path) / sizeof(path[0])) return;
  if((slash = wcsrchr(path, L'\\'))) *slash = 0;
  chiral_posix_path(path, out, size);
}

static int
enter_dir(const char *posix) {
  wchar_t w[PATH_MAX + 8];
  return chiral_win_path(posix, w, sizeof(w) / sizeof(w[0])) > 0 && SetCurrentDirectoryW(w);
}
#endif

int
system_data_dir_candidates(const char **out, int max) {
  if(max < 1) return 0;
#ifdef _WIN32
  {
    /* Beside the program when a "data" folder is there already (a copy
       carried on a USB stick); otherwise %APPDATA%\chiral-ftp, where
       Windows programs keep their settings. Either way the folder is
       reached from the working directory, by a relative path: libssh and
       libssh2 open the host key and known_hosts themselves, with the C
       library's fopen(), which knows nothing of "/c/...". */
    static char rel[32];
    char dir[1024], data[1100];
    struct stat st;
    const char *appdata = getenv("APPDATA");

    exe_dir(dir, sizeof(dir));
    snprintf(data, sizeof(data), "%s/data", dir);
    if(dir[0] && !stat(data, &st) && S_ISDIR(st.st_mode) && enter_dir(dir)) {
      snprintf(rel, sizeof(rel), "./data");
    } else if(appdata && *appdata) {
      chiral_posix_path_utf8(appdata, dir, sizeof(dir));
      if(enter_dir(dir)) snprintf(rel, sizeof(rel), "./chiral-ftp");
    }
    if(rel[0]) {
      out[0] = rel;
      return 1;
    }
  }
#endif
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
      /* The settings code makes the last folder only; a new account may not
         have ~/.local/share yet. */
      if(data_home && *data_home) {
        mkdir(data_home, 0700);
        snprintf(xdg, sizeof(xdg), "%s/chiral-ftp", data_home);
      } else if(home && *home) {
        snprintf(xdg, sizeof(xdg), "%s/.local", home);
        mkdir(xdg, 0700);
        snprintf(xdg, sizeof(xdg), "%s/.local/share", home);
        mkdir(xdg, 0700);
        snprintf(xdg, sizeof(xdg), "%s/.local/share/chiral-ftp", home);
      }
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
  static system_root_t roots[30];
  int n = 0;
  const char *test = getenv("CHIRAL_TEST_ROOT");  /* the automated UI tests */
  const char *home = getenv("HOME");

  if(test) roots[n++] = (system_root_t){test, "Test drive", 0};
  if(home) roots[n++] = (system_root_t){home, "Home", 0};
#ifdef _WIN32
  {
    /* Each drive, as Explorer names it: "Windows (C:)", or "D:" when it
       has no label. Card readers and disc drives only with something in
       them; a network drive's label isn't asked for, since a server that
       has gone away takes its time to say so. */
    static char paths[26][4], labels[26][80];
    const DWORD drives = GetLogicalDrives();

    for(int i = 0; i < 26 && n < (int)(sizeof(roots) / sizeof(roots[0])); i++) {
      wchar_t root[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0}, name[MAX_PATH + 1];
      char label[64];
      UINT type;

      if(!(drives & (1u << i))) continue;
      type = GetDriveTypeW(root);
      if(type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) continue;
      snprintf(paths[i], sizeof(paths[i]), "/%c", 'a' + i);
      label[0] = 0;
      if(type != DRIVE_REMOTE && GetVolumeInformationW(root, name, MAX_PATH + 1, NULL, NULL, NULL, NULL, 0) &&
         name[0]) {
        WideCharToMultiByte(CP_UTF8, 0, name, -1, label, sizeof(label), NULL, NULL);
      }
      if(label[0]) snprintf(labels[i], sizeof(labels[i]), "%s (%c:)", label, 'A' + i);
      else snprintf(labels[i], sizeof(labels[i]), "%c:", 'A' + i);
      roots[n++] = (system_root_t){paths[i], labels[i], type == DRIVE_REMOVABLE || type == DRIVE_CDROM};
    }
  }
#else
  roots[n++] = (system_root_t){"/tmp", "/tmp", 0};
  roots[n++] = (system_root_t){"/", "Root", 0};
#endif

  if(n > max) n = max;
  memcpy(out, roots, (size_t)n * sizeof(*out));
  return n;
}

long long
system_free_space(const char *path, long long *total) {
#ifdef _WIN32
  wchar_t w[PATH_MAX + 8];
  ULARGE_INTEGER avail, size;

  if(total) *total = -1;
  /* "/" is every drive at once: no one answer. */
  if(chiral_win_path(path, w, sizeof(w) / sizeof(w[0])) <= 0) return -1;
  if(!GetDiskFreeSpaceExW(w, &avail, &size, NULL)) return -1;
  if(total) *total = (long long)size.QuadPart;
  return (long long)avail.QuadPart;
#else
  struct statvfs vfs;

  if(total) *total = -1;
  if(statvfs(path, &vfs)) return -1;
  if(total) *total = (long long)vfs.f_blocks * (long long)vfs.f_frsize;
  return (long long)vfs.f_bavail * (long long)vfs.f_frsize;
#endif
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

#ifndef _WIN32
/* What the system calls an interface: "Wi-Fi", "Ethernet", "Thunderbolt
   Bridge" on a Mac (already in the user's language), "VPN" for a tunnel.
   Empty when there is nothing better than the interface's own name.
   (Windows hands over the name itself: system_networks.) */
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
#endif

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

/* Each network's broadcast address, from its address and prefix length;
   0 for a /31 or /32, which has none. */
static unsigned
broadcast_of(unsigned host, unsigned prefix) {
  if(prefix >= 31) return 0;
  return host | (prefix ? ~(0xFFFFFFFFu << (32 - prefix)) : 0xFFFFFFFFu);
}

/* The networks this machine is on, for system_networks(), and when bcast
   is given, each one's broadcast address (host order) beside it, for
   system_broadcasts(). */
#ifdef _WIN32
static int
list_networks(system_net_t *out, unsigned *bcast, int max) {
  /* Named as in Settings > Network: "Wi-Fi", "Ethernet 2". */
  ULONG size = 16 * 1024;
  IP_ADAPTER_ADDRESSES *list = NULL;
  ULONG r = ERROR_BUFFER_OVERFLOW;
  int n = 0;

  for(int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
    free(list);
    if(!(list = malloc(size))) return 0;
    r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             NULL, list, &size);
  }
  if(r != NO_ERROR) {
    free(list);
    return 0;
  }
  for(IP_ADAPTER_ADDRESSES *a = list; a && n < max; a = a->Next) {
    if(a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for(IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < max; u = u->Next) {
      const struct sockaddr_in *in = (const struct sockaddr_in *)u->Address.lpSockaddr;
      unsigned host;

      if(!in || in->sin_family != AF_INET) continue;
      host = ntohl(in->sin_addr.s_addr);
      if(!host || (host >> 16) == 0xA9FE) continue;  /* none yet, or self-assigned 169.254 */
      memset(&out[n], 0, sizeof(out[n]));
      WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, out[n].label, sizeof(out[n].label), NULL, NULL);
      snprintf(out[n].name, sizeof(out[n].name), "%s", out[n].label);
      inet_ntop(AF_INET, &in->sin_addr, out[n].addr, sizeof(out[n].addr));
      if(bcast) bcast[n] = broadcast_of(host, u->OnLinkPrefixLength);
      n++;
    }
  }
  free(list);
  return n;
}
#else
static int
list_networks(system_net_t *out, unsigned *bcast, int max) {
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
    if(bcast) {
      const struct sockaddr_in *m = (const struct sockaddr_in *)i->ifa_netmask;
      const unsigned mask = m && m->sin_family == AF_INET ? ntohl(m->sin_addr.s_addr) : 0;
      unsigned prefix = 0;
      while(prefix < 32 && (mask & (0x80000000u >> prefix))) prefix++;
      bcast[n] = (i->ifa_flags & IFF_BROADCAST) && mask ? broadcast_of(host, prefix) : 0;
    }
    n++;
  }
  freeifaddrs(list);
  return n;
}
#endif

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
#ifdef _WIN32
  /* The display language chosen in Windows' settings. LANG, if set at
     all, belongs to whatever terminal started the program. */
  static char ui_lang[16];
  if(!lang || !*lang) {
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    if(LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) &&
       WideCharToMultiByte(CP_UTF8, 0, name, -1, ui_lang, sizeof(ui_lang), NULL, NULL)) {
      lang = ui_lang;
    }
  }
#else
  if(!lang || !*lang) lang = getenv("LANG");
#endif
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

#ifdef _WIN32
  /* Windows has no one font with every script in it, but one per script,
     all in %WINDIR%\Fonts. */
  static const struct { const char *lang, *file; } windows[] = {
    {"ja", "YuGothM.ttc"},
    {"zh", "msyh.ttc"},
    {"ko", "malgun.ttf"},
    {"ar", "segoeui.ttf"},
  };
  char fonts[512];
  const char *windir = getenv("WINDIR");

  if(!dir && windir && *windir) {
    chiral_posix_path_utf8(windir, fonts, sizeof(fonts) - 8);
    strcat(fonts, "/Fonts");
    for(int pass = 0; pass < 2 && n < max; pass++) {
      for(size_t i = 0; i < sizeof(windows) / sizeof(windows[0]) && n < max; i++) {
        const int matches = !strcmp(windows[i].lang, current_lang);
        char path[1024];
        system_font_t f;

        if(pass == 0 ? !matches : matches) continue;
        snprintf(path, sizeof(path), "%s/%s", fonts, windows[i].file);
        if(map_font(path, &f)) out[n++] = f;
      }
    }
    return n;
  }
#endif
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
  const char *home = getenv("HOME");  /* %USERPROFILE% on Windows (win32/posix.c) */

  if(root && *root) return root;
  if(home && *home) return home;
  return "/tmp";
}

const char *
system_ftpd_full_root(void) {
  /* Windows too: there "/" is the folder of drives, /c, /d... */
  return "/";
}
