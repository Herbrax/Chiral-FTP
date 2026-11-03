// Updates: see update.h. The question, the answer, what the user chose about
// it, and the download -- the same everywhere; replacing the program is the
// platform's (update::os).
//
// Raw pthreads, as in ui.cpp's Jobs and the engine: the C++ threading
// classes are what once killed the console builds at startup.

#include "update.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <vector>
#include <pthread.h>
#include <unistd.h>

#include "i18n.h"
#include "json.h"
#include "platform_config.h"

extern "C" {
#include "config.h"
#include "log.h"
#include "settings.h"
#include "strbuf.h"
}

namespace update {

#if CHIRAL_HAS_UPDATER

namespace {

const char *const API = "https://chiral-ftp.com/api/v1/check";
const char *const PAGE = "https://chiral-ftp.com/download/";
/* Downloads come from the project's own releases and nowhere else, whatever
   the answer says. GitHub doesn't mind the case; this doesn't either. */
const char *const RELEASES = "https://github.com/herbrax/chiral-ftp/releases/download/";
const int CHECK_TIMEOUT_S = 8;
const double SNOOZE_S = 7 * 24 * 3600.0;

/* What is remembered, in update.json in the data folder (data_file_write,
   out of the web app's reach): the device id, the switch, and what the user
   said about the last offer. */
struct Saved {
  std::string device;
  bool check = true;
  std::string skip;        // the version "Skip this version" was chosen for
  double snooze_until = 0; // "Not now": quiet until then
};

/* Never destroyed: a check still waiting on the network when the app quits
   is left to the exit rather than waited for, and must not find these taken
   apart by the static destructors. */
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
Saved &g_saved = *new Saved;
State g_state = IDLE;
bool g_manual = false;
Offer &g_offer = *new Offer;
std::string &g_error = *new std::string, &g_launch = *new std::string;
std::atomic<bool> g_cancel{false};
std::atomic<long long> g_received{0};
pthread_t g_worker;
bool g_worker_started = false;

struct Lock {
  Lock() { pthread_mutex_lock(&g_mutex); }
  ~Lock() { pthread_mutex_unlock(&g_mutex); }
};

const char *const SAVED = "update.json";

void load() {
  strbuf_t b = {0, 0, 0};
  std::vector<json::Object> items;
  if(!data_file_read(SAVED, &b) && b.data) json::parse_array(std::string(b.data, b.len), items);
  strbuf_free(&b);
  if(items.empty()) return;
  g_saved.device = json::get_str(items[0], "device");
  g_saved.check = json::get_bool(items[0], "check", true);
  g_saved.skip = json::get_str(items[0], "skip");
  g_saved.snooze_until = json::get_num(items[0], "snooze_until", 0);
}

// With the lock held.
void save() {
  json::Object o;
  json::set_str(o, "device", g_saved.device);
  json::set_bool(o, "check", g_saved.check);
  json::set_str(o, "skip", g_saved.skip);
  json::set_num(o, "snooze_until", g_saved.snooze_until);
  const std::string text = json::dump_array({o});
  if(data_file_write(SAVED, text.data(), text.size())) log_msg(0, LOG_ERROR, "Could not save %s", SAVED);
}

/* 128 random bits as hex: no name, no address, nothing about the machine --
   only enough to count each device once. */
std::string new_device_id() {
  unsigned char bytes[16];
  if(!os::random_bytes(bytes, sizeof(bytes))) {
    /* No system randomness: the clock and the address of a local will do
       to tell two devices apart. */
    unsigned long long x = (unsigned long long)time(nullptr) ^ (unsigned long long)(size_t)&bytes;
    for(unsigned char &b : bytes) {
      x = x * 6364136223846793005ULL + 1442695040888963407ULL;
      b = (unsigned char)(x >> 56);
    }
  }
  char out[33];
  for(int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", bytes[i]);
  return out;
}

/* Versions compare by their numbers, part by part, up to the first thing that
   isn't one: 1.10 > 1.9, 1.2.1 > 1.2, 1.2-beta == 1.2. */
std::vector<long> numbers(const std::string &v) {
  std::vector<long> out;
  size_t i = v.size() && (v[0] == 'v' || v[0] == 'V') ? 1 : 0;
  while(i < v.size() && isdigit((unsigned char)v[i])) {
    long n = 0;
    while(i < v.size() && isdigit((unsigned char)v[i])) n = n * 10 + (v[i++] - '0');
    out.push_back(n);
    if(i < v.size() && v[i] == '.') i++;
    else break;
  }
  return out;
}

bool newer(const std::string &a, const std::string &b) {
  std::vector<long> x = numbers(a), y = numbers(b);
  if(x.empty()) return false;
  while(x.size() < y.size()) x.push_back(0);
  while(y.size() < x.size()) y.push_back(0);
  return x > y;
}

bool same_nocase(const char *a, const char *b, size_t n) {
  for(size_t i = 0; i < n; i++) {
    if(tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  }
  return true;
}

bool starts_with_nocase(const std::string &s, const char *prefix) {
  const size_t n = strlen(prefix);
  return s.size() >= n && same_nocase(s.c_str(), prefix, n);
}

bool ends_with(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size() && !s.compare(s.size() - suffix.size(), suffix.size(), suffix);
}

/* A file name the release could really have: no folders in it, nothing odd. */
bool plain_name(const std::string &s) {
  if(s.empty() || s.size() > 128 || s[0] == '.') return false;
  for(unsigned char c : s) {
    if(!isalnum(c) && c != '.' && c != '-' && c != '_') return false;
  }
  return true;
}

void set_state(State s) {
  Lock l;
  g_state = s;
}

void join() {
  if(g_worker_started) pthread_join(g_worker, nullptr);
  g_worker_started = false;
}

bool start(void *(*fn)(void *), void *arg) {
  join();
  /* A stack of its own size: the console's default is small, and a TLS
     handshake plus the PS5's helper start (platform/ps5/update.cpp) need more. */
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 1024 * 1024);
  g_worker_started = !pthread_create(&g_worker, &attr, fn, arg);
  pthread_attr_destroy(&attr);
  return g_worker_started;
}

void *run_check(void *arg) {
  const bool manual = arg != nullptr;
  /* Asked as the machine, not as this build: an x64 copy on an ARM PC is a
     windows-arm64 device, and says what it is in "build" so that it can be
     moved onto the ARM program. */
  const std::string machine = os::platform_key(), build = os::build_key();
  char body[256];
  if(machine == build) {
    snprintf(body, sizeof(body), "{\"platform\":\"%s\",\"version\":\"%s\",\"device\":\"%s\"}",
             machine.c_str(), VERSION_TAG, g_saved.device.c_str());
  } else {
    snprintf(body, sizeof(body), "{\"platform\":\"%s\",\"version\":\"%s\",\"device\":\"%s\",\"build\":\"%s\"}",
             machine.c_str(), VERSION_TAG, g_saved.device.c_str(), build.c_str());
  }
  long status = 0;
  std::string reply;
  const bool answered = os::post_json(API, body, CHECK_TIMEOUT_S, &status, &reply);
  json::Object top;
  std::map<std::string, json::Object> inner;
  if(!answered || status != 200 || !json::parse_object(reply, top, &inner)) {
    log_msg(0, LOG_STATUS, "Update check: no answer (status %ld)", status);
    set_state(manual ? CHECK_FAILED : IDLE);
    return nullptr;
  }
  const json::Object &latest = inner["latest"];
  Offer o;
  o.version = json::get_str(latest, "version");
  o.file = json::get_str(latest, "file");
  o.url = json::get_str(latest, "url");
  o.size = (long long)json::get_num(latest, "size", 0);
  o.page = json::get_str(top, "page");
  if(!starts_with_nocase(o.page, "https://")) o.page = PAGE;
  /* Only what is really newer, and really ours: the answer is checked, not
     trusted. A copy running on hardware it wasn't built for also takes the
     same version built for that hardware. A copy that was never trades
     itself for one that isn't: an ARM PC offered the x64 program, from a
     release without an ARM one, keeps its own. */
  const std::string fits = os::file_key(o.file);
  const bool onto_machine = build != machine && fits == machine && !newer(VERSION_TAG, o.version);
  const bool off_machine = build == machine && fits != machine;
  const bool offered = json::get_bool(top, "update") && (newer(o.version, VERSION_TAG) || onto_machine) &&
                       !off_machine && starts_with_nocase(o.url, RELEASES) && plain_name(o.file) &&
                       ends_with(o.url, "/" + o.file);
  if(machine != build) {
    log_msg(0, LOG_STATUS, "Update check: this is a %s copy on a %s machine", build.c_str(), machine.c_str());
  }
  if(!offered) {
    set_state(manual ? UP_TO_DATE : IDLE);
    return nullptr;
  }
  std::string why;
  if(os::target(o.file, &why).empty()) o.cannot = why;
  Lock l;
  if(!manual && (o.version == g_saved.skip || (double)time(nullptr) < g_saved.snooze_until)) {
    g_state = IDLE;  // asked not to be told
    return nullptr;
  }
  log_msg(0, LOG_STATUS, "Update check: chiral-ftp %s is available", o.version.c_str());
  g_offer = o;
  g_state = AVAILABLE;
  return nullptr;
}

void fail(const std::string &msg) {
  log_msg(0, LOG_ERROR, "Update: %s", msg.c_str());
  Lock l;
  g_error = msg;
  g_state = FAILED;
}

void *run_install(void *) {
  const Offer o = offer();
  std::string why;
  const std::string target = os::target(o.file, &why);
  if(target.empty()) {
    fail(why);
    return nullptr;
  }
  const std::string dl = os::download_path(target, o.file);
  unlink(dl.c_str());
  g_received = 0;
  const bool got = os::download(o.url, dl, g_cancel, g_received, &why);
  if(!got || g_cancel || (o.size > 0 && g_received != o.size)) {
    unlink(dl.c_str());
    if(g_cancel) set_state(AVAILABLE);  // back to the offer
    else fail(got ? N_("The download was not the expected file. Nothing was changed.") : why);
    return nullptr;
  }
  set_state(INSTALLING);
  /* A file named for its version ("chiral-ftp-1.2-windows-x64.exe") takes
     the new one's name, so that it doesn't go on saying 1.2; anything else
     keeps its own, and with it any shortcut to it. */
  const size_t slash = target.find_last_of('/');
  std::string name = slash == std::string::npos ? target : target.substr(slash + 1);
  if(name.find(std::string("-") + VERSION_TAG + "-") != std::string::npos) name = o.file;
  std::string launch;
  if(!os::put_in_place(target, dl, name, &launch, &why)) {
    unlink(dl.c_str());
    fail(why);
    return nullptr;
  }
  Lock l;
  if(launch.empty()) {
    log_msg(0, LOG_STATUS, "Update: chiral-ftp %s handed to the system's installer", o.version.c_str());
    g_state = HANDED_OFF;
    return nullptr;
  }
  log_msg(0, LOG_STATUS, "Update: chiral-ftp %s is in place, restarting", o.version.c_str());
  g_launch = launch;
  g_state = RESTARTING;
  return nullptr;
}

void *run_clean_up(void *arg) {
  std::string *left = static_cast<std::string *>(arg);
  os::clean_up(*left);
  delete left;
  return nullptr;
}

}  // namespace

bool available() {
  return true;
}

void init(int argc, char **argv) {
  {
    Lock l;
    load();
    if(g_saved.device.size() != 32) {
      g_saved.device = new_device_id();
      save();
    }
  }
  os::clean_up("");
  for(int i = 1; i + 1 < argc; i++) {
    if(!strcmp(argv[i], "--updated-from")) {
      pthread_t t;
      std::string *left = new std::string(argv[i + 1]);
      if(!pthread_create(&t, nullptr, run_clean_up, left)) pthread_detach(t);
      else delete left;
    }
  }
}

void check(bool manual) {
  if(test_build()) return;
  {
    Lock l;
    if(!manual && !g_saved.check) return;
    if(g_state != IDLE && g_state != UP_TO_DATE && g_state != CHECK_FAILED) return;
    g_state = CHECKING;
    g_manual = manual;
  }
  if(!start(run_check, manual ? (void *)1 : nullptr)) set_state(manual ? CHECK_FAILED : IDLE);
}

State poll() {
  Lock l;
  return g_state;
}

bool manual() {
  Lock l;
  return g_manual;
}

Offer offer() {
  Lock l;
  return g_offer;
}

void progress(long long *done, long long *total) {
  *done = g_received;
  Lock l;
  *total = g_offer.size;
}

std::string error() {
  Lock l;
  return g_error;
}

void install() {
  {
    Lock l;
    if(g_state != AVAILABLE && g_state != FAILED) return;
    g_state = DOWNLOADING;
  }
  g_cancel = false;
  if(!start(run_install, nullptr)) fail(N_("The download failed. Check the connection and try again."));
}

void not_now() {
  Lock l;
  g_saved.snooze_until = (double)time(nullptr) + SNOOZE_S;
  g_state = IDLE;
  save();
}

void skip_version() {
  Lock l;
  g_saved.skip = g_offer.version;
  g_state = IDLE;
  save();
}

void cancel() {
  g_cancel = true;
}

void dismiss() {
  Lock l;
  if(g_state != CHECKING && g_state != DOWNLOADING && g_state != INSTALLING && g_state != RESTARTING) g_state = IDLE;
}

bool enabled() {
  Lock l;
  return g_saved.check;
}

void set_enabled(bool on) {
  Lock l;
  g_saved.check = on;
  save();
}

void stop() {
  /* A download is stopped; a new version already being put in place is let
     finish, so this copy is never left half replaced. */
  const State s = poll();
  if(s == CHECKING) {
    /* No waiting for a slow network on the way out (g_saved's comment). */
    if(g_worker_started) pthread_detach(g_worker);
    g_worker_started = false;
    return;
  }
  if(s == DOWNLOADING) g_cancel = true;
  join();
}

const char *restart_note() {
  return os::restart_note();
}

void finish() {
  std::string launch;
  {
    Lock l;
    if(g_state != RESTARTING) return;
    launch = g_launch;
  }
  if(!os::relaunch(launch)) startup_trace("Update: could not start %s", launch.c_str());
}

#else  // no updater on this platform

bool available() { return false; }
void init(int, char **) {}
void check(bool) {}
State poll() { return IDLE; }
bool manual() { return false; }
Offer offer() { return Offer(); }
void progress(long long *done, long long *total) { *done = *total = 0; }
std::string error() { return ""; }
void install() {}
void not_now() {}
void skip_version() {}
void cancel() {}
void dismiss() {}
bool enabled() { return false; }
const char *restart_note() { return ""; }
void set_enabled(bool) {}
void stop() {}
void finish() {}

#endif

namespace {
bool lower_has(const char *s, const char *what) {
  std::string t = s;
  for(char &c : t) c = (char)tolower((unsigned char)c);
  return t.find(what) != std::string::npos;
}
}  // namespace

bool test_build() {
  return lower_has(VERSION_TAG, "-dev") || lower_has(VERSION_TAG, "-beta") || lower_has(VERSION_TAG, "-alpha");
}

}  // namespace update
