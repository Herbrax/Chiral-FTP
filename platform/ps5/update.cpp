// Updates on the PS5 (app/update.h). The question and the download go over
// HTTPS with PacBrew's libcurl and OpenSSL, set up the way the boilerplate's
// docs/CURL.md and ProsperoStore do it on an elevated title: the console's own
// sceSsl refuses every public site once a title has left its sandbox, which
// chiral-ftp does (Lapy). Its name lookups and the libc pieces PacBrew's
// archives need are pacbrew_netdb.c and pacbrew_compat.c.
//
// Putting the new version in place is the helper's (platform/ps5/self-updater,
// shipped beside eboot.bin as self-updater.elf), the boilerplate's self-update
// helper made chiral-ftp's:
//
//   1. target() finds the installed copy: a folder with sce_sys/param.json
//      naming PPSA96024, in /data/homebrew or on a drive, two folders deep at
//      most. Two copies or more: "Duplicate copies found, update impossible".
//   2. download() sends the helper to the payload loader (port 9021, like
//      lapy.elf), tells it what is coming, and streams the release zip to it
//      as it downloads. The helper saves it beside the copy, checks its size,
//      unpacks it, makes every file 0777 and checks its param.json: "staged".
//   3. put_in_place() says "apply", and chiral-ftp closes. Once it has gone,
//      the helper moves the old files out of the copy's folder and the new
//      ones in -- the folder ShadowMountPlus mounted stays where it is --
//      deletes the old ones and posts "Chiral-FTP was updated to 1.2. Open it
//      again."
//
// No SHA-256: chiral-ftp's update API gives the release's size and its
// GitHub address, which are what is checked, as on the computers.

#include "update.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <curl/curl.h>

#include "i18n.h"
#include "self-updater/protocol.h"

extern "C" {
#include "config.h"
}

extern "C" {
int sceNetConnect(int socket, const void *address, std::uint32_t address_length);
int sceNetSend(int socket, const void *data, std::size_t length, int flags);
int sceNetRecv(int socket, void *data, std::size_t length, int flags);
int sceNetSetsockopt(int socket, int level, int option, const void *value, std::uint32_t size);
int sceNetSocket(const char *name, int domain, int type, int protocol);
int sceNetSocketClose(int socket);
int sceSystemServiceLoadExec(const char *path, const char *const *arguments);
void arc4random_buf(void *buf, size_t len);
}

