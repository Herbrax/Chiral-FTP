// Updates on the Switch (app/update.h). The question and the download go over
// HTTPS with devkitPro's libcurl, whose TLS is the console's own ssl service
// (curl's libnx backend): it checks servers against the console's trusted
// certificates, kept up to date by Nintendo's system updates, so the app
// carries none of its own.
//
// Putting the new version in place:
//   1. The .nro's own path comes from hbmenu (argv): sdmc:/switch/chiral-ftp.nro.
//   2. The new one is downloaded beside it as chiral-ftp.new.nro.
//   3. The old one becomes chiral-ftp.nro.old (the SD card's FAT won't rename
//      over a file, so an older .old goes first), and the new one takes its
//      name. hbloader read the running .nro into memory, so renaming it is fine.
//   4. The .old is deleted straight away (at the next start, if that failed),
//      and chiral-ftp closes, as it does when quit.

#include "update.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <curl/curl.h>
#include <switch.h>

#include "i18n.h"

extern "C" {
#include "log.h"
}

namespace {

/* ---------- HTTPS ---------- */

pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
bool g_curl_ready = false;

/* Also starts the ssl service (and csrng) for curl's libnx backend. */
void curl_start() {
  g_curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

/* A handle for the update API or GitHub: HTTPS only, the certificate (against
   the console's trusted ones) and the name checked, and a stall rather than a
   total time ending a download. */
CURL *handle(const std::string &url) {
  pthread_once(&g_curl_once, curl_start);
  if(!g_curl_ready) {
    log_msg(0, LOG_ERROR, "Update: curl didn't start (the console's ssl service?)");
    return nullptr;
  }
  CURL *c = curl_easy_init();
  if(!c) return nullptr;
  CURLcode ok = CURLE_OK;
  auto set = [&](CURLoption option, auto value) {
    const CURLcode r = curl_easy_setopt(c, option, value);
    if(r != CURLE_OK) ok = r;
  };
  set(CURLOPT_URL, url.c_str());
  set(CURLOPT_NOSIGNAL, 1L);
  set(CURLOPT_SSL_VERIFYPEER, 1L);
  set(CURLOPT_SSL_VERIFYHOST, 2L);
  /* No file of certificates: the backend would import one on top of the
     console's, and a built-in default would be a path the SD card hasn't. */
  set(CURLOPT_CAINFO, (const char *)nullptr);
  set(CURLOPT_CAPATH, (const char *)nullptr);
  /* switch-curl is 7.69: the _STR forms of these came in 7.85. */
  set(CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
  set(CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
  set(CURLOPT_CONNECTTIMEOUT, 15L);
  set(CURLOPT_LOW_SPEED_LIMIT, 1L);
  set(CURLOPT_LOW_SPEED_TIME, 20L);
  set(CURLOPT_USERAGENT, "chiral-ftp/" VERSION_TAG);
  if(ok != CURLE_OK) {
    log_msg(0, LOG_ERROR, "Update: curl refused an option: %s (curl %d)", curl_easy_strerror(ok), (int)ok);
    curl_easy_cleanup(c);
    return nullptr;
  }
  return c;
}

/* Why curl gave up, in the message log: the shared code only knows that it
   did. The libnx backend's messages carry the ssl service's result code. */
void log_failure(const char *what, CURLcode r, const char *detail) {
  log_msg(0, LOG_ERROR, "Update %s: %s (curl %d)", what, detail[0] ? detail : curl_easy_strerror(r), (int)r);
  /* The ssl service checks certificates' dates, and a console kept offline
     often has the wrong one: the usual cause, so the log says what to do. */
  if(r == CURLE_SSL_CONNECT_ERROR || r == CURLE_PEER_FAILED_VERIFICATION) {
    char date[32] = "?";
    const time_t now = time(nullptr);
    struct tm tm;
    if(localtime_r(&now, &tm)) strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &tm);
    log_msg(0, LOG_ERROR,
            "Update %s: the secure connection failed. The console's clock says %s: if that's wrong, set it in "
            "System Settings > System > Date and Time, then try again.",
            what, date);
  }
}

size_t to_string(char *data, size_t size, size_t count, void *user) {
  std::string &out = *static_cast<std::string *>(user);
  const size_t n = size * count;
  if(out.size() + n > 64 * 1024) return 0;
  out.append(data, n);
  return n;
}

struct Download {
  FILE *file;
  const std::atomic<bool> *cancel;
  std::atomic<long long> *received;
};

size_t to_file(char *data, size_t size, size_t count, void *user) {
  Download &d = *static_cast<Download *>(user);
  const size_t n = size * count;
  if(*d.cancel || fwrite(data, 1, n, d.file) != n) return 0;
  *d.received += (long long)n;
  return n;
}

/* ---------- the .nro ---------- */

bool exists(const std::string &path) {
  struct stat st;
  return !stat(path.c_str(), &st);
}

/* The running .nro, as hbmenu passed it: the first word of the argv string,
   in quotes when it has spaces. "" when it didn't (nxlink, a forwarder). */
std::string self_path() {
  const char *argv = envHasArgv() ? (const char *)envGetArgv() : nullptr;
  if(!argv) return "";
  std::string first;
  const char *p = argv;
  while(*p == ' ') p++;
  if(*p == '"') {
    const char *end = strchr(p + 1, '"');
    first = end ? std::string(p + 1, end) : std::string(p + 1);
  } else {
    const char *end = strchr(p, ' ');
    first = end ? std::string(p, end) : std::string(p);
  }
  if(first.size() < 4 || strcasecmp(first.c_str() + first.size() - 4, ".nro")) return "";
  /* hbmenu writes sdmc:/...; a bare /switch/... is on the SD card too. */
  if(first[0] == '/') first = "sdmc:" + first;
  return exists(first) ? first : "";
}

/* chiral-ftp.nro -> chiral-ftp.new.nro */
std::string new_path(const std::string &nro) {
  return nro.substr(0, nro.size() - 4) + ".new.nro";
}

}  // namespace

namespace update {
namespace os {

const char *platform_key() {
  return "switch";
}

const char *build_key() {
  return platform_key();
}

std::string file_key(const std::string &file) {
  (void)file;
  return platform_key();
}

bool random_bytes(void *out, size_t n) {
  randomGet(out, n);
  return true;
}

bool post_json(const std::string &url, const std::string &body, int timeout_s, long *status, std::string *reply) {
  CURL *c = handle(url);
  if(!c) return false;
  curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
  curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)timeout_s);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_string);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, reply);
  char err[CURL_ERROR_SIZE] = "";
  curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
  const CURLcode r = curl_easy_perform(c);
  if(r == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
  else log_failure("check", r, err);
  curl_slist_free_all(headers);
  curl_easy_cleanup(c);
  return r == CURLE_OK;
}

bool download(const std::string &url, const std::string &path, const std::atomic<bool> &cancel,
              std::atomic<long long> &received, std::string *why) {
  *why = N_("The download failed. Check the connection and try again.");
  CURL *c = handle(url);
  FILE *f = c ? fopen(path.c_str(), "wb") : nullptr;
  if(!f) {
    if(c) curl_easy_cleanup(c);
    return false;
  }
  Download d{f, &cancel, &received};
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);  // GitHub's to its release file host
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_file);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &d);
  char err[CURL_ERROR_SIZE] = "";
  curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
  long status = 0;
  const CURLcode r = curl_easy_perform(c);
  if(r != CURLE_OK && !cancel) log_failure("download", r, err);
  const bool ok = r == CURLE_OK && curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status == 200;
  curl_easy_cleanup(c);
  return fclose(f) == 0 && ok && !cancel;
}