namespace {

const char *const TITLE = "PPSA96024";
const char *const CA_LIST = "/system/common/cert/CA_LIST.cer";
const char *const HELPER = "self-updater.elf";
const int SO_NBIO = 0x1200;

/* ---------- HTTPS ---------- */

pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
bool g_curl_ready = false;

void curl_start() {
  g_curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

/* The console's sockets stay blocking without SO_NBIO, and an answer that has
   arrived then waits for the server to close (docs/CURL.md). */
int nonblocking(void *, curl_socket_t socket, curlsocktype) {
  int on = 1;
  return setsockopt(socket, SOL_SOCKET, SO_NBIO, &on, sizeof(on)) == 0 ? CURL_SOCKOPT_OK : CURL_SOCKOPT_ERROR;
}

/* A handle set up for the console: HTTPS only, the console's certificate
   list, IPv4, HTTP/1.1, and a stall rather than a total time ending it. */
CURL *console_handle(const std::string &url) {
  pthread_once(&g_curl_once, curl_start);
  if(!g_curl_ready) return nullptr;
  CURL *c = curl_easy_init();
  if(!c) return nullptr;
  CURLcode ok = CURLE_OK;
  auto set = [&](CURLoption option, auto value) {
    const CURLcode r = curl_easy_setopt(c, option, value);
    if(r != CURLE_OK) ok = r;
  };
  set(CURLOPT_URL, url.c_str());
  set(CURLOPT_NOSIGNAL, 1L);
  set(CURLOPT_CAINFO, CA_LIST);
  set(CURLOPT_SSL_VERIFYPEER, 1L);
  set(CURLOPT_SSL_VERIFYHOST, 2L);
  set(CURLOPT_PROTOCOLS_STR, "https");
  set(CURLOPT_REDIR_PROTOCOLS_STR, "https");
  set(CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
  set(CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
  set(CURLOPT_SOCKOPTFUNCTION, nonblocking);
  set(CURLOPT_CONNECTTIMEOUT, 15L);
  set(CURLOPT_LOW_SPEED_LIMIT, 1L);
  set(CURLOPT_LOW_SPEED_TIME, 20L);
  set(CURLOPT_ACCEPT_ENCODING, "identity");
  set(CURLOPT_USERAGENT, "chiral-ftp/" VERSION_TAG);
  if(ok != CURLE_OK) {
    curl_easy_cleanup(c);
    return nullptr;
  }
  return c;
}

size_t to_string(char *data, size_t size, size_t count, void *user) {
  std::string &out = *static_cast<std::string *>(user);
  const size_t n = size * count;
  if(out.size() + n > 64 * 1024) return 0;
  out.append(data, n);
  return n;
}

/* ---------- the installed copy ---------- */

/* What ShadowMountPlus scans for apps, and where the helper looks too
   (self-updater/updater.cpp, default_roots): the same list on both sides. */
std::vector<std::string> roots() {
  std::vector<std::string> drives{"/data", "/mnt/ext0", "/mnt/ext1"};
  for(int i = 0; i < 8; i++) drives.push_back("/mnt/usb" + std::to_string(i));
  std::vector<std::string> out;
  for(const std::string &d : drives) {
    out.push_back(d + "/homebrew");
    out.push_back(d + "/etaHEN/games");
    if(d != "/data") out.push_back(d);
  }
  return out;
}

bool is_dir(const std::string &path) {
  struct stat st;
  return !lstat(path.c_str(), &st) && S_ISDIR(st.st_mode);
}

/* The titleId a folder's sce_sys/param.json gives, or "". */
std::string title_of(const std::string &folder) {
  FILE *f = fopen((folder + "/sce_sys/param.json").c_str(), "rb");
  if(!f) return "";
  char buf[8192];
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  const char *key = strstr(buf, "\"titleId\"");
  if(!key) return "";
  const char *q = strchr(key + 9, '"');
  const char *e = q ? strchr(q + 1, '"') : nullptr;
  return q && e && e - q - 1 <= 9 ? std::string(q + 1, e) : "";
}

/* Copies of this title in folder and the folders inside it, depth levels
   down; an app's own folder isn't looked into. */
void find_in(const std::string &folder, unsigned depth, std::vector<std::string> &out) {
  DIR *d = opendir(folder.c_str());
  if(!d) return;
  std::vector<std::string> names;
  while(struct dirent *e = readdir(d)) {
    if(strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && strcmp(e->d_name, "self-update")) names.push_back(e->d_name);
  }
  closedir(d);
  for(const std::string &name : names) {
    const std::string path = folder + "/" + name;
    if(!is_dir(path)) continue;
    const std::string id = title_of(path);
    if(id == TITLE) {
      bool known = false;
      for(const std::string &p : out) known = known || p == path;
      if(!known) out.push_back(path);
    } else if(id.empty() && depth > 1) {
      find_in(path, depth - 1, out);
    }
  }
}

/* This build's contentVersion, as tools/content-version.py makes it from the
   same version: 1.2 -> 01.002.000, a test build's -beta left out. */
std::string content_version(const std::string &version) {
  unsigned n[3] = {0, 0, 0};
  int i = 0;
  for(const char *p = version.c_str(); *p && i < 3; p++) {
    if(*p >= '0' && *p <= '9') n[i] = n[i] * 10 + (unsigned)(*p - '0');
    else if(*p == '.') i++;
    else break;
  }
  char out[16];
  snprintf(out, sizeof(out), "%02u.%03u.%03u", n[0] % 100, n[1] % 1000, n[2] % 1000);
  return out;
}

/* ---------- the helper ---------- */

std::string g_folder;  // the copy target() found
int g_helper = -1;     // the connection to it, from download() to put_in_place()

struct NetSockaddrIn {
  std::uint8_t length;
  std::uint8_t family;
  std::uint16_t port;
  std::uint32_t address;
  std::uint16_t virtual_port;
  std::uint8_t zero[6];
};

bool send_all(int socket, const void *data, size_t size) {
  const char *p = static_cast<const char *>(data);
  while(size) {
    const int n = sceNetSend(socket, p, size, 0);
    if(n <= 0) return false;
    p += n;
    size -= (size_t)n;
  }
  return true;
}

bool send_line(int socket, const std::string &line) {
  return send_all(socket, (line + "\n").data(), line.size() + 1);
}

bool read_line(int socket, std::string &line) {
  line.clear();
  for(;;) {
    char c;
    if(sceNetRecv(socket, &c, 1, 0) != 1) return false;
    if(c == '\n') return true;
    if(line.size() >= CHIRAL_UPDATE_LINE) return false;
    line += c;
  }
}

void close_helper() {
  if(g_helper >= 0) sceNetSocketClose(g_helper);
  g_helper = -1;
}

/* The helper program, beside eboot.bin: /app0 while it is ours, else the
   console's mount of the running app, else the copy target() found. */
std::string helper_path() {
  for(const std::string &dir : {std::string("/app0"), std::string("/system_ex/app/") + TITLE, g_folder}) {
    const std::string p = dir + "/" + HELPER;
    if(!dir.empty() && !access(p.c_str(), R_OK)) return p;
  }
  return "";
}

/* Sends the helper to the payload loader, which starts it and hands it this
   connection (platform/ps5/elevation does the same with lapy.elf). */
bool start_helper(std::string *why) {
  const std::string path = helper_path();
  const int file = path.empty() ? -1 : open(path.c_str(), O_RDONLY);
  if(file < 0) {
    *why = N_("This copy can't update itself: only the released program can.");
    return false;
  }
  const int socket = sceNetSocket("chiral_update", 2, 1, 6);
  const int timeout_us = 30 * 1000 * 1000, connect_us = 5 * 1000 * 1000;
  bool ok = socket >= 0 && sceNetSetsockopt(socket, 0xffff, 0x1105, &timeout_us, sizeof(timeout_us)) >= 0 &&
            sceNetSetsockopt(socket, 0xffff, 0x1106, &timeout_us, sizeof(timeout_us)) >= 0 &&
            sceNetSetsockopt(socket, 0xffff, 0x1109, &connect_us, sizeof(connect_us)) >= 0;
  const std::uint16_t port = CHIRAL_UPDATE_LOADER_PORT;
  const NetSockaddrIn loader{sizeof(NetSockaddrIn), 2, (std::uint16_t)((port << 8) | (port >> 8)), 0x0100007f, 0, {0}};
  ok = ok && sceNetConnect(socket, &loader, sizeof(loader)) >= 0;
  char buf[64 * 1024];
  ssize_t n;
  while(ok && (n = read(file, buf, sizeof(buf))) != 0) ok = n > 0 && send_all(socket, buf, (size_t)n);
  close(file);
  if(!ok) {
    if(socket >= 0) sceNetSocketClose(socket);
    *why = N_("The update helper couldn't be started. Is the payload loader (port 9021) running?");
    return false;
  }
  g_helper = socket;
  return true;
}

/* A helper's "fail <reason>", as the reason; its reasons are English, and the
   ones it gives are in the translations. */
std::string reason(const std::string &line) {
  return line.compare(0, 5, "fail ") ? std::string(N_("The update helper stopped.")) : line.substr(5);
}

/* The download, piece by piece, to the helper: a four-byte length, then the
   bytes. */
struct Stream {
  int socket;
  const std::atomic<bool> *cancel;
  std::atomic<long long> *received;
  bool sent = true;
};

size_t to_helper(char *data, size_t size, size_t count, void *user) {
  Stream &s = *static_cast<Stream *>(user);
  const size_t n = size * count;
  if(*s.cancel) return 0;
  for(size_t at = 0; at < n;) {
    const size_t piece = std::min<size_t>(n - at, CHIRAL_UPDATE_PIECE);
    const unsigned char len[4] = {(unsigned char)piece, (unsigned char)(piece >> 8), (unsigned char)(piece >> 16),
                                  (unsigned char)(piece >> 24)};
    if(!send_all(s.socket, len, 4) || !send_all(s.socket, data + at, piece)) {
      s.sent = false;
      return 0;
    }
    at += piece;
  }
  *s.received += (long long)n;
  return n;
}

void *exit_soon(void *) {
  /* Long enough to read the dialog; the helper waits two minutes. */
  sleep(4);
  fflush(nullptr);
  sceSystemServiceLoadExec("exit", nullptr);
  return nullptr;
}

}  // namespace

namespace update {
namespace os {

const char *platform_key() {
  return "ps5";
}

const char *build_key() {
  return platform_key();
}

std::string file_key(const std::string &file) {
  (void)file;
  return platform_key();
}

bool random_bytes(void *out, size_t n) {
  arc4random_buf(out, n);  // platform/ps5/runtime.c, on the CPU's RDRAND
  return true;
}

bool post_json(const std::string &url, const std::string &body, int timeout_s, long *status, std::string *reply) {
  CURL *c = console_handle(url);
  if(!c) return false;
  curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
  curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)timeout_s);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_string);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, reply);
  const CURLcode r = curl_easy_perform(c);
  if(r == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(c);
  return r == CURLE_OK;
}

bool download(const std::string &url, const std::string &path, const std::atomic<bool> &cancel,
              std::atomic<long long> &received, std::string *why) {
  (void)path;  // the zip goes to the helper, which saves it beside the copy
  *why = N_("The download failed. Check the connection and try again.");
  const Offer o = update::offer();
  close_helper();
  if(o.size <= 0 || !start_helper(why)) return false;
  const bool told = send_line(g_helper, CHIRAL_UPDATE_MAGIC) && send_line(g_helper, "update") &&
                    send_line(g_helper, TITLE) && send_line(g_helper, content_version(VERSION_TAG)) &&
                    send_line(g_helper, content_version(o.version)) && send_line(g_helper, std::to_string(o.size)) &&
                    send_line(g_helper, "Chiral-FTP") && send_line(g_helper, o.version);
  std::string line;
  if(!told || !read_line(g_helper, line) || line != "ready") {
    if(told && !line.empty()) *why = reason(line);
    close_helper();
    return false;
  }
  CURL *c = console_handle(url);
  Stream s{g_helper, &cancel, &received};
  bool got = false;
  if(c) {
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);  // GitHub's to its release file host
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_helper);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
    long status = 0;
    got = curl_easy_perform(c) == CURLE_OK && curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK &&
          status == 200;
    curl_easy_cleanup(c);
  }
  if(!got || cancel || !s.sent) {
    if(s.sent) send_line(g_helper, "cancel");  // the helper removes what it wrote
    close_helper();
    return false;
  }
  /* The end mark, then the helper unpacks: "p <done> <total>" until "staged". */
  const unsigned char end[4] = {0, 0, 0, 0};
  if(!send_all(g_helper, end, 4)) {
    close_helper();
    return false;
  }
  while(read_line(g_helper, line)) {
    if(line == "staged") return true;
    if(line.compare(0, 2, "p ")) {
      *why = reason(line);
      break;
    }
  }
  close_helper();
  return false;
}

std::string target(const std::string &file, std::string *why) {
  const size_t n = file.size();
  if(n < 4 || strcasecmp(file.c_str() + n - 4, ".zip")) {
    *why = N_("This copy can't update itself: only the released program can.");
    return "";
  }
  std::vector<std::string> copies;
  for(const std::string &r : roots()) {
    if(is_dir(r)) find_in(r, 2, copies);
  }
  if(copies.size() > 1) {
    *why = N_("Duplicate copies found, update impossible");
    return "";
  }
  if(copies.empty()) {
    *why = N_("chiral-ftp's folder wasn't found, so it can't update itself. A copy installed as an image can't.");
    return "";
  }
  g_folder = copies[0];
  return g_folder;
}

std::string download_path(const std::string &target, const std::string &file) {
  (void)target;
  (void)file;
  /* Nothing is downloaded to a file here: the helper has the zip. A path
     the shared code may remove without harm. */
  return std::string(data_dir()) + "/update.unused";
}

bool put_in_place(const std::string &target, const std::string &download, const std::string &name,
                  std::string *launch, std::string *why) {
  (void)target;
  (void)download;
  (void)name;
  *why = N_("The update helper stopped.");
  std::string line;
  const bool applying = g_helper >= 0 && send_line(g_helper, "apply") && read_line(g_helper, line) && line == "applying";
  close_helper();
  if(!applying) return false;
  /* The helper swaps the files once chiral-ftp is gone, and a title can't
     quit by returning from main (can_quit), so it closes itself, as
     ProsperoStore does. */
  pthread_t t;
  if(!pthread_create(&t, nullptr, exit_soon, nullptr)) pthread_detach(t);
  *launch = "exit";
  return true;
}

bool relaunch(const std::string &launch) {
  (void)launch;
  return true;
}

void clean_up(const std::string &path) {
  (void)path;  // the helper removes its work folder itself
}

const char *restart_note() {
  return N_("chiral-ftp closes now. A notification says when the new version is in place; then open it again.");
}

}  // namespace os
}  // namespace update