std::string target(const std::string &file, std::string *why) {
  const std::string self = self_path();
  if(self.empty() || file.size() < 4 || strcasecmp(file.c_str() + file.size() - 4, ".nro")) {
    *why = N_("This copy can't update itself: only the released program can.");
    return "";
  }
  return self;
}

std::string download_path(const std::string &target, const std::string &file) {
  (void)file;
  return new_path(target);
}

bool put_in_place(const std::string &target, const std::string &download, const std::string &name,
                  std::string *launch, std::string *why) {
  (void)name;  // the .nro keeps its own name
  *why = N_("chiral-ftp could not put the new version in place. This copy is unchanged.");
  const std::string old = target + ".old";
  unlink(old.c_str());  // FAT renames over nothing
  if(rename(target.c_str(), old.c_str())) return false;
  if(rename(download.c_str(), target.c_str())) {
    rename(old.c_str(), target.c_str());  // the old one back: this copy is unchanged
    return false;
  }
  /* In place: the old one has done its job. Nothing reads it any more (no
     RomFS; hbloader has the program in memory), so it goes now. A delete that
     fails leaves it for clean_up at the next start. */
  unlink(old.c_str());
  *launch = target;
  return true;
}

bool relaunch(const std::string &launch) {
  /* Nothing to start: chiral-ftp closes as it always does, and the user opens
     the new one from hbmenu. */
  (void)launch;
  return true;
}

void clean_up(const std::string &path) {
  (void)path;
  /* At every start: what the last update left, the old .nro, and a download
     cut short. */
  const std::string self = self_path();
  if(self.empty()) return;
  unlink((self + ".old").c_str());
  unlink(new_path(self).c_str());
}

const char *restart_note() {
  return N_("chiral-ftp closes now; open it again from hbmenu.");
}

}  // namespace os
}  // namespace update
