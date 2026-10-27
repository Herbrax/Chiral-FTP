// Controller-first FileZilla-style UI: the SD card on the left, the server on
// the right, the transfer queue on Minus, menus on Plus.

#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <pthread.h>

#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "i18n.h"
#include "json.h"

extern "C" {
#include "config.h"
#include "ftp.h"
#include "fsjobs.h"
#include "ftpd.h"
#include "localfs.h"
#include "log.h"
#include "queue.h"
#include "remote_ops.h"
#include "session.h"
#include "settings.h"
#include "system.h"
#include "strbuf.h"
#include "websrv.h"
#include "discovery.h"
#if CHIRAL_HAS_SFTPD
#include "sftpd.h"
#endif
}

#if CHIRAL_HAS_SFTPD
#define SFTPD_PORT SFTPD_DEFAULT_PORT
#else
#define SFTPD_PORT 2222  // kept in the settings, unused without an SFTP server
#endif

namespace ui {
namespace {

/* The space the layout fills: 1920x1080 on a console, the window on a
   computer (platform::view_width), set at the start of every frame. */
float W = 1920, H = 1080;
const float TOP_H = 96, HINT_H = 76, STATUS_H = 46;
const float PANE_Y = TOP_H + 12, PANE_GAP = 20, MARGIN = 36;
const float ROW_H = 54, PANE_HEAD = 84, PANE_FOOT = 46;
const float FS = 28, FS_SMALL = 23, FS_TITLE = 34;
/* A step up from FS_SMALL for prose meant to be read rather than glanced
   at -- the About screen's body, the start screen's card descriptions and
   the note under its heading. On a TV across a room, FS_SMALL is small. */
const float FS_READ = 27, FS_CARD = 32;

/* A dialog's width. On a phone the window can be narrower than the dialog
   was drawn for (a folded phone is about 600 units across), so it shrinks
   to fit there; everywhere else it is the width it was given. */
inline float dialog_w(float w) {
#if CHIRAL_UI_MOBILE
  return std::min(w, W - 24);
#else
  return w;
#endif
}

#if CHIRAL_UI_DESKTOP
/* The computer interface's sidebar and pages, or the console layout. A
   phone or tablet has the former, arranged by the mobile shell; a TV, from
   the same build, the latter: its window isn't the layout. */
inline bool desktop_ui() {
#if CHIRAL_UI_MOBILE
  return platform::windowed();
#else
  return true;
#endif
}
#endif

/* Text that names the machine. A console's says "the console", and a
   computer's (CHIRAL_DEVICE_COMPUTER) says "this computer" -- and leaves out
   what only a console has, the SD card or the TV. Both are marked N_() at
   the call, so each is in the catalogue; translate the result with tr(). */
#if CHIRAL_UI_MOBILE
/* Android, one build for both: a phone, or a TV with the console's layout.
   Each console sentence has its phone and TV versions here, found by the
   console one; nullptr where a phone never shows it (the TV's is used). */
struct DeviceWords {
  const char *console, *phone, *tv;
};
const DeviceWords kDeviceWords[] = {
    {"Could not save to the console", N_("Could not save to this phone"), N_("Could not save to the TV")},
    {"Start in a console folder", N_("Start in a folder on this phone"), N_("Start in a folder on the TV")},
    {"Automatic follows the console's system language.", N_("Automatic follows this phone's system language."),
     N_("Automatic follows the TV's system language.")},
    {"Left/Right to change. Settings are saved on the console right away.", nullptr,
     N_("Left/Right to change. Settings are saved on the TV right away.")},
    {"The ultimate file manager and FTP interface for this console:", nullptr,
     N_("The ultimate file manager and FTP interface for this TV:")},
    {"  FTP server: other devices connect to the console with any FTP app.", nullptr,
     N_("  FTP server: other devices connect to the TV with any SFTP or FTP app.")},
    {"  Web app: manage the console's files from a browser, upload and download.", nullptr,
     N_("  Web app: manage the TV's files from a browser, upload and download.")},
    {"Open a console folder on the left first", N_("Open a local folder on the left first"), N_("Open a local folder on the left first")},
    {"Permanently delete {0} from the console? Folders are deleted with everything inside.",
     N_("Permanently delete {0} from this phone? Folders are deleted with everything inside."),
     N_("Permanently delete {0} from the TV? Folders are deleted with everything inside.")},
    {"Download to the console", N_("Download to this phone"), N_("Download to the TV")},
    {"Other devices connect to the console with FileZilla, a file manager or any FTP app.",
     N_("Other devices connect to this phone with FileZilla, Cyberduck, a file manager or any SFTP or FTP app."),
     N_("Other devices connect to the TV with FileZilla, Cyberduck, a file manager or any SFTP or FTP app.")},
    {"Console files: browse, download to your PC, upload from it (drag and drop works), copy, move, rename, delete.", nullptr,
     N_("TV files: browse, download to your PC, upload from it (drag and drop works), copy, move, rename, delete.")},
    {"This console shows up on the others as \"{0}\".", N_("This device shows up on the others as \"{0}\"."),
     N_("This device shows up on the others as \"{0}\".")},
    {"Turn the console into an FTP server. Connect to it from your PC, phone or another console.", nullptr,
     N_("Turn the TV into an SFTP or FTP server. Connect to it from your PC, phone or a console.")},
    {"Open chiral-ftp in a browser on your PC: manage the console's files, upload and download.", nullptr,
     N_("Open chiral-ftp in a browser on your PC: manage the TV's files, upload and download.")},
    {"Copy and move files on the console: the SD card, the homebrew folder, anywhere.", nullptr,
     N_("Copy and move files on the TV: its storage, USB drives, anywhere.")},
    {"Connect from the console to your laptop, PC or NAS and transfer files both ways.", nullptr,
     N_("Connect from the TV to your laptop, PC or NAS and transfer files both ways.")},
};
#endif

inline const char *device_text(const char *console, const char *computer) {
#if CHIRAL_DEVICE_COMPUTER
  (void)console;
  return computer;
#elif CHIRAL_UI_MOBILE
  (void)computer;
  for(const DeviceWords &d : kDeviceWords) {
    if(!strcmp(d.console, console)) return platform::windowed() && d.phone ? d.phone : d.tv;
  }
  return console;
#else
  (void)computer;
  return console;
#endif
}

/* ---------- small helpers ---------- */

std::string fmt_size(long long n) {
  char buf[32];
  if(n < 0) return "";
  if(n < 1024) {
    snprintf(buf, sizeof(buf), "%lld B", n);
    return buf;
  }
  const char *units[] = {"KB", "MB", "GB", "TB"};
  double v = (double)n;
  int i = -1;
  do {
    v /= 1024;
    i++;
  } while(v >= 1024 && i < 3);
  snprintf(buf, sizeof(buf), v >= 100 ? "%.0f %s" : v >= 10 ? "%.1f %s" : "%.2f %s", v, units[i]);
  return buf;
}

std::string fmt_date(long long t) {
  char buf[32];
  time_t tt = (time_t)t;
  struct tm tm;
  if(t < 0) return "";
  localtime_r(&tt, &tm);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
  return buf;
}

std::string join(const std::string &dir, const std::string &name) {
  if(dir.empty() || dir[dir.size() - 1] == '/') return dir + name;
  return dir + "/" + name;
}

std::string parent_of(const std::string &path) {
  if(path.empty() || path == "/") return "/";
  std::string p = path;
  while(p.size() > 1 && p[p.size() - 1] == '/') p.pop_back();
  size_t i = p.rfind('/');
  return i == 0 || i == std::string::npos ? "/" : p.substr(0, i);
}

std::string base_of(const std::string &path) {
  size_t i = path.rfind('/');
  return i == std::string::npos ? path : path.substr(i + 1);
}

double now() {
  return ImGui::GetTime();
}

/* ---------- text wrapping, for every language ---------- */

// The code point at s[i], and its length in bytes.
unsigned utf8_at(const std::string &s, size_t i, size_t *len) {
  unsigned char c = (unsigned char)s[i];
  int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
  if(i + n > s.size()) n = 1;
  unsigned cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
  for(int k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
  *len = (size_t)n;
  return cp;
}

// Japanese, Chinese and Korean text may break between any two characters.
bool breaks_anywhere(unsigned cp) {
  return (cp >= 0x3000 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0xFF00 && cp <= 0xFFEF);
}

bool right_to_left(unsigned cp) {
  return (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF);
}

/* Lines no wider than max_w. Words break at spaces, and Japanese, Chinese and
   Korean anywhere. Arabic arrives in drawing order (tools/gen-i18n.py), its
   sentence starting at the right: lines are filled from the right end so the
   first one shown is the start of the sentence. */
std::vector<std::string> wrap_text(const std::string &text, float size, float max_w) {
  struct Token { std::string text; bool space_before; };
  std::vector<Token> tokens;
  bool rtl = false, space = false;
  for(size_t i = 0; i < text.size();) {
    size_t len;
    unsigned cp = utf8_at(text, i, &len);
    rtl |= right_to_left(cp);
    if(cp == ' ') {
      space = true;
    } else if(breaks_anywhere(cp) || tokens.empty() || space || breaks_anywhere(utf8_at(tokens.back().text, 0, &len))) {
      utf8_at(text, i, &len);
      tokens.push_back({text.substr(i, len), space});
      space = false;
    } else {
      utf8_at(text, i, &len);
      tokens.back().text += text.substr(i, len);
    }
    i += len;
  }
  std::vector<std::string> lines;
  std::string line;
  auto fits = [&](const std::string &t) { return draw::text_size(size, t.c_str()).x <= max_w; };
  if(!rtl) {
    for(size_t k = 0; k < tokens.size(); k++) {
      std::string trial = line.empty() ? tokens[k].text : line + (tokens[k].space_before ? " " : "") + tokens[k].text;
      if(!line.empty() && !fits(trial)) {
        lines.push_back(line);
        line = tokens[k].text;
      } else {
        line = trial;
      }
    }
  } else {
    for(size_t k = tokens.size(); k-- > 0;) {
      bool gap = k + 1 < tokens.size() && tokens[k + 1].space_before;
      std::string trial = line.empty() ? tokens[k].text : tokens[k].text + (gap ? " " : "") + line;
      if(!line.empty() && !fits(trial)) {
        lines.push_back(line);
        line = tokens[k].text;
      } else {
        line = trial;
      }
    }
  }
  if(!line.empty()) lines.push_back(line);
  return lines;
}

/* ---------- background jobs ---------- */

// One worker thread for blocking engine calls (connect, list, delete...), so
// the UI keeps drawing; completions run back on the UI thread.
class Jobs {
 public:
  /* Raw pthreads, not std::thread / std::mutex / std::condition_variable.
     This was the only place in the app using the C++ threading classes,
     and on the console it was what killed startup: a build with just the
     FTP server -- which never starts this worker -- ran fine, while the
     file manager and client builds, which do, died inside ui::init().
     The engine has used pthread_create throughout from the beginning and
     works on the same hardware, so this now does too. */
  Jobs() {
    pthread_mutex_init(&mutex_, nullptr);
    pthread_cond_init(&cv_, nullptr);
  }

  // False when the worker couldn't start; callers carry on without it.
  bool start() {
    if(started_) return true;
    /* Joinable, not detached: stop() waits for it. A detached worker would
       still be looking at todo_ and done_ while the static destructors take
       them apart on the way out of main. */
    if(pthread_create(&thread_, nullptr, &trampoline, this) != 0) return false;
    started_ = true;
    return true;
  }

  // Ends the worker and waits for it. Anything still queued is dropped.
  void stop() {
    if(!started_) return;
    pthread_mutex_lock(&mutex_);
    quit_ = true;
    todo_.clear();
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mutex_);
    pthread_join(thread_, nullptr);
    started_ = false;
  }

  void run(std::function<void()> work, std::function<void()> then) {
    pthread_mutex_lock(&mutex_);
    todo_.push_back({work, then});
    pending_++;
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mutex_);
  }

  void poll() {
    std::deque<std::function<void()>> ready;
    pthread_mutex_lock(&mutex_);
    ready.swap(done_);
    pthread_mutex_unlock(&mutex_);
    for(auto &f : ready) f();
  }

  int pending() {
    pthread_mutex_lock(&mutex_);
    int n = pending_;
    pthread_mutex_unlock(&mutex_);
    return n;
  }

 private:
  struct Job {
    std::function<void()> work, then;
  };

  static void *trampoline(void *self) {
    static_cast<Jobs *>(self)->loop();
    return nullptr;
  }

  void loop() {
    for(;;) {
      Job job;
      pthread_mutex_lock(&mutex_);
      while(todo_.empty() && !quit_) pthread_cond_wait(&cv_, &mutex_);
      if(quit_) {
        pthread_mutex_unlock(&mutex_);
        return;
      }
      job = todo_.front();
      todo_.pop_front();
      pthread_mutex_unlock(&mutex_);

      job.work();

      pthread_mutex_lock(&mutex_);
      done_.push_back([this, then = job.then] {
        pthread_mutex_lock(&mutex_);
        pending_--;
        pthread_mutex_unlock(&mutex_);
        if(then) then();
      });
      pthread_mutex_unlock(&mutex_);
    }
  }

  pthread_t thread_{};
  bool started_ = false;
  bool quit_ = false;
  pthread_mutex_t mutex_;
  pthread_cond_t cv_;
  std::deque<Job> todo_;
  std::deque<std::function<void()>> done_;
  int pending_ = 0;
};

Jobs g_jobs;

/* ---------- app state ---------- */

struct Entry {
  std::string name;
  bool dir = false, link = false, drive = false;
  long long size = -1, mtime = -1;
  std::string perm, owner, detail;
};

struct Pane {
  bool remote = false;
  /* Free space on the drive this folder is on, -1 when the platform won't
     say (system_free_space). Shown in the footer only when it is known. */
  long long free_bytes = -1;
  double free_asked = 0;  // when refresh_free_space() last asked (now())
  std::string path;  // "" = drive list (local) / server list (remote, disconnected)
  std::vector<Entry> entries;
  int cursor = 0, scroll = 0;
  std::set<std::string> marked;
  int loading = 0;
  std::string error;
};

struct ServerItem {
  enum Kind { NEW, SITE, RECENT } kind;
  int index;
};

enum OverlayKind { OV_OTHER, OV_QUEUE, OV_LOG, OV_TEXT, OV_MENU, OV_QUICK, OV_PROMPT };

/* Touch. The UI is drawn fresh every frame, so rather than keep a parallel
   model of where everything is, each screen records the rectangles it has
   just drawn and a tap is matched against those. The list is rebuilt during
   draw and read on the next touch, which is a frame behind -- invisible at
   a finger's speed, and it keeps the drawing code as the one description of
   the layout.

   Taps are deliberately not the same as presses. On a list, the first tap
   moves the cursor and the second opens what it is on, so a mis-tap in a
   file manager costs nothing; menus and cards act on the first tap, since
   there the choice is the whole point.

   A mouse on a computer uses the same rectangles, and more of them: it is
   always somewhere, so what it is over takes the highlight, and a click then
   acts on it straight away (handle_pointer). */
enum HitKind {
  HIT_ROW,            // a file or folder in a pane
  HIT_SERVER,         // a saved site or recent connection
  HIT_SETTING,        // a row on the FTP server / web app screens
  HIT_CARD,           // a mode on the start screen
  HIT_MENU_ITEM,      // a menu's item, or any overlay's: a is which, b a sub-kind
  HIT_OVERLAY_BODY,
  HIT_DISCONNECT,
  HIT_SCROLL,
  HIT_HINT,           // a key in a hint bar: a is its Action
  HIT_QUICK,          // the start screen's Quick connect button
};

struct Hit {
  float x, y, w, h;
  HitKind kind;
  int a, b;  // row: pane index, entry index. card/menu: which one.
  int layer;  // 0 the screen, n the n-th overlay up
};

std::vector<Hit> g_hits;
int g_hit_layer = 0;         // which layer is being drawn
bool g_touch_dirty = false;  // keeps the main loop redrawing while in use

void hit_add(HitKind kind, float x, float y, float w, float h, int a = 0, int b = 0) {
  if(!platform::has_touch() && !platform::has_pointer()) return;
  g_hits.push_back({x, y, w, h, kind, a, b, g_hit_layer});
}

// layer -1: any layer.
const Hit *hit_at(float x, float y, HitKind kind, int layer = -1) {
  // Last drawn wins, so something drawn on top takes the tap.
  for(size_t i = g_hits.size(); i-- > 0;) {
    const Hit &h = g_hits[i];
    if(h.kind == kind && (layer < 0 || h.layer == layer) && x >= h.x && x < h.x + h.w && y >= h.y &&
       y < h.y + h.h) {
      return &g_hits[i];
    }
  }
  return nullptr;
}

// A hint whose key can be clicked. The hint bars are drawn with these.
float hint(float x, float y, Action a, const char *caption) {
  const float w = draw::hint(x, y, a, caption);
  hit_add(HIT_HINT, x - 6, y - 8, w - 28, 54, a);
  return w;
}

// "Switch side": clicking it switches, as Tab does.
float hint_lr(float x, float y, const char *caption) {
  const float w = draw::hint_lr(x, y, caption);
  hit_add(HIT_HINT, x - 6, y - 8, w - 28, 54, ACT_R1);
  return w;
}

struct Overlay {
  virtual ~Overlay() {}
  virtual OverlayKind kind() { return OV_OTHER; }
  virtual bool input(Action a) = 0;  // false closes the overlay
  virtual void draw() = 0;
  /* The mouse is over item a (b: which part of it, where an item has
     several) -- what each overlay's HIT_MENU_ITEM rectangles mean. Moves
     the overlay's own highlight there; a click then sends ACT_CROSS. */
  virtual void focus(int a, int b) {
    (void)a;
    (void)b;
  }
  /* A click on item a: the highlight moves there and it is chosen, as with
     Enter. False when that closed the overlay. A right click does nothing
     unless an overlay gives it a meaning. */
  virtual bool click(int a, int b, bool right) {
    if(right) return true;
    focus(a, b);
    return input(ACT_CROSS);
  }
};

enum Mode { MODE_HOME, MODE_CLIENT, MODE_FILES, MODE_FTPD, MODE_WEB };

/* One-feature builds. -DCHIRAL_ONLY_MODE=<n> leaves the other three modes
   on the start screen, greyed out and unselectable, and skips their startup
   work entirely -- the point being to isolate a fault on hardware, so it is
   not enough to merely hide them. Undefined (0) is the whole app. */
#define CHIRAL_ONLY_FTPD   1
#define CHIRAL_ONLY_WEB    2
#define CHIRAL_ONLY_FILES  3
#define CHIRAL_ONLY_CLIENT 4

#ifndef CHIRAL_ONLY_MODE
#define CHIRAL_ONLY_MODE 0
#endif

inline bool mode_enabled(Mode m) {
#if CHIRAL_ONLY_MODE == CHIRAL_ONLY_FTPD
  return m == MODE_HOME || m == MODE_FTPD;
#elif CHIRAL_ONLY_MODE == CHIRAL_ONLY_WEB
  return m == MODE_HOME || m == MODE_WEB;
#elif CHIRAL_ONLY_MODE == CHIRAL_ONLY_FILES
  return m == MODE_HOME || m == MODE_FILES;
#elif CHIRAL_ONLY_MODE == CHIRAL_ONLY_CLIENT
  return m == MODE_HOME || m == MODE_CLIENT;
#else
  (void)m;
  return true;
#endif
}

/* The two panes and the background worker belong to the file manager and
   the client; an FTP-server-only or web-only build never touches them. */
inline bool panes_enabled() {
  return mode_enabled(MODE_FILES) || mode_enabled(MODE_CLIENT);
}

struct App {
  Mode mode = MODE_HOME;
  Pane panes[2];
  Pane fm[2];  // the file manager: two panes on the console's own storage
  /* Disconnect sits in the server pane's header, above the list. Pressing
     Up from the first row moves onto it, Down comes back -- so it is
     reachable with the d-pad, and not only by tapping it. */
  bool disconnect_focus = false;
  int fm_active = 0;
  unsigned long fm_finished = 0;
  int active = 0;
  bool connected = false;
  ftp_server_t server;
  std::vector<json::Object> sites, history;
  bool show_hidden = true, save_pass = true;
  /* The network whose address the server screens show, by interface name
     ("en0"); empty for automatic. Only a computer offers the choice
     (system_networks). */
  std::string network;
  /* What the client calls the server it is connected to: the device's
     name after a quick connect, else "" (and it says "Server"). */
  std::string server_label;
  std::vector<ServerItem> servers;
  std::vector<std::unique_ptr<Overlay>> overlays;
  std::string toast;
  bool toast_err = false;
  double toast_until = 0;
  std::string status;
  bool status_err = false;
  unsigned long log_seq = 0;
  std::deque<std::pair<int, std::string>> log;  // kind, text
  queue_stats_t qs;
  int last_done = -1;
  double refresh_at = 0;
  bool quit = false;
};

App g;

/* On the start screen: built in (mode_enabled), and never on a computer for
   the file manager and the client -- there they live in the web app, in its
   own browser. */
bool mode_shown(Mode m) {
#if CHIRAL_DEVICE_COMPUTER
  if(m == MODE_FILES || m == MODE_CLIENT) return false;
#endif
  return mode_enabled(m);
}

void show_toast(const std::string &msg, bool err = false) {
  g.toast = msg;
  g.toast_err = err;
  g.toast_until = now() + (err ? 6 : 3);
}

void push(Overlay *o) {
  g.overlays.emplace_back(o);
}

Pane &local() { return g.panes[0]; }
Pane &remote() { return g.panes[1]; }
Pane &active() { return g.panes[g.active]; }

/* ---------- persistence (shared with the web UI) ---------- */

std::vector<json::Object> load_store(const char *name) {
  strbuf_t b = {0, 0, 0};
  std::vector<json::Object> out;
  if(!store_read(name, &b) && b.data) json::parse_array(std::string(b.data, b.len), out);
  strbuf_free(&b);
  return out;
}

void save_store(const char *name, const std::vector<json::Object> &items) {
  std::string s = json::dump_array(items);
  if(store_write(name, s.c_str(), s.size())) show_toast(tr(device_text(N_("Could not save to the console"), N_("Could not save to this computer"))), true);
}

#if CHIRAL_UI_MOBILE
/* The order of a phone's lists (a pane's ⋮, Sort by), the same in every
   pane and remembered. Folders stay first whatever it is. */
enum SortBy { SORT_NAME, SORT_DATE, SORT_SIZE, SORT_TYPE };
int g_sort_by = SORT_NAME;
bool g_sort_desc = false;
#endif

void load_all() {
  g.sites = load_store("sites");
  g.history = load_store("history");
  std::vector<json::Object> prefs = load_store("prefs");
  if(!prefs.empty()) {
    g.show_hidden = json::get_bool(prefs[0], "show_hidden", true);
    g.save_pass = json::get_bool(prefs[0], "save_pass", true);
    g.network = json::get_str(prefs[0], "network");
    platform::set_zoom((float)json::get_num(prefs[0], "zoom", 1.0));
#if CHIRAL_UI_MOBILE
    g_sort_by = std::max(0, std::min((int)SORT_TYPE, (int)json::get_num(prefs[0], "sort_by", SORT_NAME)));
    g_sort_desc = json::get_bool(prefs[0], "sort_desc", false);
#endif
  }
}

void save_prefs() {
  json::Object o;
  json::set_bool(o, "show_hidden", g.show_hidden);
  json::set_bool(o, "save_pass", g.save_pass);
  json::set_str(o, "language", i18n_choice());
  json::set_str(o, "network", g.network);
  json::set_num(o, "zoom", platform::zoom());
#if CHIRAL_UI_MOBILE
  json::set_num(o, "sort_by", g_sort_by);
  json::set_bool(o, "sort_desc", g_sort_desc);
#endif
  save_store("prefs", {o});
}

int default_port(const std::string &proto) {
  return proto == "sftp" ? 22 : 21;
}

std::string site_label(const json::Object &s) {
  std::string proto = json::get_str(s, "protocol", "ftp");
  std::string user = json::get_bool(s, "anonymous") ? "" : json::get_str(s, "user");
  int port = (int)json::get_num(s, "port", default_port(proto));
  char buf[400];
  snprintf(buf, sizeof(buf), "%s://%s%s%s:%d", proto.c_str(), user.c_str(), user.empty() ? "" : "@",
           json::get_str(s, "host").c_str(), port);
  return buf;
}

std::string site_key(const json::Object &s) {
  return site_label(s);
}

void rebuild_servers() {
  g.servers.clear();
  g.servers.push_back({ServerItem::NEW, 0});
  for(int i = 0; i < (int)g.sites.size(); i++) g.servers.push_back({ServerItem::SITE, i});
  for(int i = 0; i < (int)g.history.size(); i++) g.servers.push_back({ServerItem::RECENT, i});
  Pane &p = remote();
  if(!g.connected) {
    if(p.cursor >= (int)g.servers.size()) p.cursor = (int)g.servers.size() - 1;
    if(p.cursor < 0) p.cursor = 0;
  }
}

void add_history(json::Object site) {
  json::Object h;
  const char *keys[] = {"protocol", "host", "user", "keyfile", "remote_dir", "local_dir", "name"};
  for(const char *k : keys) json::set_str(h, k, json::get_str(site, k));
  json::set_num(h, "port", json::get_num(site, "port", default_port(json::get_str(site, "protocol", "ftp"))));
  json::set_num(h, "pasv_mode", json::get_num(site, "pasv_mode", 0));
  json::set_bool(h, "anonymous", json::get_bool(site, "anonymous"));
  json::set_str(h, "pass", g.save_pass && !json::get_bool(site, "anonymous") ? json::get_str(site, "pass") : "");
  json::set_num(h, "time", (double)time(nullptr));
  std::string key = site_key(h);
  std::vector<json::Object> out = {h};
  for(auto &o : g.history) {
    if(site_key(o) != key && out.size() < MAX_HISTORY) out.push_back(o);
  }
  g.history = out;
  save_store("history", g.history);
  rebuild_servers();
}

/* ---------- pane loading ---------- */

#if CHIRAL_UI_MOBILE
// A file's type, for Sort by type: what follows its last dot ("" for none, or a dot file).
const char *name_ext(const std::string &name) {
  const size_t dot = name.rfind('.');
  return dot == std::string::npos || dot == 0 ? "" : name.c_str() + dot + 1;
}

void sort_entries(std::vector<Entry> &v) {
  std::stable_sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) {
    if(a.dir != b.dir) return a.dir;
    int c = 0;
    switch(g_sort_by) {
    case SORT_DATE: c = a.mtime < b.mtime ? -1 : a.mtime > b.mtime; break;
    case SORT_SIZE: if(!a.dir) c = a.size < b.size ? -1 : a.size > b.size; break;  // folders by name
    case SORT_TYPE: c = strcasecmp(name_ext(a.name), name_ext(b.name)); break;
    default: break;
    }
    if(g_sort_desc) c = -c;
    if(c) return c < 0;
    // Then by name: A to Z, unless it's the name order itself that's reversed.
    const int n = strcasecmp(a.name.c_str(), b.name.c_str());
    return g_sort_by == SORT_NAME && g_sort_desc ? n > 0 : n < 0;
  });
}
#else
void sort_entries(std::vector<Entry> &v) {
  std::sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) {
    if(a.dir != b.dir) return a.dir;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
}
#endif

void clamp_cursor(Pane &p, int count) {
  if(p.cursor >= count) p.cursor = count - 1;
  if(p.cursor < 0) p.cursor = 0;
}

void set_entries(Pane &p, const std::string &path, std::vector<Entry> entries, bool keep) {
  std::string old_name = keep && p.cursor < (int)p.entries.size() ? p.entries[p.cursor].name : "";
  bool same = path == p.path;
  sort_entries(entries);
  p.path = path;
  p.entries = std::move(entries);
  p.error.clear();
  if(!same) {
    p.cursor = 0;
    p.scroll = 0;
    p.marked.clear();
  } else {
    std::set<std::string> names;
    for(auto &e : p.entries) names.insert(e.name);
    for(auto it = p.marked.begin(); it != p.marked.end();) {
      it = names.count(*it) ? std::next(it) : p.marked.erase(it);
    }
    for(int i = 0; i < (int)p.entries.size(); i++) {
      if(p.entries[i].name == old_name) p.cursor = i;
    }
  }
  clamp_cursor(p, (int)p.entries.size());
}

void load_drives(Pane &p = local()) {
  localfs_root_t roots[16];
  int n = localfs_roots(roots, 16);
  std::vector<Entry> v;
  for(int i = 0; i < n; i++) {
    Entry e;
    e.name = roots[i].path;
    e.detail = roots[i].label;
    e.dir = e.drive = true;
    e.size = roots[i].free_bytes;
    v.push_back(e);
  }
  p.path = "";
  p.entries = v;  // drive order is meaningful; don't sort
  p.marked.clear();
  clamp_cursor(p, (int)v.size());
}

/* Free space that wasn't known yet when the list was made. Some platforms
   answer on a thread of their own (the Switch: a USB drive took 350 ms), so
   the first answer is "unknown" and the real one comes a moment later; ask
   again, about once a second, until it does -- the answer is kept, so asking
   costs nothing. Without this a drive's free space only appeared once the
   list was made again. */
void refresh_free_space(Pane &p) {
  if(p.remote || now() - p.free_asked < 1) return;
  p.free_asked = now();
  if(p.path.empty()) {
    for(Entry &e : p.entries) {
      if(e.drive && e.size < 0) e.size = system_free_space(e.name.c_str(), nullptr);
    }
  } else if(p.free_bytes < 0) {
    p.free_bytes = system_free_space(p.path.c_str(), nullptr);
  }
}

// Lists a local folder into a pane: the client's left side by default, or a
// file manager pane.
void load_local(const std::string &path, bool keep = false, Pane *target = nullptr) {
  Pane &lp = target ? *target : local();
  lp.free_bytes = system_free_space(path.c_str(), nullptr);
  Pane &p = target ? *target : local();
  ftp_listing_t l;
  if(path.empty()) {
    load_drives(p);
    return;
  }
  if(localfs_list(path.c_str(), &l)) {
    show_toast(trf("Could not open {0}: {1}", {path, strerror(errno)}), true);
    return;
  }
  std::vector<Entry> v;
  for(size_t i = 0; i < l.count; i++) {
    Entry e;
    e.name = l.entries[i].name;
    e.dir = l.entries[i].is_dir;
    e.link = l.entries[i].is_link;
    e.size = l.entries[i].size;
    e.mtime = l.entries[i].mtime;
    e.perm = l.entries[i].perm;
    v.push_back(e);
  }
  ftp_listing_free(&l);
  set_entries(p, path, v, keep);
}

struct ListCtx {
  std::string path;
  ftp_listing_t listing;
  char resolved[1024];
};

int do_list(ftp_conn_t *c, void *p) {
  ListCtx *ctx = (ListCtx *)p;
  ftp_listing_free(&ctx->listing);
  return ftp_list(c, ctx->path.c_str(), &ctx->listing, ctx->resolved, sizeof(ctx->resolved));
}

double g_remote_load_at = 0;  // when the listing still in flight was asked for

void load_remote(const std::string &path, bool keep = false) {
  /* One listing at a time. Pressing Open or Back again while the server
     hasn't answered would only send it another request behind the first;
     after five seconds without an answer, a new one may go. */
  if(remote().loading && now() - g_remote_load_at < 5) return;
  g_remote_load_at = now();
  auto ctx = std::make_shared<ListCtx>();
  auto err = std::make_shared<std::string>();
  auto ok = std::make_shared<bool>(false);
  ctx->path = path;
  memset(&ctx->listing, 0, sizeof(ctx->listing));
  remote().loading++;
  g_jobs.run(
      [ctx, err, ok] {
        char e[512] = "";
        *ok = !session_run(do_list, ctx.get(), e, sizeof(e));
        *err = e;
      },
      [ctx, err, ok, keep] {
        Pane &p = remote();
        p.loading--;
        if(!*ok) {
          ftp_listing_free(&ctx->listing);
          show_toast(*err, true);
          ftp_server_t s;
          if(!session_server(&s)) {
            g.connected = false;
            p.path.clear();
            rebuild_servers();
          }
          return;
        }
        std::vector<Entry> v;
        for(size_t i = 0; i < ctx->listing.count; i++) {
          ftp_entry_t &f = ctx->listing.entries[i];
          Entry e;
          e.name = f.name;
          e.dir = f.is_dir;
          e.link = f.is_link;
          e.size = f.size;
          e.mtime = f.mtime;
          e.perm = f.perm;
          e.owner = f.owner;
          v.push_back(e);
        }
        ftp_listing_free(&ctx->listing);
        set_entries(p, ctx->resolved, v, keep);
      });
}

void refresh(Pane &p) {
  if(!p.remote) load_local(p.path, true, &p);
  else if(g.connected && !p.loading) load_remote(p.path, true);
}

/* ---------- connecting ---------- */

void server_from_site(const json::Object &s, ftp_server_t &out) {
  memset(&out, 0, sizeof(out));
  std::string proto = json::get_str(s, "protocol", "ftp");
  bool anon = json::get_bool(s, "anonymous");
  out.protocol = proto == "sftp" ? FTP_PROTO_SFTP : FTP_PROTO_FTP;
  snprintf(out.host, sizeof(out.host), "%s", json::get_str(s, "host").c_str());
  out.port = (unsigned short)json::get_num(s, "port", default_port(proto));
  snprintf(out.user, sizeof(out.user), "%s", anon ? "" : json::get_str(s, "user").c_str());
  snprintf(out.pass, sizeof(out.pass), "%s", anon ? "" : json::get_str(s, "pass").c_str());
  snprintf(out.keyfile, sizeof(out.keyfile), "%s",
           out.protocol == FTP_PROTO_SFTP ? json::get_str(s, "keyfile").c_str() : "");
  out.pasv_mode = (int)json::get_num(s, "pasv_mode", 0);
  out.show_hidden = g.show_hidden;
}

void connect(json::Object site) {
  if(json::get_str(site, "host").empty()) {
    show_toast(tr("Enter a host first"), true);
    return;
  }
  auto srv = std::make_shared<ftp_server_t>();
  auto home = std::make_shared<std::string>();
  auto err = std::make_shared<std::string>();
  auto ok = std::make_shared<bool>(false);
  server_from_site(site, *srv);
  remote().loading++;
  show_toast(trf("Connecting to {0}...", {json::get_str(site, "host")}));
  g_jobs.run(
      [srv, home, err, ok] {
        char h[1024] = "", e[512] = "";
        *ok = !session_connect(srv.get(), h, sizeof(h), e, sizeof(e));
        *home = h;
        *err = e;
        if(*ok) session_server(srv.get());  // may have switched FTP -> SFTP
      },
      [srv, home, err, ok, site]() mutable {
        remote().loading--;
        if(!*ok) {
          show_toast(*err, true);
          return;
        }
        g.connected = true;
        g.server = *srv;
        // A quick connect's server is a device with a name: "Nintendo Switch".
        g.server_label = json::get_bool(site, "quick") ? json::get_str(site, "name") : std::string();
        json::set_str(site, "protocol", srv->protocol == FTP_PROTO_SFTP ? "sftp" : "ftp");
        json::set_num(site, "port", srv->port);
        // A quick connect's login lasts only while that server runs: nothing to remember.
        if(!json::get_bool(site, "quick")) add_history(site);
        show_toast(trf("Connected to {0}", {srv->host}));
        std::string dir = json::get_str(site, "remote_dir");
        remote().path = "\x01";  // force a fresh pane state
        load_remote(dir.empty() ? *home : dir);
        std::string ldir = json::get_str(site, "local_dir");
        if(!ldir.empty()) load_local(ldir);
        g.active = 1;
      });
}

// The remote side, as it is when nothing is connected.
void forget_connection() {
  g.connected = false;
  g.server_label.clear();
  remote().path.clear();
  remote().entries.clear();
  remote().marked.clear();
  rebuild_servers();
}

void disconnect() {
  g_jobs.run([] { session_disconnect(); }, [] {
    forget_connection();
    show_toast(tr("Disconnected"));
  });
}

void go_mode(Mode m);  // further down

/* The session gave the connection up: the server hadn't answered for 30
   seconds (session_drops). The client stops saying it is connected, and
   closes -- on a computer, back to the page it was opened from. */
void watch_connection() {
  static unsigned seen = session_drops();
  const unsigned drops = session_drops();
  if(drops == seen) return;
  seen = drops;
  if(!g.connected) return;
  const std::string who = g.server_label.empty() ? std::string(g.server.host) : g.server_label;
  forget_connection();
  if(g.mode == MODE_CLIENT) go_mode(MODE_HOME);
  show_toast(trf("Lost the connection to {0}", {who}), true);
}

/* ---------- text entry ---------- */

struct TextEntry : Overlay {
  std::string title, value;
  TextKind text_kind;
  std::function<void(std::string)> done;

  TextEntry(const std::string &t, const std::string &v, TextKind k, std::function<void(std::string)> d)
      : title(t), value(v), text_kind(k), done(d) {
    platform::set_text_input(true);
  }
  ~TextEntry() { platform::set_text_input(false); }
  OverlayKind kind() override { return OV_TEXT; }

  bool input(Action a) override {
    if(a == ACT_CIRCLE) return false;
    return true;
  }

  bool typing() {
    std::string text;
    bool bs, enter, esc;
    if(!platform::take_typed(text, bs, enter, esc)) return true;
    if(esc) return false;
    if(bs && !value.empty()) {
      value.pop_back();
      while(!value.empty() && ((unsigned char)value.back() & 0xC0) == 0x80) value.pop_back();
    }
    value += text;
    if(enter) {
      auto d = done;
      std::string v = value;
      d(v);
      return false;
    }
    return true;
  }

  void draw() override {
    float w = dialog_w(1100), h = 250, x = (W - w) / 2, y = (H - h) / 2;
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 150));
    draw::rect(x, y, w, h, draw::PANEL, 18);
    hit_add(HIT_OVERLAY_BODY, x, y, w, h);
    draw::text(x + 40, y + 30, FS_TITLE, draw::TEXT, title.c_str(), true);
    draw::rect(x + 40, y + 100, w - 80, 64, draw::BG, 10);
    std::string shown = text_kind == TEXT_PASSWORD ? std::string(value.size(), '*') : value;
    bool blink = fmod(now(), 1.0) < 0.5;
    shown += blink ? "|" : " ";
    draw::text_clip(x + 60, y + 115, FS, draw::TEXT, shown.c_str(), w - 120);
    draw::text(x + 40, y + 190, FS_SMALL, draw::MUTED, tr("Type on the keyboard, Enter to confirm, Esc to cancel"));
  }
};

void prompt(const std::string &title, const std::string &initial, TextKind kind,
            std::function<void(std::string)> done) {
  if(platform::has_native_keyboard()) {
    std::string out;
    if(platform::native_text_prompt(title.c_str(), initial, kind, out)) done(out);
    else if(platform::keyboard_failed()) show_toast(tr("The system keyboard couldn't open"), true);
    return;
  }
  push(new TextEntry(title, initial, kind, done));
}

/* ---------- menus ---------- */

struct MenuItem {
  std::string label;
  std::function<void()> run;
  bool enabled = true;
  bool danger = false;
};

struct Menu : Overlay {
  OverlayKind kind() override { return OV_MENU; }
  std::string title, subtitle;
  std::vector<MenuItem> items;
  int cursor = 0;

  void focus(int a, int b) override {
    (void)b;
    if(a >= 0 && a < (int)items.size() && items[a].enabled) cursor = a;
  }

  bool input(Action a) override {
    int n = (int)items.size();
    if(a == ACT_CIRCLE || a == ACT_OPTIONS) return false;
    if(a == ACT_UP) cursor = (cursor + n - 1) % n;
    if(a == ACT_DOWN) cursor = (cursor + 1) % n;
    if(a == ACT_CROSS && items[cursor].enabled) {
      auto run = items[cursor].run;
      run();
      return false;
    }
    return true;
  }

  void draw() override {
    float row = 62, w = dialog_w(760);
    float h = 120 + row * items.size() + (subtitle.empty() ? 0 : 36);
    float x = (W - w) / 2, y = (H - h) / 2;
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 140));
    draw::rect(x, y, w, h, draw::PANEL, 18);
    hit_add(HIT_OVERLAY_BODY, x, y, w, h);
    draw::text_clip(x + 36, y + 28, FS_TITLE, draw::TEXT, title.c_str(), w - 72, true);
    float yy = y + 84;
    if(!subtitle.empty()) {
      draw::text_clip(x + 36, yy - 8, FS_SMALL, draw::MUTED, subtitle.c_str(), w - 72);
      yy += 36;
    }
    for(int i = 0; i < (int)items.size(); i++) {
      bool on = i == cursor;
      if(items[i].enabled) hit_add(HIT_MENU_ITEM, x + 20, yy, w - 40, row - 6, i);
      if(on) draw::rect(x + 20, yy, w - 40, row - 6, draw::SEL, 10);
      ImU32 col = !items[i].enabled ? draw::MUTED : items[i].danger ? draw::ERR : draw::TEXT;
      draw::text(x + 44, yy + 12, FS, col, items[i].label.c_str(), on);
      yy += row;
    }
  }
};

struct Confirm : Overlay {
  std::string title, text, yes;
  std::function<void()> run;
  int choice = 1;  // default to Cancel

  void focus(int a, int b) override {
    (void)b;
    choice = a ? 1 : 0;
  }

  bool input(Action a) override {
    if(a == ACT_CIRCLE) return false;
    if(a == ACT_LEFT || a == ACT_RIGHT || a == ACT_UP || a == ACT_DOWN) choice = 1 - choice;
    if(a == ACT_CROSS) {
      if(choice == 0) run();
      return false;
    }
    return true;
  }

  void draw() override {
    float w = dialog_w(980), h = 300, x = (W - w) / 2, y = (H - h) / 2;
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 150));
    draw::rect(x, y, w, h, draw::PANEL, 18);
    hit_add(HIT_OVERLAY_BODY, x, y, w, h);
    draw::text(x + 40, y + 30, FS_TITLE, draw::TEXT, title.c_str(), true);
    draw::text_clip(x + 40, y + 96, FS, draw::MUTED, text.c_str(), w - 80);
    const char *labels[2] = {yes.c_str(), tr("Cancel")};
    for(int i = 0; i < 2; i++) {
      float bx = x + w - 40 - (2 - i) * 250 + 10;
      hit_add(HIT_MENU_ITEM, bx, y + 200, 230, 64, i);
      draw::rect(bx, y + 200, 230, 64, i == choice ? (i == 0 ? draw::ERR : draw::ACCENT) : draw::PANEL2, 12);
      ImVec2 ts = draw::text_size(FS, labels[i], true);
      draw::text(bx + (230 - ts.x) / 2, y + 216, FS, draw::TEXT, labels[i], true);
    }
  }
};

void confirm(const std::string &title, const std::string &text, const std::string &yes,
             std::function<void()> run) {
  Confirm *c = new Confirm;
  c->title = title;
  c->text = text;
  c->yes = yes;
  c->run = run;
  push(c);
}

/* ---------- connect / site form ---------- */

struct Form : Overlay {
  enum Field { PROTO, HOST, PORT, USER, PASS, ANON, KEY, NAME, LOCAL_DIR, REMOTE_DIR, CONNECT, SAVE, CANCEL };
  json::Object site;
  int site_index;  // -1: new connection
  int cursor = HOST;

  Form(json::Object s, int index) : site(s), site_index(index) {
    if(json::get_str(site, "protocol").empty()) json::set_str(site, "protocol", "ftp");
  }

  bool sftp() { return json::get_str(site, "protocol") == "sftp"; }

  std::vector<int> fields() {
    std::vector<int> f = {PROTO, HOST, PORT};
    bool anon = json::get_bool(site, "anonymous");
    if(!sftp()) f.push_back(ANON);
    if(!anon || sftp()) {
      f.push_back(USER);
      f.push_back(PASS);
    }
    if(sftp()) f.push_back(KEY);
    f.push_back(NAME);
    f.push_back(LOCAL_DIR);
    f.push_back(REMOTE_DIR);
    f.push_back(CONNECT);
    f.push_back(SAVE);
    f.push_back(CANCEL);
    return f;
  }

  const char *label(int f) {
    switch(f) {
    case PROTO: return tr("Protocol");
    case HOST: return tr("Host");
    case PORT: return tr("Port");
    case USER: return tr("Username");
    case PASS: return sftp() && !json::get_str(site, "keyfile").empty() ? tr("Key passphrase") : tr("Password");
    case ANON: return tr("Anonymous login");
    case KEY: return tr("Private key file");
    case NAME: return tr("Site name");
    case LOCAL_DIR: return tr(device_text(N_("Start in a console folder"), N_("Start in a local folder")));
    case REMOTE_DIR: return tr("Start in server folder");
    default: return "";
    }
  }

  std::string value(int f) {
    switch(f) {
    case PROTO: return sftp() ? tr("SFTP  (SSH, e.g. Mac/Linux Remote Login)") : "FTP";
    case HOST: return json::get_str(site, "host");
    case PORT: {
      int p = (int)json::get_num(site, "port", 0);
      return p ? std::to_string(p) : trf("{0}  (default)", {std::to_string(default_port(json::get_str(site, "protocol")))});
    }
    case USER: return json::get_str(site, "user");
    case PASS: return std::string(json::get_str(site, "pass").size(), '*');
    case ANON: return json::get_bool(site, "anonymous") ? tr("On") : tr("Off");
    case KEY: return json::get_str(site, "keyfile");
    case NAME: return json::get_str(site, "name");
    case LOCAL_DIR: return json::get_str(site, "local_dir");
    case REMOTE_DIR: return json::get_str(site, "remote_dir");
    default: return "";
    }
  }

  void toggle_proto() {
    bool to_sftp = !sftp();
    int port = (int)json::get_num(site, "port", 0);
    json::set_str(site, "protocol", to_sftp ? "sftp" : "ftp");
    if(port == 21 || port == 22) json::set_num(site, "port", to_sftp ? 22 : 21);
    if(to_sftp) json::set_bool(site, "anonymous", false);
  }

  void edit(int f) {
    struct { const char *key; TextKind kind; } map[] = {
      {"", TEXT_DEFAULT}, {"host", TEXT_URL}, {"port", TEXT_NUMBER}, {"user", TEXT_DEFAULT},
      {"pass", TEXT_PASSWORD}, {"", TEXT_DEFAULT}, {"keyfile", TEXT_URL}, {"name", TEXT_DEFAULT},
      {"local_dir", TEXT_URL}, {"remote_dir", TEXT_URL},
    };
    const char *key = map[f].key;
    std::string initial = f == PORT ? (json::get_num(site, "port", 0) ? std::to_string((int)json::get_num(site, "port", 0)) : "")
                                    : json::get_str(site, key);
    Form *self = this;
    prompt(label(f), initial, map[f].kind, [self, f, key](std::string v) {
      if(f == PORT) {
        json::set_num(self->site, "port", atoi(v.c_str()));
      } else {
        if(f == HOST) {
          // Accept a pasted ftp://host:port or sftp://host:port
          if(v.rfind("sftp://", 0) == 0) { json::set_str(self->site, "protocol", "sftp"); v = v.substr(7); }
          else if(v.rfind("ftp://", 0) == 0) { json::set_str(self->site, "protocol", "ftp"); v = v.substr(6); }
          size_t slash = v.find('/');
          if(slash != std::string::npos) v = v.substr(0, slash);
          size_t colon = v.rfind(':');
          if(colon != std::string::npos && v.find(':') == colon) {
            json::set_num(self->site, "port", atoi(v.c_str() + colon + 1));
            v = v.substr(0, colon);
          }
        }
        json::set_str(self->site, key, v);
      }
    });
  }

  void save() {
    if(json::get_str(site, "host").empty()) {
      show_toast(tr("Enter a host first"), true);
      return;
    }
    if(json::get_str(site, "name").empty()) json::set_str(site, "name", json::get_str(site, "host"));
    if(!json::get_num(site, "port", 0)) json::set_num(site, "port", default_port(json::get_str(site, "protocol")));
    if(json::get_str(site, "id").empty()) json::set_str(site, "id", std::to_string((long long)time(nullptr)) + std::to_string(rand() % 1000));
    if(site_index >= 0 && site_index < (int)g.sites.size()) g.sites[site_index] = site;
    else {
      g.sites.push_back(site);
      site_index = (int)g.sites.size() - 1;
    }
    save_store("sites", g.sites);
    rebuild_servers();
    show_toast(trf("Saved {0}", {json::get_str(site, "name")}));
  }

  void focus(int a, int b) override {
    (void)b;
    cursor = a;
  }

  bool input(Action a) override {
    std::vector<int> f = fields();
    int idx = (int)(std::find(f.begin(), f.end(), cursor) - f.begin());
    if(idx >= (int)f.size()) idx = 1;
    if(a == ACT_CIRCLE) return false;
    if(a == ACT_UP) cursor = f[(idx + f.size() - 1) % f.size()];
    if(a == ACT_DOWN) cursor = f[(idx + 1) % f.size()];
    bool on_buttons = cursor >= CONNECT;
    if(on_buttons && (a == ACT_LEFT || a == ACT_RIGHT)) {
      int b = cursor - CONNECT + (a == ACT_RIGHT ? 1 : 2);
      cursor = CONNECT + b % 3;
    }
    if(cursor == PROTO && (a == ACT_LEFT || a == ACT_RIGHT)) toggle_proto();
    if(a == ACT_CROSS) {
      switch(cursor) {
      case PROTO: toggle_proto(); break;
      case ANON: json::set_bool(site, "anonymous", !json::get_bool(site, "anonymous")); break;
      case CONNECT: {
        json::Object s = site;
        connect(s);
        return false;
      }
      case SAVE: save(); break;
      case CANCEL: return false;
      default: edit(cursor); break;
      }
    }
    return true;
  }

  void draw() override {
    std::vector<int> f = fields();
    float row = 60, w = dialog_w(1240);
    int text_rows = (int)f.size() - 3;
    float h = 130 + row * text_rows + 110;
    float x = (W - w) / 2, y = (H - h) / 2;
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 150));
    draw::rect(x, y, w, h, draw::PANEL, 18);
    hit_add(HIT_OVERLAY_BODY, x, y, w, h);
    draw::text(x + 40, y + 28, FS_TITLE, draw::TEXT,
               site_index >= 0 ? trf("Edit site: {0}", {json::get_str(site, "name")}).c_str() : tr("Connect to a server"), true);
    float yy = y + 96;
    for(int fi : f) {
      if(fi >= CONNECT) break;
      bool on = fi == cursor;
      hit_add(HIT_MENU_ITEM, x + 20, yy, w - 40, row - 6, fi);
      if(on) draw::rect(x + 20, yy, w - 40, row - 6, draw::SEL, 10);
      draw::text(x + 44, yy + 12, FS, on ? draw::TEXT : draw::MUTED, label(fi));
      std::string v = value(fi);
      bool empty = v.empty();
      if(empty) v = fi == HOST ? tr("e.g. 192.168.1.20") : fi == KEY ? tr("optional, e.g. /data/chiral-ftp/id_rsa") :
                    fi == USER ? (sftp() ? tr("your login name") : tr("e.g. anonymous")) : "";
      if(fi == PROTO || fi == ANON) v = "<  " + v + "  >";
      draw::text_clip(x + 440, yy + 12, FS, empty ? IM_COL32(100, 112, 138, 255) : draw::TEXT, v.c_str(), w - 500);
      yy += row;
    }
    const char *labels[3] = {tr("Connect"), tr("Save site"), tr("Cancel")};
    float bw = 250;
    for(int i = 0; i < 3; i++) {
      float bx = x + w - 40 - (3 - i) * (bw + 20) + 20;
      bool on = cursor == CONNECT + i;
      hit_add(HIT_MENU_ITEM, bx, yy + 24, bw, 64, CONNECT + i);
      draw::rect(bx, yy + 24, bw, 64, on ? (i == 0 ? draw::ACCENT : draw::SEL) : draw::PANEL2, 12);
      ImVec2 ts = draw::text_size(FS, labels[i], true);
      draw::text(bx + (bw - ts.x) / 2, yy + 40, FS, draw::TEXT, labels[i], true);
    }
  }
};

/* ---------- settings ---------- */

// Defined with the server screens, further down.
const std::string &local_ip();
void ip_changed();

struct Settings : Overlay {
  int cursor = 0;  // an index into rows()
  enum { LANG, NETWORK, CONNS, POLICY, RETRIES, MTIME, HIDDEN, SAVEPASS, KEYS, LOGVIEW, COUNT };

  /* The rows shown. Network only where the machine is on networks to choose
     between, which is a computer (system_networks returns 0 elsewhere). */
  static std::vector<int> rows() {
    system_net_t nets[16];
    const bool networks = system_networks(nets, 16) > 0;
    std::vector<int> r;
    for(int i = 0; i < COUNT; i++) {
      if(i == NETWORK && !networks) continue;
      r.push_back(i);
    }
    return r;
  }

  int id() {
    std::vector<int> r = rows();
    if(cursor >= (int)r.size()) cursor = (int)r.size() - 1;
    return r[cursor];
  }

  // "Automatic (English)" or a language's own name.
  static std::string language_value() {
    std::string choice = i18n_choice();
    std::string current = i18n_current();
    std::string name;
    for(int i = 0; i < i18n_count(); i++) if(current == i18n_code(i)) name = i18n_native_name(i);
    return choice == "auto" ? trf("Automatic ({0})", {name}) : name;
  }

  // Automatic, then each language.
  static void change_language(int dir) {
    int n = i18n_count() + 1, at = 0;
    std::string choice = i18n_choice();
    for(int i = 0; i < i18n_count(); i++) if(choice == i18n_code(i)) at = i + 1;
    at = (at + n + dir) % n;
    i18n_set(at ? i18n_code(at - 1) : "auto");
    save_prefs();
  }

  // "Wi-Fi  ·  192.168.1.23"; a tunnel says which one, there may be several.
  static std::string network_name(const system_net_t &n) {
    std::string label = n.label;
    if(label.empty()) label = n.name;
    else if(label == "VPN") label += std::string(" (") + n.name + ")";
    return label + "  \xc2\xb7  " + n.addr;
  }

  // "Automatic (192.168.1.23)", or the chosen network.
  static std::string network_value() {
    if(!g.network.empty()) {
      system_net_t nets[16];
      const int n = system_networks(nets, 16);
      for(int i = 0; i < n; i++) if(g.network == nets[i].name) return network_name(nets[i]);
    }
    return trf("Automatic ({0})", {local_ip()});
  }

  // Automatic, then each network the machine is on.
  static void change_network(int dir) {
    system_net_t nets[16];
    const int n = system_networks(nets, 16);
    int at = 0;
    for(int i = 0; i < n; i++) if(g.network == nets[i].name) at = i + 1;
    at = (at + n + 1 + dir) % (n + 1);
    g.network = at ? nets[at - 1].name : "";
    save_prefs();
    ip_changed();
  }

  static const char *policy_name(int p) {
    switch(p) {
    case POLICY_OVERWRITE: return tr("Always overwrite");
    case POLICY_SKIP: return tr("Skip");
    case POLICY_RENAME: return tr("Keep both (rename)");
    default: return tr("Resume if smaller, skip if same size");
    }
  }

  void change(int dir) {
    settings_t s = settings_get();
    switch(id()) {
    case LANG: change_language(dir); return;
    case NETWORK: change_network(dir); return;
    case CONNS: s.connections = std::max(1, std::min(MAX_TRANSFER_CONNECTIONS, s.connections + dir)); break;
    case POLICY: s.policy = (s.policy + 4 + dir) % 4; break;
    case RETRIES: s.retries = std::max(0, std::min(10, s.retries + dir)); break;
    case MTIME: s.preserve_mtime = !s.preserve_mtime; break;
    case HIDDEN: g.show_hidden = !g.show_hidden; save_prefs(); return;
    case SAVEPASS:
      g.save_pass = !g.save_pass;
      if(!g.save_pass) {
        for(auto &h : g.history) json::set_str(h, "pass", "");
        save_store("history", g.history);
      }
      save_prefs();
      return;
    default: return;
    }
    settings_set(&s);
    queue_kick();
  }

  void focus(int a, int b) override {
    (void)b;
    cursor = a;
  }

  // A right click steps a value back, as Left does.
  bool click(int a, int b, bool right) override {
    focus(a, b);
    if(right) {
      change(-1);
      return true;
    }
    return input(ACT_CROSS);
  }

  bool input(Action a) override;

  void draw() override {
    settings_t s = settings_get();
    const std::vector<int> shown = rows();
    const int count = (int)shown.size();
    // An extra row still fits above the hint bar.
    float row = count > 9 ? 62 : 66, w = 1240, h = 140 + row * (float)count + 74;
    float x = (W - w) / 2, y = (H - h) / 2;
    char buf[64];
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 150));
    draw::rect(x, y, w, h, draw::PANEL, 18);
    hit_add(HIT_OVERLAY_BODY, x, y, w, h);
    draw::text(x + 40, y + 28, FS_TITLE, draw::TEXT, tr("Settings"), true);
    struct { const char *label; std::string value; } rows[COUNT];
    rows[LANG] = {tr("Language"), "<  " + language_value() + "  >"};
    rows[NETWORK] = {tr("Network"), "<  " + network_value() + "  >"};
    snprintf(buf, sizeof(buf), "<  %d  >", s.connections);
    rows[CONNS] = {tr("Simultaneous transfers"), buf};
    rows[POLICY] = {tr("When the file already exists"), std::string("<  ") + policy_name(s.policy) + "  >"};
    snprintf(buf, sizeof(buf), "<  %d  >", s.retries);
    rows[RETRIES] = {tr("Retries after a dropped connection"), buf};
    rows[MTIME] = {tr("Keep file dates"), s.preserve_mtime ? tr("On") : tr("Off")};
    rows[HIDDEN] = {tr("Show hidden files (FTP LIST -a)"), g.show_hidden ? tr("On") : tr("Off")};
    rows[SAVEPASS] = {tr("Remember passwords in Recent"), g.save_pass ? tr("On") : tr("Off")};
    rows[KEYS] = {tr("Forget saved SFTP host keys"), ""};
    rows[LOGVIEW] = {tr("Message log"), ""};
    const int current = id();
    float yy = y + 100;
    for(int k = 0; k < count; k++) {
      const int i = shown[k];
      bool on = k == cursor;
      hit_add(HIT_MENU_ITEM, x + 20, yy, w - 40, row - 6, k);
      if(on) draw::rect(x + 20, yy, w - 40, row - 6, draw::SEL, 10);
      draw::text(x + 44, yy + (row - 36) / 2, FS, draw::TEXT, rows[i].label, on);
      ImVec2 ts = draw::text_size(FS, rows[i].value.c_str());
      draw::text(x + w - 60 - ts.x, yy + (row - 36) / 2, FS,
                 i == CONNS || i == LANG || i == NETWORK ? draw::ACCENT2 : draw::MUTED, rows[i].value.c_str());
      yy += row;
    }
    const char *hint = current == CONNS ? tr("Each transfer logs in with its own connection, like FileZilla. Up to 30; the queue backs off if the server allows fewer.")
                       : current == NETWORK ? tr("The address the FTP server and web app screens show. Automatic picks your local network (192.168.x.x) over a VPN.")
                       : current == LANG ? tr(device_text(N_("Automatic follows the console's system language."),
                                                          N_("Automatic follows your system language.")))
                                         : tr(device_text(N_("Left/Right to change. Settings are saved on the console right away."),
                                                          N_("Left/Right or click to change. Settings are saved right away.")));
    std::vector<std::string> hint_lines = wrap_text(hint, FS_SMALL, w - 88);
    for(size_t k = 0; k < hint_lines.size() && k < 2; k++)
      draw::text(x + 44, yy + 6 + 32 * (float)k, FS_SMALL, draw::MUTED, hint_lines[k].c_str());
  }
};

struct LogView : Overlay {
  int scroll = 0;  // lines from the bottom
  OverlayKind kind() override { return OV_LOG; }

  bool input(Action a) override {
    int n = (int)g.log.size();
    if(a == ACT_CIRCLE || a == ACT_TOUCHPAD) return false;
    if(a == ACT_UP) scroll = std::min(n, scroll + 1);
    if(a == ACT_DOWN) scroll = std::max(0, scroll - 1);
    if(a == ACT_L2) scroll = std::min(n, scroll + 20);
    if(a == ACT_R2) scroll = std::max(0, scroll - 20);
    if(a == ACT_TRIANGLE) {
      g.log.clear();
      log_clear();
      scroll = 0;
    }
    return true;
  }

  void draw() override {
    float x = 60, y = 60, w = W - 120, h = H - 120;
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 170));
    draw::rect(x, y, w, h, draw::PANEL, 18);
    hit_add(HIT_OVERLAY_BODY, x, y, w, h);
    draw::text(x + 36, y + 24, FS_TITLE, draw::TEXT, tr("Message log"), true);
    float lh = 30;
    int rows = (int)((h - 160) / lh);
    int n = (int)g.log.size();
    int end = n - scroll, start = std::max(0, end - rows);
    float yy = y + 84;
    for(int i = start; i < end; i++) {
      int kind = g.log[i].first;
      ImU32 col = kind == LOG_COMMAND ? IM_COL32(127, 183, 255, 255) : kind == LOG_RESPONSE ? draw::OK :
                  kind == LOG_ERROR ? draw::ERR : draw::TEXT;
      draw::text_clip(x + 36, yy, 22, col, g.log[i].second.c_str(), w - 72);
      yy += lh;
    }
    float hx = x + 36, hy = y + h - 60;
    hx += hint(hx, hy, ACT_UP, tr("Scroll"));
    hx += hint(hx, hy, ACT_TRIANGLE, tr("Clear"));
    hint(hx, hy, ACT_CIRCLE, tr("Close"));
  }
};

/* ---------- about ---------- */

struct About : Overlay {
  int scroll = 0;

  bool input(Action a) override {
    if(a == ACT_CIRCLE || a == ACT_OPTIONS || a == ACT_CROSS) return false;
    if(a == ACT_DOWN) scroll = std::min(scroll + 1, 8);
    if(a == ACT_UP) scroll = std::max(scroll - 1, 0);
    return true;
  }

  void draw() override {
    float w = dialog_w(1400), h = 960, x = (W - w) / 2, y = (H - h) / 2;
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 160));
    draw::rect(x, y, w, h, draw::PANEL, 18);
    hit_add(HIT_OVERLAY_BODY, x, y, w, h);
    draw::brand_mark(x + 50, y + 34, 84);
    float ww = draw::brand_wordmark(x + 150, y + 44, 56);
    draw::text(x + 150 + ww + 24, y + 66, FS, draw::MUTED, trf("version {0}", {VERSION_TAG}).c_str());
    struct Line { float size; ImU32 col; bool bold; const char *text; };
    std::vector<Line> lines = {
        {FS, draw::TEXT, false, device_text(N_("The ultimate file manager and FTP interface for this console:"),
                                            N_("The ultimate file manager and FTP interface for this computer:"))},
        {FS_READ, draw::MUTED, false, device_text(N_("  FTP server: other devices connect to the console with any FTP app."),
                                                  N_("  FTP server: other devices connect to this computer with any FTP app."))},
        {FS_READ, draw::MUTED, false, device_text(N_("  Web app: manage the console's files from a browser, upload and download."),
                                                  N_("  Web app: manage this computer's files from another device's browser."))},
        {FS_READ, draw::MUTED, false, platform::about_file_manager()},
        {FS_READ, draw::MUTED, false, N_("  FTP / SFTP client: up to 30 transfers at once with your laptop, PC or NAS.")},
        {FS_READ, draw::TEXT, false, N_("chiral-ftp runs on Windows, Mac, Linux, Android, Nintendo Switch and PS5. Get it at:")},
        {FS_READ, draw::ACCENT2, false, "chiral-ftp.com"},
        {12, 0, false, ""},
        {FS, draw::ACCENT2, true, N_("Made by Herbrax (Simo Hakim)")},
        {FS_READ, draw::MUTED, false, "simohakim.com  \xc2\xb7  linkedin.com/in/simo-hakim"},
        {FS, draw::TEXT, false, N_("Found a bug? Please report it on GitHub. Pull requests are welcome.")},
        {FS_READ, draw::ACCENT2, false, "github.com/Herbrax/Chiral-FTP"},
        {12, 0, false, ""},
        {FS_READ, draw::TEXT, true, N_("Built with open-source work by others, thank you:")},
    };
    const char *const *thanks = nullptr;
    for(int i = 0, n = platform::about_thanks(&thanks); i < n; i++) lines.push_back({FS_READ, draw::MUTED, false, thanks[i]});
    lines.push_back({FS_READ, draw::MUTED, false,
                     N_("chiral-ftp is free software under the GPL-3.0; licenses are in the app's licenses folder.")});
    float yy = y + 140;
    int first = scroll;
    for(int i = first; i < (int)lines.size(); i++) {
      if(lines[i].text[0]) draw::text_clip(x + 50, yy, lines[i].size, lines[i].col, tr(lines[i].text), w - 100, lines[i].bold);
      yy += lines[i].size + 17;
      if(yy > y + h - 100) break;
    }
    hint(x + 50, y + h - 70, ACT_CIRCLE, tr("Close"));
  }
};

bool Settings::input(Action a) {
  const int n = (int)rows().size();
  if(a == ACT_CIRCLE || a == ACT_OPTIONS) return false;
  if(a == ACT_UP) cursor = (cursor + n - 1) % n;
  if(a == ACT_DOWN) cursor = (cursor + 1) % n;
  if(a == ACT_LEFT) change(-1);
  if(a == ACT_RIGHT) change(1);
  if(a == ACT_CROSS) {
    if(id() == KEYS) {
      confirm(tr("Forget host keys"), tr("Forget every saved SFTP server key? The next connection trusts the server's current key."),
              tr("Forget"), [] { show_toast(ftp_forget_host_keys() ? tr("Could not remove the keys") : tr("Saved host keys forgotten")); });
    } else if(id() == LOGVIEW) {
      push(new LogView);
    } else {
      change(1);
    }
  }
  return true;
}

/* ---------- transfer queue ---------- */

struct QueueView : Overlay {
  int tab = 0, cursor = 0;
  std::vector<queue_view_t> items;
  const char *tab_name() { return tab == 0 ? "queued" : tab == 1 ? "failed" : "done"; }
  OverlayKind kind() override { return OV_QUEUE; }

  void refresh_items() {
    static queue_view_t buf[200];
    queue_stats_t st;
    int n = queue_snapshot(tab_name(), buf, 200, &st);
    items.assign(buf, buf + n);
    if(cursor >= n) cursor = n - 1;
    if(cursor < 0) cursor = 0;
  }

  // b: 0 a transfer's row, 1 a tab.
  void focus(int a, int b) override {
    if(b == 0) cursor = a;
  }

  bool click(int a, int b, bool right) override {
    if(right) return true;
    if(b == 1) {
      tab = a;
      cursor = 0;
      return true;
    }
    focus(a, b);
    return input(ACT_CROSS);
  }

  bool input(Action a) override {
    if(a == ACT_CIRCLE || a == ACT_TOUCHPAD) return false;
    if(a == ACT_L1) tab = (tab + 2) % 3, cursor = 0;
    if(a == ACT_R1) tab = (tab + 1) % 3, cursor = 0;
    refresh_items();
    int n = (int)items.size();
    if(a == ACT_UP && n) cursor = (cursor + n - 1) % n;
    if(a == ACT_DOWN && n) cursor = (cursor + 1) % n;
    if(a == ACT_TRIANGLE) queue_set_paused(!g.qs.paused);
    if(a == ACT_SQUARE) {
      if(tab == 1) queue_retry(0);
      else if(tab == 2) queue_clear("done");
      else confirm(tr("Cancel all"), tr("Cancel every queued and running transfer?"), tr("Cancel all"), [] { queue_cancel(0); });
    }
    if(a == ACT_CROSS && n) {
      queue_view_t it = items[cursor];
      Menu *m = new Menu;
      m->title = base_of(it.upload ? it.local : it.remote);
      m->subtitle = it.error[0] ? std::string(it.error) : it.upload ? trf("Upload to {0}", {it.remote}) : trf("Download to {0}", {it.local});
      unsigned long id = it.id;
      std::string st = it.state;
      if(st == "queued" || st == "running") m->items.push_back({tr("Cancel transfer"), [id] { queue_cancel(id); }, true, true});
      if(st == "failed" || st == "canceled") m->items.push_back({tr("Retry"), [id] { queue_retry(id); }});
      if(st != "running") m->items.push_back({tr("Remove from list"), [id] { queue_remove(id); }});
      m->items.push_back({tr("Close"), [] {}});
      push(m);
    }
    return true;
  }

  void draw() override {
    refresh_items();
    float x = 36, y = 300, w = W - 72, h = H - y - HINT_H - 10;
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 110));
    draw::rect(x, y, w, h, draw::PANEL, 18);
    hit_add(HIT_OVERLAY_BODY, x, y, w, h);
    const char *tabs[3] = {tr("Queued"), tr("Failed"), tr("Successful")};
    int counts[3] = {g.qs.queued, g.qs.failed, g.qs.done};
    float tx = x + 36;
    hit_add(HIT_HINT, tx, y + 24, draw::button(tx, y + 24, 40, ACT_L1), 40, ACT_L1);
    tx += 70;
    for(int i = 0; i < 3; i++) {
      char buf[64];
      snprintf(buf, sizeof(buf), "%s  %d", tabs[i], counts[i]);
      ImVec2 ts = draw::text_size(FS, buf, true);
      hit_add(HIT_MENU_ITEM, tx - 16, y + 18, ts.x + 32, 54, i, 1);
      if(i == tab) draw::rect(tx - 16, y + 18, ts.x + 32, 54, draw::SEL, 12);
      draw::text(tx, y + 28, FS, i == tab ? draw::TEXT : draw::MUTED, buf, true);
      tx += ts.x + 56;
    }
    hit_add(HIT_HINT, tx - 20, y + 24, draw::button(tx - 20, y + 24, 40, ACT_R1), 40, ACT_R1);
    char sum[160];
    snprintf(sum, sizeof(sum), "%s%s%s", g.qs.paused ? (std::string(tr("Paused")) + "  ·  ").c_str() : "",
             trf("{0} of {1} connections busy", {std::to_string(g.qs.running), std::to_string(settings_get().connections)}).c_str(),
             g.qs.speed > 0 ? ("  ·  " + fmt_size((long long)g.qs.speed) + "/s").c_str() : "");
    ImVec2 ss = draw::text_size(FS_SMALL, sum);
    draw::text(x + w - 36 - ss.x, y + 34, FS_SMALL, g.qs.paused ? draw::ACCENT2 : draw::MUTED, sum);

    float row = 74, yy = y + 96;
    int rows = (int)((h - 110) / row);
    int start = std::max(0, std::min(cursor - rows / 2, (int)items.size() - rows));
    if(items.empty()) {
      const char *msg = tab == 0 ? tr(device_text(N_("No transfers. Mark files and press triangle to send them to the other side."),
                                                  N_("No transfers. Mark files and press T to send them to the other side."))) :
                        tab == 1 ? tr("No failed transfers.") : tr("Finished transfers appear here.");
      draw::text(x + 40, yy + 20, FS, draw::MUTED, msg);
    }
    for(int i = start; i < (int)items.size() && i < start + rows; i++) {
      queue_view_t &it = items[i];
      bool on = i == cursor;
      hit_add(HIT_MENU_ITEM, x + 20, yy, w - 40, row - 8, i, 0);
      if(on) draw::rect(x + 20, yy, w - 40, row - 8, draw::SEL, 10);
      draw::text(x + 40, yy + 8, FS, it.upload ? draw::ACCENT : draw::ACCENT2, it.upload ? tr("UP") : tr("DN"), true);
      std::string name = base_of(it.upload ? it.local : it.remote);
      if(it.is_dir) name += "/";
      draw::text_clip(x + 110, yy + 6, FS, draw::TEXT, name.c_str(), 760, true);
      std::string where = trf("to {0}", {it.upload ? it.remote : it.local});
      draw::text_clip(x + 110, yy + 40, 20, draw::MUTED, where.c_str(), 760);
      float bx = x + 900, bw = 520;
      std::string state = it.state;
      double pct = it.size > 0 ? std::min(1.0, (double)it.done / (double)it.size) : (state == "done" ? 1.0 : 0.0);
      if(!it.is_dir) {
        draw::rect(bx, yy + 20, bw, 26, draw::BG, 13);
        draw::rect(bx, yy + 20, (float)(bw * pct), 26, state == "failed" ? draw::ERR : state == "done" ? draw::OK : draw::ACCENT, 13);
        char p[96];
        snprintf(p, sizeof(p), "%d%%  %s / %s", (int)(pct * 100), fmt_size(it.done).c_str(), fmt_size(it.size).c_str());
        draw::text(bx + 16, yy + 20, 21, draw::TEXT, p);
      }
      std::string status;
      if(state == "running") {
        char s[96];
        if(it.is_dir) snprintf(s, sizeof(s), "%s", trf("#{0} listing folder", {std::to_string(it.slot)}).c_str());
        else if(it.speed > 0 && it.size > 0) {
          int eta = (int)((it.size - it.done) / it.speed);
          snprintf(s, sizeof(s), "#%d  %s/s  %d:%02d", it.slot, fmt_size((long long)it.speed).c_str(), eta / 60, eta % 60);
        } else snprintf(s, sizeof(s), "%s", trf("#{0} starting", {std::to_string(it.slot)}).c_str());
        status = s;
      } else if(state == "queued") status = it.attempts ? trf("Retry {0}", {std::to_string(it.attempts)}) : tr("Waiting");
      else if(state == "done") status = it.is_dir ? tr("Folder listed") : tr("Done");
      else if(state == "skipped") status = trf("Skipped: {0}", {it.error});
      else status = it.error[0] ? it.error : state;
      ImU32 sc = state == "failed" || state == "canceled" ? draw::ERR : state == "done" ? draw::OK : draw::MUTED;
      draw::text_clip(bx + bw + 30, yy + 18, 22, sc, status.c_str(), x + w - (bx + bw + 30) - 30);
      yy += row;
    }
  }
};

/* ---------- pane actions ---------- */

bool is_fm(const Pane &p) {
  return &p == &g.fm[0] || &p == &g.fm[1];
}

void fm_copy(Pane &from, bool move);

std::vector<Entry> selection(Pane &p) {
  std::vector<Entry> out;
  for(auto &e : p.entries) {
    if(p.marked.count(e.name)) out.push_back(e);
  }
  if(out.empty() && p.cursor < (int)p.entries.size() && !p.entries[p.cursor].drive) out.push_back(p.entries[p.cursor]);
  return out;
}

void transfer(Pane &from) {
  Pane &to = from.remote ? local() : remote();
  if(!g.connected) {
    show_toast(tr("Connect to a server first (right side)"), true);
    return;
  }
  if(to.path.empty()) {
    show_toast(from.remote ? tr(device_text(N_("Open a console folder on the left first"), N_("Open a local folder on the left first")))
                            : tr("Open a server folder first"), true);
    return;
  }
  std::vector<Entry> items = selection(from);
  if(items.empty()) return;
  ftp_server_t s;
  if(!session_server(&s)) return;
  int added = 0;
  for(auto &e : items) {
    std::string src = join(from.path, e.name);
    std::string dst = join(to.path, e.name);
    if(!queue_add(from.remote ? QUEUE_DOWNLOAD : QUEUE_UPLOAD, &s, from.remote ? dst.c_str() : src.c_str(),
                  from.remote ? src.c_str() : dst.c_str(), e.dir, e.dir ? -1 : e.size, e.mtime)) {
      added++;
    }
  }
  from.marked.clear();
  char buf[128];
  snprintf(buf, sizeof(buf), "%s", trf(from.remote ? N_("Downloading {0} items")
                                                   : N_("Uploading {0} items"), {std::to_string(added)}).c_str());
  show_toast(buf);
}

struct PathCtx {
  std::string a, b;
  unsigned mode;
  int (*op)(ftp_conn_t *, const char *);
};

int run_path_op(ftp_conn_t *c, void *p) {
  PathCtx *x = (PathCtx *)p;
  return x->op(c, x->a.c_str());
}

int run_rename(ftp_conn_t *c, void *p) {
  PathCtx *x = (PathCtx *)p;
  return ftp_rename(c, x->a.c_str(), x->b.c_str());
}

int run_chmod(ftp_conn_t *c, void *p) {
  PathCtx *x = (PathCtx *)p;
  return ftp_chmod(c, x->a.c_str(), x->mode);
}

int run_chmod_tree(ftp_conn_t *c, void *p) {
  PathCtx *x = (PathCtx *)p;
  return remote_chmod_tree(c, x->a.c_str(), x->mode);
}

struct DeleteCtx {
  std::vector<std::pair<std::string, bool>> paths;
};

int run_delete(ftp_conn_t *c, void *p) {
  DeleteCtx *x = (DeleteCtx *)p;
  for(auto &it : x->paths) {
    if(it.second ? remote_delete_tree(c, it.first.c_str()) : ftp_delete(c, it.first.c_str())) return -1;
  }
  return 0;
}

// Runs a remote operation in the background, then refreshes the pane.
void remote_op(std::function<int(char *, size_t)> op, const std::string &done_msg) {
  auto err = std::make_shared<std::string>();
  auto ok = std::make_shared<bool>(false);
  remote().loading++;
  g_jobs.run([op, err, ok] {
    char e[512] = "";
    *ok = !op(e, sizeof(e));
    *err = e;
  }, [err, ok, done_msg] {
    remote().loading--;
    show_toast(*ok ? done_msg : *err, !*ok);
    refresh(remote());
  });
}

void make_folder(Pane &p) {
  bool is_remote = p.remote;
  std::string dir = p.path;
  Pane *pp = &p;
  prompt(tr("New folder name"), tr("New folder"), TEXT_DEFAULT, [is_remote, dir, pp](std::string name) {
    if(name.empty() || name.find('/') != std::string::npos) return;
    std::string path = join(dir, name);
    if(is_remote) {
      remote_op([path](char *e, size_t n) {
        PathCtx x{path, "", 0, ftp_mkdir};
        return session_run(run_path_op, &x, e, n);
      }, trf("Created {0}", {name}));
    } else {
      if(localfs_mkdir(path.c_str())) show_toast(trf("Could not create folder: {0}", {strerror(errno)}), true);
      else show_toast(trf("Created {0}", {name}));
      load_local(dir, true, pp);
    }
  });
}

void rename_item(Pane &p, const Entry &e) {
  bool is_remote = p.remote;
  std::string dir = p.path, old = e.name;
  Pane *pp = &p;
  prompt(trf("Rename \"{0}\"", {e.name}), e.name, TEXT_DEFAULT, [is_remote, dir, old, pp](std::string name) {
    if(name.empty() || name == old || name.find('/') != std::string::npos) return;
    std::string from = join(dir, old), to = join(dir, name);
    if(is_remote) {
      remote_op([from, to](char *e, size_t n) {
        PathCtx x{from, to, 0, nullptr};
        return session_run(run_rename, &x, e, n);
      }, trf("Renamed to {0}", {name}));
    } else {
      if(rename(from.c_str(), to.c_str())) show_toast(trf("Could not rename: {0}", {strerror(errno)}), true);
      load_local(dir, true, pp);
    }
  });
}

void delete_items(Pane &p) {
  std::vector<Entry> items = selection(p);
  if(items.empty()) return;
  bool is_remote = p.remote;
  std::string dir = p.path;
  Pane *pp = &p;
  std::string what = items.size() == 1 ? "\"" + items[0].name + "\"" : trf("{0} items", {std::to_string(items.size())});
  confirm(tr("Delete"), trf(is_remote ? N_("Permanently delete {0} from the server? Folders are deleted with everything inside.")
                                      : device_text(N_("Permanently delete {0} from the console? Folders are deleted with everything inside."),
                                                    N_("Permanently delete {0} from this computer? Folders are deleted with everything inside.")), {what}),
          tr("Delete"), [items, is_remote, dir, pp] {
    if(is_remote) {
      auto ctx = std::make_shared<DeleteCtx>();
      for(auto &e : items) ctx->paths.push_back({join(dir, e.name), e.dir && !e.link});
      remote_op([ctx](char *e, size_t n) { return session_run(run_delete, ctx.get(), e, n); },
                trf("Deleted {0}", {items.size() == 1 ? items[0].name : trf("{0} items", {std::to_string(items.size())})}));
      remote().marked.clear();
    } else {
      auto failed = std::make_shared<std::string>();
      pp->loading++;
      g_jobs.run([items, dir, failed] {
        for(auto &e : items) {
          if(localfs_delete(join(dir, e.name).c_str()) && failed->empty()) {
            *failed = trf("Could not delete everything in \"{0}\": {1}", {e.name, strerror(errno)});
          }
        }
      }, [dir, failed, pp] {
        pp->loading--;
        pp->marked.clear();
        show_toast(failed->empty() ? tr("Deleted") : *failed, !failed->empty());
        load_local(dir, true, pp);
      });
    }
  });
}

/* deep: folders with everything in them (remote_chmod_tree,
   localfs_chmod_tree) -- "Change permissions recursively". */
void chmod_items(Pane &p, bool deep = false) {
  std::vector<Entry> items = selection(p);
  if(items.empty()) return;
  bool is_remote = p.remote;
  std::string dir = p.path;
  std::string cur = items[0].perm.size() == 4 ? items[0].perm.substr(1) : "644";
  Pane *pp = &p;
  prompt(tr("Permissions (octal, e.g. 755)"), cur, TEXT_NUMBER, [items, is_remote, dir, pp, deep](std::string mode) {
    if(mode.size() < 3 || mode.size() > 4 || mode.find_first_not_of("01234567") != std::string::npos) {
      show_toast(tr("Use an octal mode like 755"), true);
      return;
    }
    const unsigned m = (unsigned)strtoul(mode.c_str(), nullptr, 8);
    if(is_remote) {
      auto paths = std::make_shared<std::vector<std::pair<std::string, bool>>>();
      for(auto &e : items) paths->push_back({join(dir, e.name), deep && e.dir && !e.link});
      remote_op([paths, m](char *e, size_t n) {
        for(auto &p : *paths) {
          PathCtx x{p.first, "", m, nullptr};
          if(session_run(p.second ? run_chmod_tree : run_chmod, &x, e, n)) return -1;
        }
        return 0;
      }, tr("Permissions changed"));
    } else {
      bool failed = false;
      for(auto &e : items) {
        const std::string path = join(dir, e.name);
        if(deep && e.dir && !e.link ? localfs_chmod_tree(path.c_str(), m) : chmod(path.c_str(), m)) failed = deep;
      }
      if(failed) show_toast(tr("Some permissions couldn't be changed"), true);
      load_local(dir, true, pp);
    }
  });
}

// Whether a folder is among what the item actions would act on (selection()).
bool selection_has_folder(Pane &p) {
  for(auto &e : selection(p)) if(e.dir && !e.link) return true;
  return false;
}

void open_entry(Pane &p) {
  if(p.cursor >= (int)p.entries.size()) return;
  Entry e = p.entries[p.cursor];
  if(e.drive) {
    load_local(e.name, false, &p);
    return;
  }
  if(e.dir) {
    if(p.remote) load_remote(join(p.path, e.name));
    else load_local(join(p.path, e.name), false, &p);
    return;
  }
  // A file: offer its actions.
  Menu *m = new Menu;
  m->title = e.name;
  m->subtitle = fmt_size(e.size) + (e.mtime >= 0 ? "  ·  " + fmt_date(e.mtime) : "") + (e.perm.empty() ? "" : "  ·  " + e.perm);
  Pane *pp = &p;
  if(is_fm(p)) {
    m->items.push_back({tr("Copy to the other side"), [pp] { fm_copy(*pp, false); }});
    m->items.push_back({tr("Move to the other side"), [pp] { fm_copy(*pp, true); }});
  } else {
    m->items.push_back({p.remote ? tr(device_text(N_("Download to the console"), N_("Download to this computer"))) : tr("Upload to server"),
                        [pp] { transfer(*pp); }, g.connected});
  }
  m->items.push_back({p.marked.count(e.name) ? tr("Unmark") : tr("Mark"), [pp, e] {
    if(pp->marked.count(e.name)) pp->marked.erase(e.name);
    else pp->marked.insert(e.name);
  }});
  m->items.push_back({tr("Rename"), [pp, e] { rename_item(*pp, e); }});
  m->items.push_back({tr("Change permissions"), [pp] { chmod_items(*pp); }});
  if(selection_has_folder(p)) m->items.push_back({tr("Change permissions recursively"), [pp] { chmod_items(*pp, true); }});
  m->items.push_back({tr("Delete"), [pp] { delete_items(*pp); }, true, true});
  push(m);
}

void go_up(Pane &p) {
  if(p.remote) {
    if(!g.connected || p.path == "/" || p.path.empty()) return;
    load_remote(parent_of(p.path));
    return;
  }
  if(p.path.empty()) return;
  // From a drive's top folder, go back to the drive list; a shortcut inside
  // another drive (Homebrew in /data) goes up to its folder instead.
  localfs_root_t roots[24];
  int n = localfs_roots(roots, 24);
  auto inside_other = [&](const std::string &path) {
    for(int k = 0; k < n; k++) {
      std::string r = roots[k].path;
      if(r != "/" && r != path && path.compare(0, r.size() + 1, r + "/") == 0) return true;
    }
    return false;
  };
  for(int i = 0; i < n; i++) {
    if(p.path == roots[i].path && !inside_other(p.path)) {
      std::string was = p.path;
      load_drives(p);
      for(int k = 0; k < (int)p.entries.size(); k++) if(p.entries[k].name == was) p.cursor = k;
      return;
    }
  }
  std::string from = base_of(p.path);
  load_local(parent_of(p.path), false, &p);
  for(int k = 0; k < (int)p.entries.size(); k++) if(p.entries[k].name == from) p.cursor = k;
}

void edit_site(int index) {
  push(new Form(g.sites[index], index));
}

void main_menu() {
  Pane &p = active();
  bool in_folder = p.remote ? g.connected : !p.path.empty();
  Menu *m = new Menu;
  m->title = tr("Menu");
  Pane *pp = &p;
  m->items.push_back({tr("Start screen"), [] { g.mode = MODE_HOME; }});
  m->items.push_back({tr("New connection..."), [] { push(new Form(json::Object(), -1)); }});
  if(g.connected) m->items.push_back({tr("Disconnect"), [] { disconnect(); }});
  m->items.push_back({tr("Refresh"), [pp] { refresh(*pp); }, in_folder});
  m->items.push_back({tr("New folder"), [pp] { make_folder(*pp); }, in_folder});
  /* What the cursor is on, or what is marked -- the only way to rename,
     chmod or delete a folder, since Cross opens one. */
  const bool on_item = in_folder && (!p.marked.empty() || (p.cursor < (int)p.entries.size() && !p.entries[p.cursor].drive));
  if(on_item && p.marked.empty()) {
    Entry e = p.entries[p.cursor];
    m->items.push_back({trf("Rename \"{0}\"", {e.name}), [pp, e] { rename_item(*pp, e); }});
  }
  if(on_item) {
    m->items.push_back({tr("Change permissions"), [pp] { chmod_items(*pp); }});
    if(selection_has_folder(p)) m->items.push_back({tr("Change permissions recursively"), [pp] { chmod_items(*pp, true); }});
    m->items.push_back({tr("Delete"), [pp] { delete_items(*pp); }, true, true});
  }
  m->items.push_back({tr("Mark all"), [pp] { for(auto &e : pp->entries) if(!e.drive) pp->marked.insert(e.name); }, in_folder});
  m->items.push_back({tr("Clear marks"), [pp] { pp->marked.clear(); }, !p.marked.empty()});
  if(!p.remote) m->items.push_back({tr("Drives"), [] { load_drives(); }});
  m->items.push_back({tr("Transfers"), [] { push(new QueueView); }});
  m->items.push_back({tr("Settings"), [] { push(new Settings); }});
  m->items.push_back({tr("Message log"), [] { push(new LogView); }});
  m->items.push_back({tr("About chiral-ftp"), [] { push(new About); }});
  if(platform::can_quit()) {
    m->items.push_back({tr("Exit chiral-ftp"), [] {
      confirm(tr("Exit"), tr("Close chiral-ftp? Running transfers stop."), tr("Exit"), [] { g.quit = true; });
    }, true, true});
  } else {
    m->items.push_back({tr("To close chiral-ftp, use the PS button"), [] {}, false});
  }
  push(m);
}

void server_action(Action a) {
  Pane &p = remote();
  if(p.cursor >= (int)g.servers.size()) return;
  ServerItem it = g.servers[p.cursor];
  if(a == ACT_CROSS) {
    if(it.kind == ServerItem::NEW) push(new Form(json::Object(), -1));
    else if(it.kind == ServerItem::SITE) connect(g.sites[it.index]);
    else {
      json::Object h = g.history[it.index];
      bool needs_pass = json::get_str(h, "pass").empty() && !json::get_bool(h, "anonymous") &&
                        !json::get_str(h, "user").empty() && json::get_str(h, "keyfile").empty();
      if(needs_pass) {
        prompt(trf("Password for {0}", {json::get_str(h, "user")}), "", TEXT_PASSWORD, [h](std::string pw) mutable {
          json::set_str(h, "pass", pw);
          connect(h);
        });
      } else {
        connect(h);
      }
    }
  } else if(a == ACT_TRIANGLE) {
    if(it.kind == ServerItem::SITE) edit_site(it.index);
    else if(it.kind == ServerItem::RECENT) {
      json::Object h = g.history[it.index];
      h.erase("time");
      push(new Form(h, -1));
    }
  } else if(a == ACT_SQUARE && it.kind != ServerItem::NEW) {
    bool site = it.kind == ServerItem::SITE;
    int idx = it.index;
    std::string name = site ? json::get_str(g.sites[idx], "name") : site_label(g.history[idx]);
    confirm(site ? tr("Delete site") : tr("Remove from Recent"), trf("Remove \"{0}\"?", {name}), tr("Remove"), [site, idx] {
      auto &v = site ? g.sites : g.history;
      if(idx < (int)v.size()) v.erase(v.begin() + idx);
      save_store(site ? "sites" : "history", v);
      rebuild_servers();
    });
  }
}

void move_cursor(Pane &p, int count, int delta) {
  if(!count) return;
  p.cursor = std::max(0, std::min(count - 1, p.cursor + delta));
}

/* ---------- drawing ---------- */

int visible_rows() {
  float h = H - PANE_Y - STATUS_H - HINT_H - 16 - PANE_HEAD - PANE_FOOT;
  return (int)(h / ROW_H);
}

void draw_server_list(float x, float y, float w) {
  Pane &p = remote();
  bool focus = g.active == 1 && g.overlays.empty();
  int rows = visible_rows();
  if(p.cursor < p.scroll) p.scroll = p.cursor;
  if(p.cursor >= p.scroll + rows) p.scroll = p.cursor - rows + 1;
  float yy = y;
  for(int i = p.scroll; i < (int)g.servers.size() && i < p.scroll + rows; i++) {
    ServerItem it = g.servers[i];
    bool on = i == p.cursor;
    hit_add(HIT_SERVER, x + 8, yy, w - 16, ROW_H, i);
    if(on) draw::rect(x + 8, yy + 2, w - 16, ROW_H * 1.0f - 4, focus ? draw::SEL : draw::PANEL2, 10);
    std::string title, sub;
    if(it.kind == ServerItem::NEW) {
      title = tr("+  New connection");
      sub = tr("FTP or SFTP");
    } else {
      const json::Object &o = it.kind == ServerItem::SITE ? g.sites[it.index] : g.history[it.index];
      title = json::get_str(o, "name");
      if(title.empty()) title = json::get_str(o, "host");
      sub = site_label(o);
    }
    const char *tag = it.kind == ServerItem::SITE ? tr("SAVED") : it.kind == ServerItem::RECENT ? tr("RECENT") : "";
    if(*tag) {
      ImVec2 ts = draw::text_size(18, tag, true);
      draw::rect(x + 24, yy + 15, ts.x + 20, 26, it.kind == ServerItem::SITE ? IM_COL32(60, 52, 20, 255) : IM_COL32(28, 46, 76, 255), 8);
      draw::text(x + 34, yy + 17, 18, it.kind == ServerItem::SITE ? draw::ACCENT2 : draw::ACCENT, tag, true);
    }
    float tx = x + (*tag ? 140 : 28);
    draw::text_clip(tx, yy + 11, FS, draw::TEXT, title.c_str(), w * 0.45f, on);
    ImVec2 ss = draw::text_size(FS_SMALL, sub.c_str());
    float sx = std::max(tx + w * 0.47f, x + w - 28 - ss.x);
    draw::text_clip(sx, yy + 15, FS_SMALL, draw::MUTED, sub.c_str(), x + w - 28 - sx);
    yy += ROW_H;
  }
  if(g.sites.empty() && g.history.empty()) {
    draw::text(x + 28, yy + 30, FS_SMALL, draw::MUTED, tr("Your saved sites and recent connections appear here."));
    draw::text(x + 28, yy + 64, FS_SMALL, draw::MUTED, tr("Mac/Linux: use SFTP with Remote Login. Windows: FileZilla Server."));
  }
}

// Draws one pane: the client's console side (idx 0) or server side (idx 1),
// or a file manager pane (fm: both are folders on the console).
void draw_pane(Pane &p, int idx, bool fm, float x, float y, float w, float h) {
  bool focus = (fm ? g.fm_active : g.active) == idx && g.overlays.empty();
  bool server_side = !fm && idx == 1;
  draw::rect(x, y, w, h, draw::PANEL, 16);
  if(focus) draw::frame(x, y, w, h, draw::ACCENT, 16, 3);

  // Header: which side, and where we are.
#if CHIRAL_UI_MOBILE
  const char *who = !server_side ? (platform::windowed() ? tr("This phone") : tr("This TV"))
#else
  const char *who = !server_side ? device_text("Console", tr("This computer"))
#endif
                    : g.connected ? (g.server_label.empty() ? tr("Server") : g.server_label.c_str())
                                  : tr("Servers");
  draw::text(x + 28, y + 14, FS_TITLE, focus ? draw::TEXT : draw::MUTED, who, true);
  /* Disconnect, where it is actually wanted. It was only in the menu
     behind Plus, which is a poor place for the one thing you look for
     while connected. */
  float right = x + w - 20;  // where the header's text has to stop
  if(server_side && g.connected) {
    const char *label = tr("Disconnect");
    const float pad = 18, bh = 44;
    const float bw = draw::text_size(FS_SMALL, label, true).x + pad * 2;
    const float bx = x + w - bw - 20, by = y + 16;
    right = bx - 16;
    const bool on = g.disconnect_focus && focus;
    draw::rect(bx, by, bw, bh, on ? draw::ERR : IM_COL32(86, 34, 42, 255), 12);
    draw::frame(bx, by, bw, bh, draw::ERR, 12, on ? 3 : 2);
    draw::text(bx + pad, by + (bh - FS_SMALL) / 2 - 2, FS_SMALL,
               on ? IM_COL32(20, 10, 12, 255) : draw::ERR, label, true);
    hit_add(HIT_DISCONNECT, bx - 8, by - 8, bw + 16, bh + 16);
  }
  std::string where;
  if(!server_side) where = p.path.empty() ? tr("Choose a drive") : p.path;
  else where = g.connected ? std::string(g.server.protocol == FTP_PROTO_SFTP ? "sftp://" : "ftp://") + g.server.host + "  " + p.path
                           : tr(device_text(N_("Connect to your laptop, PC or NAS"), N_("Connect to a server, NAS or console")));
  /* Loading: the spinner just left of Disconnect (or of the edge), and the
     path stops before it rather than running underneath. */
  if(p.loading) {
    draw::spinner(right - 14, y + 38, 14, draw::ACCENT);
    right -= 44;
  }
  float wx = x + 32 + draw::text_size(FS_TITLE, who, true).x + 20;
  draw::text_clip(wx, y + 22, FS_SMALL, draw::MUTED, where.c_str(), right - wx);
  draw::rect(x + 16, y + PANE_HEAD - 6, w - 32, 2, draw::LINE);

  float ly = y + PANE_HEAD;
  if(server_side && !g.connected) {
    draw_server_list(x, ly, w);
  } else {
    refresh_free_space(p);
    int rows = visible_rows();
    int n = (int)p.entries.size();
    if(p.cursor < p.scroll) p.scroll = p.cursor;
    if(p.cursor >= p.scroll + rows) p.scroll = p.cursor - rows + 1;
    if(p.scroll > std::max(0, n - rows)) p.scroll = std::max(0, n - rows);
    float yy = ly;
    if(!n && !p.loading) draw::text(x + 28, yy + 20, FS, draw::MUTED, p.path.empty() ? tr("No drives found") : tr("Empty folder"));
    hit_add(HIT_SCROLL, x, ly, w, (float)rows * ROW_H, idx, fm ? 1 : 0);
    for(int i = p.scroll; i < n && i < p.scroll + rows; i++) {
      Entry &e = p.entries[i];
      bool on = i == p.cursor;
      bool marked = p.marked.count(e.name) > 0;
      hit_add(HIT_ROW, x + 8, yy, w - 16, ROW_H, fm ? idx + 2 : idx, i);
      if(marked) draw::rect(x + 8, yy + 2, w - 16, ROW_H - 4, IM_COL32(30, 52, 40, 255), 10);
      if(on) draw::rect(x + 8, yy + 2, w - 16, ROW_H - 4, focus ? draw::SEL : draw::PANEL2, 10);
      if(marked) {
        draw::dl()->AddCircleFilled(ImVec2(x + 38, yy + ROW_H / 2), 13, draw::OK, 20);
        draw::dl()->AddLine(ImVec2(x + 32, yy + ROW_H / 2), ImVec2(x + 37, yy + ROW_H / 2 + 5), draw::BG, 3);
        draw::dl()->AddLine(ImVec2(x + 37, yy + ROW_H / 2 + 5), ImVec2(x + 45, yy + ROW_H / 2 - 5), draw::BG, 3);
      }
      float ix = x + 62;
      if(e.dir) draw::folder_icon(ix, yy + 12, 30, e.drive ? draw::ACCENT : draw::ACCENT2);
      else draw::file_icon(ix, yy + 12, 30, IM_COL32(150, 162, 186, 255));
      float nx = ix + 46;
      float right = x + w - 28;
      std::string size, date;
      if(e.drive) {
        size = e.size >= 0 ? trf("{0} free", {fmt_size(e.size)}) : "";
        date = e.detail;
      } else {
        size = e.dir ? "" : fmt_size(e.size);
        date = fmt_date(e.mtime);
      }
      float dw = 230, sw = 150;
      float name_w = right - nx - dw - sw - 20;
      std::string name = e.name + (e.link ? "  ->" : "");
      if(e.drive) {
        /* The label and the path share the name column: each keeps to its
           part, so a long label ("USB drive: AM8180 NVME, exFAT") is cut
           short instead of running under the path. */
        const float label_w = name_w * 0.62f - 16;
        draw::text_clip(nx, yy + 11, FS, draw::TEXT, e.detail.c_str(), label_w, on);
        draw::text_clip(nx + name_w * 0.62f, yy + 15, FS_SMALL, draw::MUTED, e.name.c_str(), name_w * 0.38f);
      } else {
        draw::text_clip(nx, yy + 11, FS, draw::TEXT, name.c_str(), name_w, on);
      }
      ImVec2 ss = draw::text_size(FS_SMALL, size.c_str());
      draw::text(right - dw - 20 - ss.x, yy + 15, FS_SMALL, draw::MUTED, size.c_str());
      if(!e.drive) draw::text(right - dw + 10, yy + 15, FS_SMALL, draw::MUTED, date.c_str());
      yy += ROW_H;
    }
    // Scrollbar
    if(n > rows) {
      float th = h - PANE_HEAD - PANE_FOOT - 10;
      float bar = std::max(40.0f, th * rows / n);
      float by = ly + (th - bar) * p.scroll / std::max(1, n - rows);
      draw::rect(x + w - 10, by, 5, bar, draw::LINE, 3);
    }
  }

  // Footer summary
  char foot[200] = "";
  if(server_side && !g.connected) {
    snprintf(foot, sizeof(foot), "%s", trf("{0} saved  ·  {1} recent", {std::to_string(g.sites.size()), std::to_string(g.history.size())}).c_str());
  } else if(!p.path.empty()) {
    int files = 0, dirs = 0;
    long long bytes = 0, mbytes = 0;
    for(auto &e : p.entries) {
      if(e.dir) dirs++;
      else {
        files++;
        bytes += std::max(0LL, e.size);
        if(p.marked.count(e.name)) mbytes += std::max(0LL, e.size);
      }
    }
    if(!p.marked.empty()) snprintf(foot, sizeof(foot), "%s", trf("{0} marked  ·  {1}", {std::to_string(p.marked.size()), fmt_size(mbytes)}).c_str());
    else snprintf(foot, sizeof(foot), "%s", trf("Files: {0}  ·  Folders: {1}  ·  {2}", {std::to_string(files), std::to_string(dirs), fmt_size(bytes)}).c_str());
    if(!server_side && p.free_bytes >= 0) {
      size_t used = strlen(foot);
      snprintf(foot + used, sizeof(foot) - used, "  ·  %s",
               trf("{0} free", {fmt_size(p.free_bytes)}).c_str());
    }
  }
  draw::text(x + 28, y + h - PANE_FOOT + 8, FS_SMALL, p.marked.empty() ? draw::MUTED : draw::OK, foot);
}

void draw_top() {
  draw::rect(0, 0, W, TOP_H, draw::PANEL2);
  draw::rect(0, TOP_H - 2, W, 2, draw::LINE);
  // The brand lockup: mark, then the wordmark (assets/brand/BRAND.md).
  draw::brand_mark(MARGIN, 16, 64);
  draw::brand_wordmark(MARGIN + 80, 22, 40);

  // Only while transfers run: how many, and how fast.
  if(!g.qs.running && !g.qs.queued) return;
  char right[200];
  snprintf(right, sizeof(right), "%s%s%s", g.qs.paused ? (std::string(tr("Paused")) + "  ·  ").c_str() : "",
           trf("{0} active  ·  {1} queued", {std::to_string(g.qs.running), std::to_string(g.qs.queued)}).c_str(),
           g.qs.speed > 0 ? ("  ·  " + fmt_size((long long)g.qs.speed) + "/s").c_str() : "");
  ImVec2 rs = draw::text_size(FS_SMALL, right);
  float rx = W - MARGIN - rs.x;
  draw::text(rx, 34, FS_SMALL, g.qs.running ? draw::ACCENT2 : draw::MUTED, right);
  if(g.qs.running) draw::spinner(rx - 30, 48, 12, draw::ACCENT2);
}

void draw_bottom() {
  float sy = H - HINT_H - STATUS_H;
  const std::string &msg = now() < g.toast_until ? g.toast : g.status;
  bool err = now() < g.toast_until ? g.toast_err : g.status_err;
  if(!msg.empty()) draw::text_clip(MARGIN + 8, sy + 8, FS_SMALL, err ? draw::ERR : draw::MUTED, msg.c_str(), W - 2 * MARGIN);

  float hy = H - HINT_H;
  draw::rect(0, hy, W, HINT_H, draw::PANEL2);
  float x = MARGIN, y = hy + (HINT_H - 38) / 2;
  if(!g.overlays.empty()) {
    if(g.overlays.back()->kind() == OV_QUEUE) {
      int tab = static_cast<QueueView *>(g.overlays.back().get())->tab;
      x += hint(x, y, ACT_CROSS, tr("Options"));
      x += hint(x, y, ACT_TRIANGLE, g.qs.paused ? tr("Resume") : tr("Pause"));
      x += hint(x, y, ACT_SQUARE, tab == 0 ? tr("Cancel all") : tab == 1 ? tr("Retry all") : tr("Clear list"));
      x += hint(x, y, ACT_L1, tr("Tabs"));
      hint(x, y, ACT_CIRCLE, tr("Close"));
    } else if(g.overlays.back()->kind() != OV_LOG) {
      x += hint(x, y, ACT_CROSS, tr("Select"));
      x += hint(x, y, ACT_LEFT, tr("Change"));
      hint(x, y, ACT_CIRCLE, tr("Back"));
    }
    return;
  }
  Pane &p = active();
  if(p.remote && !g.connected) {
    x += hint(x, y, ACT_CROSS, tr("Connect"));
    x += hint(x, y, ACT_TRIANGLE, tr("Edit"));
    x += hint(x, y, ACT_SQUARE, tr("Delete"));
    x += hint(x, y, ACT_CIRCLE, tr("Start screen"));
  } else {
    bool on_dir = p.cursor < (int)p.entries.size() && p.entries[p.cursor].dir;
    x += hint(x, y, ACT_CROSS, on_dir ? tr("Open") : tr("Actions"));
    x += hint(x, y, ACT_SQUARE, tr("Mark"));
    if(g.connected && !p.path.empty()) {
      char t[64];
      size_t n = p.marked.empty() ? 1 : p.marked.size();
      snprintf(t, sizeof(t), "%s", trf(p.remote ? N_("Download {0}") : N_("Upload {0}"), {std::to_string(n)}).c_str());
      x += hint(x, y, ACT_TRIANGLE, t);
    }
    x += hint(x, y, ACT_CIRCLE, !p.remote && p.path.empty() ? tr("Start screen") : tr("Back"));
    if(!(!p.remote && p.path.empty())) {
      x += hint(x, y, ACT_CIRCLE_LONG, tr("Hold: start screen"));
    }
  }
  x += hint_lr(x, y, tr("Switch side"));
  x += hint(x, y, ACT_OPTIONS, tr("Menu"));
  hint(x, y, ACT_TOUCHPAD, tr("Transfers"));
}

/* ---------- log & queue polling ---------- */

void on_log(void *ctx, unsigned long seq, long long t, int conn, log_kind_t kind, const char *text) {
  (void)ctx;
  (void)seq;
  (void)t;
  char buf[600];
  snprintf(buf, sizeof(buf), "%s%s", conn ? ("#" + std::to_string(conn) + "  ").c_str() : "", text);
  g.log.push_back({kind, buf});
  if(g.log.size() > 800) g.log.pop_front();
  if(kind == LOG_STATUS || kind == LOG_ERROR) {
    g.status = buf;
    g.status_err = kind == LOG_ERROR;
  }
}

/* ---------- start screen and the two servers ---------- */

// Server settings, kept in servers.json.
struct ServerPrefs {
  ftpd_config_t ftp;
  int web_port = WEB_DEFAULT_PORT;
  /* Where the platform has an SFTP server (CHIRAL_HAS_SFTPD): which of the
     two the server screen runs, SFTP unless chosen otherwise, and SFTP's
     own port. The login and the shared folder are the same for both. */
  bool sftp = true;
  int sftp_port = SFTPD_PORT;
  /* A computer's web app opens in its own browser, so by default nothing
     else on the network can reach it (websrv_set_local_only). */
  bool web_local = true;
  /* The shared folder was picked on the server screen. Until it is, the
     platform's default applies (system_ftpd_default_root), and follows it
     when that changes -- a root saved only because it was the default
     isn't anyone's choice. */
  bool root_chosen = false;
};

ServerPrefs g_srv;

void save_server_prefs();  // further down

#if CHIRAL_DEVICE_COMPUTER || CHIRAL_UI_MOBILE  // the desktop pages' server page, on a phone too
/* Shares what the server screen was told to: a folder, or the whole
   computer (system_ftpd_full_root). */
void set_shared_root(const std::string &v) {
  snprintf(g_srv.ftp.root, sizeof(g_srv.ftp.root), "%s", v.c_str());
  g_srv.root_chosen = true;
  save_server_prefs();
}

void share_whole_computer() {
  confirm(tr("Share the whole computer?"),
          tr("Anyone with the server's login could read, change and delete every file this account can reach, not just one folder. chiral-ftp's own settings stay hidden."),
          tr("Share everything"), [] {
            set_shared_root(system_ftpd_full_root());
            show_toast(tr("Sharing the whole computer"));
          });
}
#endif

/* The server screen's server. Where there is no SFTP server, the FTP
   server, and these are just its own calls. */
bool server_is_sftp() {
#if CHIRAL_HAS_SFTPD
  return g_srv.sftp;
#else
  return false;
#endif
}

bool server_running() {
#if CHIRAL_HAS_SFTPD
  if(g_srv.sftp) return sftpd_running();
#endif
  return ftpd_running();
}

void server_stats(ftpd_stats_t *st) {
#if CHIRAL_HAS_SFTPD
  if(g_srv.sftp) return sftpd_get_stats(st);
#endif
  ftpd_get_stats(st);
}

int server_recent(char lines[][160], int max) {
#if CHIRAL_HAS_SFTPD
  if(g_srv.sftp) return sftpd_recent(lines, max);
#endif
  return ftpd_recent(lines, max);
}

unsigned short server_port() {
  return server_is_sftp() ? (unsigned short)g_srv.sftp_port : g_srv.ftp.port;
}

std::string g_ip = "unknown";
double g_ip_at = -100;

void load_server_prefs() {
  ftpd_config_default(&g_srv.ftp);
  snprintf(g_srv.ftp.root, sizeof(g_srv.ftp.root), "%s", system_ftpd_default_root());
  std::vector<json::Object> v = load_store("servers");
  if(v.empty()) return;
  json::Object &o = v[0];
  int port = (int)json::get_num(o, "ftp_port", FTPD_DEFAULT_PORT);
  if(port > 0 && port < 65536) g_srv.ftp.port = (unsigned short)port;
  std::string user = json::get_str(o, "ftp_user", FTPD_DEFAULT_USER);
  std::string pass = json::get_str(o, "ftp_pass", FTPD_DEFAULT_PASS);
  snprintf(g_srv.ftp.user, sizeof(g_srv.ftp.user), "%s", user.c_str());
  snprintf(g_srv.ftp.pass, sizeof(g_srv.ftp.pass), "%s", pass.c_str());
  g_srv.ftp.anonymous = json::get_bool(o, "ftp_anonymous", false);
  port = (int)json::get_num(o, "web_port", WEB_DEFAULT_PORT);
  if(port > 0 && port < 65536) g_srv.web_port = port;
#if CHIRAL_HAS_SFTPD
  g_srv.sftp = json::get_str(o, "server_protocol", "sftp") != "ftp";
  port = (int)json::get_num(o, "sftp_port", SFTPD_PORT);
  if(port > 0 && port < 65536) g_srv.sftp_port = port;
#endif
#if CHIRAL_DEVICE_COMPUTER
  g_srv.web_local = json::get_bool(o, "web_local", true);
  {
    /* A folder chosen on the server screen, while it is still there. */
    std::string root = json::get_str(o, "ftp_root");
    struct stat st;
    if(json::get_bool(o, "ftp_root_chosen", false) && !root.empty() && !stat(root.c_str(), &st) && S_ISDIR(st.st_mode)) {
      snprintf(g_srv.ftp.root, sizeof(g_srv.ftp.root), "%s", root.c_str());
      g_srv.root_chosen = true;
    }
  }
#endif
}

void save_server_prefs() {
  json::Object o;
  json::set_num(o, "ftp_port", g_srv.ftp.port);
  json::set_str(o, "ftp_user", g_srv.ftp.user);
  json::set_str(o, "ftp_pass", g_srv.ftp.pass);
  json::set_bool(o, "ftp_anonymous", g_srv.ftp.anonymous);
  json::set_num(o, "web_port", g_srv.web_port);
#if CHIRAL_HAS_SFTPD
  json::set_str(o, "server_protocol", g_srv.sftp ? "sftp" : "ftp");
  json::set_num(o, "sftp_port", g_srv.sftp_port);
#endif
#if CHIRAL_DEVICE_COMPUTER
  json::set_bool(o, "web_local", g_srv.web_local);
  json::set_str(o, "ftp_root", g_srv.ftp.root);
  json::set_bool(o, "ftp_root_chosen", g_srv.root_chosen);
#endif
  save_store("servers", {o});
  ftpd_set_config(&g_srv.ftp);
#if CHIRAL_HAS_SFTPD
  {
    ftpd_config_t c = g_srv.ftp;
    c.port = (unsigned short)g_srv.sftp_port;
    sftpd_set_config(&c);
  }
#endif
}

/* The address to show for the servers. Where the machine is on several
   networks (system_networks), the one chosen in Settings; else, or when
   that one has gone, the first on a 192.168 network -- the usual home or
   office LAN, where the console or phone that wants to connect is. A VPN
   tends to be the route to the internet, which is what the platform's own
   answer goes by, and nobody across the room can reach it. */
std::string pick_ip() {
  system_net_t nets[16];
  const int n = system_networks(nets, 16);
  if(!g.network.empty()) {
    for(int i = 0; i < n; i++) if(g.network == nets[i].name) return nets[i].addr;
  }
  for(int i = 0; i < n; i++) if(!strncmp(nets[i].addr, "192.168.", 8)) return nets[i].addr;
  char buf[64];
  net_local_ip(buf, sizeof(buf));
  return buf;
}

// The IP can change (Wi-Fi reconnects), so look again every few seconds.
const std::string &local_ip() {
  if(now() - g_ip_at > 5) {
    g_ip = pick_ip();
    g_ip_at = now();
  }
  return g_ip;
}

void ip_changed() {
  g_ip_at = -100;  // look again on the next frame
}

std::string server_address() {
  return std::string(server_is_sftp() ? "sftp://" : "ftp://") + local_ip() + ":" + std::to_string(server_port());
}

// Only this computer's own browser: the loopback address, which needs no network.
bool web_local() {
#if CHIRAL_DEVICE_COMPUTER
  return g_srv.web_local;
#else
  return false;
#endif
}

std::string web_address() {
  return "http://" + (web_local() ? std::string("127.0.0.1") : local_ip()) + ":" + std::to_string(g_srv.web_port) + "/";
}

void start_server() {
#if CHIRAL_HAS_SFTPD
  if(g_srv.sftp) {
    ftpd_config_t c = g_srv.ftp;
    c.port = (unsigned short)g_srv.sftp_port;
    if(sftpd_start(&c)) {
      show_toast(errno == EADDRINUSE ? trf("Could not start the SFTP server: {0} (port in use, choose another one)", {strerror(errno)})
                                     : trf("Could not start the SFTP server: {0}", {strerror(errno)}), true);
    } else {
      show_toast(trf("SFTP server running at {0}", {server_address()}));
    }
    return;
  }
#endif
  if(ftpd_start(&g_srv.ftp)) {
    show_toast(errno == EADDRINUSE ? trf("Could not start the FTP server: {0} (port in use, choose another one)", {strerror(errno)})
                                   : trf("Could not start the FTP server: {0}", {strerror(errno)}), true);
  } else {
    show_toast(trf("FTP server running at {0}", {server_address()}));
  }
}

void stop_server() {
#if CHIRAL_HAS_SFTPD
  if(g_srv.sftp) return sftpd_stop();
#endif
  ftpd_stop();
}

void start_web(bool open_browser = true) {
  websrv_set_local_only(web_local());
  if(websrv_start((unsigned short)g_srv.web_port)) {
    show_toast(errno == EADDRINUSE ? trf("Could not start the web app: {0} (port in use, choose another one)", {strerror(errno)})
                                   : trf("Could not start the web app: {0}", {strerror(errno)}), true);
  } else {
    show_toast(trf("Web app running at {0}", {web_address()}));
#if CHIRAL_DEVICE_COMPUTER
    /* A computer has its own browser: that's where the web app is used. */
    if(open_browser) platform::open_url(web_address().c_str());
#else
    (void)open_browser;
#endif
  }
}

void prompt_port(const std::string &title, int current, std::function<void(int)> done) {
  prompt(title, std::to_string(current), TEXT_NUMBER, [done](std::string v) {
    int p = atoi(v.c_str());
    if(p < 1 || p > 65535) {
      show_toast(tr("A port is a number from 1 to 65535"), true);
      return;
    }
    done(p);
  });
}

// A big panel with one setting per row; the FTP and web screens use it.
struct Row {
  std::string label, value;
  std::function<void()> run;
  bool primary = false, enabled = true;
};

int g_home_cursor = 0;
bool g_home_quick = false;  // the start screen's focus is on Quick connect, above the cards
int g_row_cursor = 0;

void draw_rows(const std::vector<Row> &rows, float x, float y, float w, float row = 70) {
  for(int i = 0; i < (int)rows.size(); i++) {
    const Row &r = rows[i];
    bool on = i == g_row_cursor && g.overlays.empty();
    float yy = y + i * row;
    if(r.enabled && r.run) hit_add(HIT_SETTING, x + 20, yy + 4, w - 40, row - 10, i);
    if(r.primary) {
      draw::rect(x + 20, yy + 4, w - 40, row - 10, on ? draw::ACCENT : draw::PANEL2, 12);
      ImVec2 ts = draw::text_size(FS, r.label.c_str(), true);
      draw::text(x + (w - ts.x) / 2, yy + (row - 32) / 2, FS, draw::TEXT, r.label.c_str(), true);
      continue;
    }
    if(on) draw::rect(x + 20, yy + 4, w - 40, row - 10, draw::SEL, 10);
    draw::text(x + 44, yy + (row - 32) / 2, FS, r.enabled ? draw::TEXT : draw::MUTED, r.label.c_str(), on);
    ImVec2 ts = draw::text_size(FS, r.value.c_str());
    draw::text_clip(std::max(x + w * 0.45f, x + w - 60 - ts.x), yy + (row - 32) / 2, FS, draw::ACCENT2, r.value.c_str(), w * 0.5f);
  }
}

bool rows_input(std::vector<Row> rows, Action a) {
  int n = (int)rows.size();
  if(g_row_cursor >= n) g_row_cursor = n - 1;
  if(a == ACT_UP) g_row_cursor = (g_row_cursor + n - 1) % n;
  if(a == ACT_DOWN) g_row_cursor = (g_row_cursor + 1) % n;
  if(a == ACT_CROSS && rows[g_row_cursor].enabled && rows[g_row_cursor].run) {
    rows[g_row_cursor].run();
    return true;
  }
  return a == ACT_UP || a == ACT_DOWN;
}

void go_mode(Mode m) {
  g.mode = m;
  g_row_cursor = 0;
}

#if CHIRAL_DEVICE_COMPUTER || CHIRAL_UI_MOBILE
/* A folder as the server screen shows it: the home folder as "~". */
std::string short_path(const std::string &path) {
  const char *home = getenv("HOME");
  const size_t n = home ? strlen(home) : 0;
  if(n > 1 && !path.compare(0, n, home) && (path.size() == n || path[n] == '/')) return "~" + path.substr(n);
  return path;
}
#endif

std::vector<Row> server_rows() {
  std::vector<Row> rows;
  const bool running = server_running(), sftp = server_is_sftp();
  rows.push_back({running ? (sftp ? tr("Stop the SFTP server") : tr("Stop the FTP server"))
                          : (sftp ? tr("Start the SFTP server") : tr("Start the FTP server")),
                  "", [running] { if(running) stop_server(); else start_server(); }, true});
#if CHIRAL_HAS_SFTPD
  /* One server at a time: SFTP, encrypted, unless something can only speak
     FTP. Changed while stopped, like the port. */
  rows.push_back({tr("Protocol"), running ? trf("{0}  (stop to change)", {sftp ? "SFTP" : "FTP"})
                                          : std::string("<  ") + (sftp ? "SFTP" : "FTP") + "  >",
                  [] {
                    g_srv.sftp = !g_srv.sftp;
                    save_server_prefs();
                  },
                  false, !running});
#endif
  rows.push_back({tr("Username"), g_srv.ftp.user, [sftp] {
    prompt(sftp ? tr("Username") : tr("FTP username"), g_srv.ftp.user, TEXT_DEFAULT, [](std::string v) {
      if(v.empty()) return show_toast(tr("The username can't be empty"), true);
      snprintf(g_srv.ftp.user, sizeof(g_srv.ftp.user), "%s", v.c_str());
      save_server_prefs();
      show_toast(tr("Username saved; new logins use it"));
    });
  }});
  rows.push_back({tr("Password"), g_srv.ftp.pass, [sftp] {
    prompt(sftp ? tr("Password") : tr("FTP password"), g_srv.ftp.pass, TEXT_DEFAULT, [](std::string v) {
      if(v.empty()) return show_toast(tr("The password can't be empty; turn on anonymous login instead"), true);
      snprintf(g_srv.ftp.pass, sizeof(g_srv.ftp.pass), "%s", v.c_str());
      save_server_prefs();
      show_toast(tr("Password saved; new logins use it"));
    });
  }});
  if(!sftp) {  // SSH has no anonymous login
    rows.push_back({tr("Anonymous login"), g_srv.ftp.anonymous ? tr("On") : tr("Off"), [] {
      g_srv.ftp.anonymous = !g_srv.ftp.anonymous;
      save_server_prefs();
    }});
  }
  const std::string port = std::to_string(server_port());
  rows.push_back({tr("Port"), running ? trf("{0}  (stop to change)", {port}) : port, [sftp] {
    prompt_port(sftp ? tr("SFTP server port") : tr("FTP server port"), server_port(), [sftp](int p) {
      if(sftp) g_srv.sftp_port = p;
      else g_srv.ftp.port = (unsigned short)p;
      save_server_prefs();
    });
  }, false, !running});
#if CHIRAL_DEVICE_COMPUTER
  /* A console shares the whole of itself; a computer, the one folder chosen
     here (the person's own to start with: system_ftpd_default_root), or
     all of it when asked to. */
  rows.push_back({tr("Shared folder"), short_path(g_srv.ftp.root), [] {
    prompt(tr("Shared folder"), short_path(g_srv.ftp.root), TEXT_URL, [](std::string v) {
      const char *home = getenv("HOME");
      if(!v.empty() && v[0] == '~' && home) v = home + v.substr(1);
      while(v.size() > 1 && v.back() == '/') v.pop_back();
      struct stat st;
      if(v.empty() || v[0] != '/' || stat(v.c_str(), &st) || !S_ISDIR(st.st_mode)) {
        return show_toast(trf("Not a folder: {0}", {v}), true);
      }
      set_shared_root(v);
      show_toast(trf("Sharing {0}", {short_path(v)}));
    });
  }});
  rows.push_back({tr("Whole computer"), "", [] { share_whole_computer(); }});
#endif
  rows.push_back({trf("Reset to {0} / {1}", {FTPD_DEFAULT_USER, FTPD_DEFAULT_PASS}), "", [] {
    confirm(tr("Reset login"), trf("Set the username back to \"{0}\" and the password to \"{1}\"?", {FTPD_DEFAULT_USER, FTPD_DEFAULT_PASS}),
            tr("Reset"), [] {
      snprintf(g_srv.ftp.user, sizeof(g_srv.ftp.user), "%s", FTPD_DEFAULT_USER);
      snprintf(g_srv.ftp.pass, sizeof(g_srv.ftp.pass), "%s", FTPD_DEFAULT_PASS);
      save_server_prefs();
    });
  }});
  rows.push_back({tr("Back to start screen"), "", [] { go_mode(MODE_HOME); }});
  return rows;
}

std::vector<Row> web_rows() {
  std::vector<Row> rows;
  bool running = websrv_running();
  rows.push_back({running ? tr("Stop the web app") : tr("Start the web app"), "",
                  [running] { if(running) websrv_stop(); else start_web(); }, true});
#if CHIRAL_DEVICE_COMPUTER
  if(running) rows.push_back({tr("Open in your browser"), "", [] { platform::open_url(web_address().c_str()); }});
  {
    const std::string reach = g_srv.web_local ? tr("This computer only") : tr("Your home network");
    rows.push_back({tr("Reachable from"), running ? trf("{0}  (stop to change)", {reach}) : "<  " + reach + "  >",
                    [] {
                      g_srv.web_local = !g_srv.web_local;
                      save_server_prefs();
                    },
                    false, !running});
  }
#endif
  rows.push_back({tr("Port"), running ? trf("{0}  (stop to change)", {std::to_string(g_srv.web_port)}) : std::to_string(g_srv.web_port), [] {
    prompt_port(tr("Web app port"), g_srv.web_port, [](int p) {
      g_srv.web_port = p;
      save_server_prefs();
    });
  }, false, !running});
  rows.push_back({tr("Back to start screen"), "", [] { go_mode(MODE_HOME); }});
  return rows;
}

void draw_header(const char *title, const char *subtitle) {
  draw::rect(0, 0, W, H, draw::BG);
  draw_top();
  draw::text(MARGIN + 8, TOP_H + 40, 48, draw::TEXT, title, true);
  draw::text(MARGIN + 8, TOP_H + 110, FS, draw::MUTED, subtitle);
}

void draw_status_pill(float x, float y, bool running, const char *on, const char *off) {
  const char *t = running ? on : off;
  ImVec2 ts = draw::text_size(FS_SMALL, t, true);
  draw::rect(x, y, ts.x + 60, 46, running ? IM_COL32(24, 64, 44, 255) : draw::PANEL2, 23);
  draw::dl()->AddCircleFilled(ImVec2(x + 25, y + 23), 8, running ? draw::OK : draw::MUTED, 12);
  draw::text(x + 42, y + 10, FS_SMALL, running ? draw::OK : draw::MUTED, t, true);
}

void draw_hints(std::initializer_list<std::pair<Action, const char *>> hints) {
  const std::string &msg = now() < g.toast_until ? g.toast : std::string();
  float sy = H - HINT_H - STATUS_H;
  if(!msg.empty()) draw::text_clip(MARGIN + 8, sy + 8, FS_SMALL, g.toast_err ? draw::ERR : draw::MUTED, msg.c_str(), W - 2 * MARGIN);
  float hy = H - HINT_H;
  draw::rect(0, hy, W, HINT_H, draw::PANEL2);
  float x = MARGIN, y = hy + (HINT_H - 38) / 2;
  for(auto &h : hints) x += hint(x, y, h.first, h.second);
}

void draw_ftpd() {
  const bool running = server_running(), sftp = server_is_sftp();
#if CHIRAL_HAS_SFTPD
  draw_header(tr("FTP / SFTP server"),
              tr(device_text(N_("Other devices connect to the console with FileZilla, a file manager or any FTP app."),
                             N_("Other devices connect to this computer with FileZilla, Cyberduck, a file manager or any SFTP or FTP app."))));
#else
  draw_header(tr("FTP server"), tr(device_text(N_("Other devices connect to the console with FileZilla, a file manager or any FTP app."),
                                               N_("Other devices connect to this computer with FileZilla, a file manager or any FTP app."))));
#endif
  float x = MARGIN, y = TOP_H + 180, w = 860, h = H - y - HINT_H - STATUS_H - 20;
  draw::rect(x, y, w, h, draw::PANEL, 16);
  draw_status_pill(x + 30, y + 30, running, tr("Running"), tr("Stopped"));
  std::string addr = server_address();
  draw::text_clip(x + 30, y + 100, 46, running ? draw::TEXT : draw::MUTED, addr.c_str(), w - 60, true);
  std::string login = g_srv.ftp.anonymous && !sftp ? trf("Login: {0} / {1}   or anonymous", {g_srv.ftp.user, g_srv.ftp.pass})
                                                   : trf("Login: {0} / {1}", {g_srv.ftp.user, g_srv.ftp.pass});
  float rows_y = y + 232, row_h = 60;
#if CHIRAL_HAS_SFTPD
  /* Two more rows than a console's (protocol, shared folder), and for SFTP
     the key an app asks you to trust the first time it connects. */
  row_h = 54;
  draw::text_clip(x + 30, y + 160, FS, draw::ACCENT2, login.c_str(), w - 60);
  char fp[128] = "";
  if(sftp) sftpd_fingerprint(fp, sizeof(fp));
  if(fp[0]) draw::text_clip(x + 30, y + 198, FS_SMALL, draw::MUTED, trf("Key: {0}", {fp}).c_str(), w - 60);
  draw::rect(x + 20, y + 236, w - 40, 2, draw::LINE);
  rows_y = y + 244;
#else
  draw::text_clip(x + 30, y + 170, FS, draw::ACCENT2, login.c_str(), w - 60);
  draw::rect(x + 20, y + 222, w - 40, 2, draw::LINE);
#endif
  draw_rows(server_rows(), x, rows_y, w, row_h);

  float rx = x + w + PANE_GAP, rw = W - MARGIN - rx;
  draw::rect(rx, y, rw, h, draw::PANEL, 16);
  ftpd_stats_t st;
  server_stats(&st);
  char line[200];
  draw::text(rx + 30, y + 28, FS_TITLE, draw::TEXT, tr("Activity"), true);
  snprintf(line, sizeof(line), "%s%s", trf("Connected: {0}  ·  Transfers: {1}", {std::to_string(st.clients), std::to_string(st.transfers)}).c_str(),
           st.transfers && st.speed > 0 ? ("  ·  " + fmt_size((long long)st.speed) + "/s").c_str() : "");
  draw::text(rx + 30, y + 84, FS, st.transfers ? draw::ACCENT2 : draw::MUTED, line);
  snprintf(line, sizeof(line), "%s", trf("Sent {0}  ·  Received {1}", {fmt_size(st.bytes_up), fmt_size(st.bytes_down)}).c_str());
  draw::text(rx + 30, y + 128, FS_SMALL, draw::MUTED, line);
  draw::rect(rx + 20, y + 175, rw - 40, 2, draw::LINE);
  char lines[16][160];
  int n = server_recent(lines, 16);
  float yy = y + 192;
  if(!n) draw::text(rx + 30, yy, FS_SMALL, draw::MUTED, tr("Logins and transfers show here."));
  for(int i = 0; i < n && yy < y + h - 40; i++, yy += 36) draw::text_clip(rx + 30, yy, FS_SMALL, draw::TEXT, lines[i], rw - 60);
  draw_hints({{ACT_CROSS, tr("Select")}, {ACT_CIRCLE, tr("Start screen (the server keeps running)")}, {ACT_OPTIONS, tr("Menu")}});
}

void draw_web() {
  bool running = websrv_running();
  float x = MARGIN, y = TOP_H + 180, w = W - 2 * MARGIN, h = H - y - HINT_H - STATUS_H - 20;
#if CHIRAL_DEVICE_COMPUTER
  /* On a computer the web app is where its file manager and client live,
     in its own browser -- opened for you when it starts. */
  const bool local = web_local();
  draw_header(tr("Web app"), tr("The file manager and the FTP / SFTP client, in your browser."));
  draw::rect(x, y, w, h, draw::PANEL, 16);
  draw_status_pill(x + 30, y + 30, running, tr("Running"), tr("Stopped"));
  draw::text(x + 30, y + 100, FS, draw::MUTED,
             local ? (running ? tr("Open in your browser at:") : tr("Start it to open it in your browser:"))
                   : (running ? tr("Open it in a browser here or on another device:")
                              : tr("Start it, then open it in a browser here or on another device:")));
  draw::text_clip(x + 30, y + 150, 64, running ? draw::TEXT : draw::MUTED, web_address().c_str(), w - 60, true);
  draw::text_clip(x + 30, y + 236, FS_SMALL, draw::TEXT,
                  tr("Files: this computer's files. Browse, upload and download (drag and drop works), copy, move, rename, delete."),
                  w - 60);
  draw::text_clip(x + 30, y + 268, FS_SMALL, draw::MUTED,
                  local ? tr("FTP / SFTP client: connect to servers and transfer files, with this window's connections and queue.")
                        : tr("FTP / SFTP client: connect to servers and transfer files. Anyone on your network can open the page while it runs."),
                  w - 60);
#else
  draw_header(tr("Web app"), tr(device_text(N_("Use chiral-ftp from a browser on your PC or phone."),
                                            N_("Use chiral-ftp from a browser on a phone, a tablet or another computer."))));
  draw::rect(x, y, w, h, draw::PANEL, 16);
  draw_status_pill(x + 30, y + 30, running, tr("Running"), tr("Stopped"));
  draw::text(x + 30, y + 100, FS, draw::MUTED,
             running ? tr(device_text(N_("Open this address in a browser on your PC:"), N_("Open this address in a browser on another device:")))
                     : tr(device_text(N_("Start it, then open this address in a browser on your PC:"),
                                      N_("Start it, then open this address in a browser on another device:"))));
  draw::text_clip(x + 30, y + 150, 64, running ? draw::TEXT : draw::MUTED, web_address().c_str(), w - 60, true);
  draw::text_clip(x + 30, y + 236, FS_SMALL, draw::TEXT,
                  tr(device_text(N_("Console files: browse, download to your PC, upload from it (drag and drop works), copy, move, rename, delete."),
                                 N_("This computer's files: browse, download to the other device, upload from it (drag and drop works), copy, move, rename, delete."))),
                  w - 60);
  draw::text_clip(x + 30, y + 268, FS_SMALL, draw::MUTED,
                  tr(device_text(N_("FTP client tab: the client from the TV, sharing its connection and queue. Anyone on your network can open the page while it runs."),
                                 N_("FTP client tab: the same client as this window, sharing its connection and queue. Anyone on your network can open the page while it runs."))),
                  w - 60);
#endif
  draw::rect(x + 20, y + 314, w - 40, 2, draw::LINE);
  float rw = 900;
  draw_rows(web_rows(), x, y + 330, rw);
  draw_hints({{ACT_CROSS, tr("Select")}, {ACT_CIRCLE, tr("Start screen (the web app keeps running)")}, {ACT_OPTIONS, tr("Menu")}});
}

/* ---------- quick connect ---------- */

/* The other chiral-ftp devices on the network (engine/discovery.h). One
   asks another to share; the person there allows it; this side connects
   with a login that only works from here, and opens the client. */

const char *kind_label(const char *kind) {
  if(!strcmp(kind, "ps5")) return "PS5";
  if(!strcmp(kind, "switch")) return "Nintendo Switch";
  if(!strcmp(kind, "mac")) return "Mac";
  if(!strcmp(kind, "windows")) return "Windows";
  if(!strcmp(kind, "linux")) return "Linux";
  if(!strcmp(kind, "android")) return "Android";
  if(!strcmp(kind, "androidtv")) return "Android TV";
  return kind;
}

const char *kind_badge(const char *kind) {
  if(!strcmp(kind, "ps5")) return "PS5";
  if(!strcmp(kind, "switch")) return "NX";
  if(!strcmp(kind, "mac")) return "Mac";
  return "PC";
}

ImU32 kind_tint(const char *kind) {
  if(!strcmp(kind, "switch")) return IM_COL32(0x3A, 0x14, 0x16, 255);
  if(!strcmp(kind, "mac")) return IM_COL32(0x2A, 0x24, 0x16, 255);
  if(!strncmp(kind, "android", 7)) return IM_COL32(0x14, 0x34, 0x22, 255);
  return IM_COL32(0x14, 0x32, 0x5C, 255);
}

ImU32 kind_ink(const char *kind) {
  if(!strcmp(kind, "switch")) return IM_COL32(0xFF, 0x8A, 0x8A, 255);
  if(!strcmp(kind, "mac")) return IM_COL32(0xF2, 0xD9, 0x8A, 255);
  if(!strncmp(kind, "android", 7)) return IM_COL32(0x8A, 0xE6, 0xA8, 255);
  return IM_COL32(0xA9, 0xCE, 0xFF, 255);
}

/* What kind of device, in an s x s square: its badge, or for Android a
   phone or a TV drawn in outline. */
void kind_square(float x, float y, float s, const char *kind) {
  draw::rect(x, y, s, s, kind_tint(kind), s * 0.22f);
  const ImU32 ink = kind_ink(kind);
  const float t = std::max(1.5f, s * 0.06f);  // the outline
  ImDrawList *d = draw::dl();
  if(!strcmp(kind, "android")) {
    const float w = s * 0.36f, h = s * 0.60f, px = x + (s - w) / 2, py = y + (s - h) / 2;
    d->AddRect(ImVec2(px, py), ImVec2(px + w, py + h), ink, s * 0.08f, 0, t);
    d->AddRectFilled(ImVec2(x + s * 0.43f, py + h - s * 0.13f), ImVec2(x + s * 0.57f, py + h - s * 0.13f + t), ink, t / 2);
    return;
  }
  if(!strcmp(kind, "androidtv")) {
    const float w = s * 0.64f, h = s * 0.42f, px = x + (s - w) / 2, py = y + s * 0.22f;
    d->AddRect(ImVec2(px, py), ImVec2(px + w, py + h), ink, s * 0.05f, 0, t);
    d->AddRectFilled(ImVec2(x + s * 0.47f, py + h), ImVec2(x + s * 0.53f, py + h + s * 0.08f), ink);
    d->AddRectFilled(ImVec2(x + s * 0.34f, py + h + s * 0.08f), ImVec2(x + s * 0.66f, py + h + s * 0.08f + t), ink, t / 2);
    return;
  }
  const char *b = kind_badge(kind);
  const float fs = s * 0.36f;
  ImVec2 ts = draw::text_size(fs, b, true);
  draw::text(x + (s - ts.x) / 2, y + (s - ts.y) / 2, fs, ink, b, true);
}

// The four digits both screens show, one to a box.
void code_boxes(float cx, float y, float box, const char *code, ImU32 bg) {
  const float gap = box * 0.2f, total = 4 * box + 3 * gap;
  for(int i = 0; i < 4 && code[i]; i++) {
    const float bx = cx - total / 2 + i * (box + gap);
    draw::rect(bx, y, box, box * 1.2f, bg, box * 0.16f);
    char d[2] = {code[i], 0};
    const float fs = box * 0.68f;
    ImVec2 ts = draw::text_size(fs, d, true);
    draw::text(bx + (box - ts.x) / 2, y + (box * 1.2f - ts.y) / 2, fs, draw::ACCENT2, d, true);
  }
}

/* Where to get chiral-ftp for the other devices, as Quick connect says it:
   the download page, and the same page as a QR code for a phone's camera. */
const char *const kDownloadPage = "chiral-ftp.com/download";
const char *const kReleasesPage = "github.com/Herbrax/Chiral-FTP/releases";

// https://chiral-ftp.com/download: QR 2-L, 25 x 25 (tools/gen-qr.py)
const int kDownloadQrSize = 25;
const std::uint32_t kDownloadQr[25] = {
    0x01FC267F, 0x01040041, 0x01758E5D, 0x01747B5D, 0x01752B5D, 0x01047441, 0x01FD557F,
    0x00018000, 0x0030FAE3, 0x00F8CC95, 0x01B9B270, 0x012549AA, 0x010D16E8, 0x0088A8A9,
    0x01B8E259, 0x016E0931, 0x005F6141, 0x00112F00, 0x01152B7F, 0x0091CF41, 0x00DFB85D,
    0x0187EA5D, 0x0162F45D, 0x011D4141, 0x01231D7F,
};

/* The code in an s x s white square, its quiet zone inside. Each module is
   a whole number of screen pixels, so a camera gets sharp edges. */
void download_qr(float x, float y, float s) {
  const int n = kDownloadQrSize, quiet = 3;
  const float px = platform::pixel();
  const float m = std::max(1.0f, std::floor(s / (n + 2 * quiet) / px)) * px;
  draw::rect(x, y, s, s, IM_COL32(255, 255, 255, 255), s * 0.04f);
  const float ox = std::round((x + (s - n * m) / 2) / px) * px;
  const float oy = std::round((y + (s - n * m) / 2) / px) * px;
  for(int r = 0; r < n; r++) {
    for(int c = 0; c < n;) {
      int e = c;
      while(e < n && (kDownloadQr[r] >> e & 1)) e++;
      if(e > c) draw::rect(ox + c * m, oy + r * m, (e - c) * m, m, IM_COL32(0, 0, 0, 255));
      c = e + 1;
    }
  }
}


/* Allowed: start the server if it isn't running, and hand the asker a login
   of its own that only works from its address, for as long as the server
   runs. */
void quick_allow(const discovery_request_t &r) {
  if(!server_running()) start_server();
  if(!server_running()) {  // start_server() has said why, here
    discovery_answer(r.nonce, -1, "", 0, "", "");
    return;
  }
  char token[7], pass[25];
  discovery_token(token, sizeof(token));
  discovery_token(pass, sizeof(pass));
  const std::string user = std::string("quick-") + token;
#if CHIRAL_HAS_SFTPD
  if(server_is_sftp()) sftpd_add_guest(user.c_str(), pass, r.ip);
  else
#endif
    ftpd_add_guest(user.c_str(), pass, r.ip);
  discovery_answer(r.nonce, 1, server_is_sftp() ? "sftp" : "ftp", server_port(), user.c_str(), pass);
  show_toast(trf("{0} is connecting", {r.name}));
}

void quick_deny(const discovery_request_t &r) {
  discovery_answer(r.nonce, 0, "", 0, "", "");
}

#if CHIRAL_UI_DESKTOP
void draw_quick_prompt_desktop(const discovery_request_t &r);  // ui_desktop.inc
#endif

/* Another device asks to connect: what it is, the code it shows, Allow or
   Deny. Refused on its own when the time runs out. */
struct QuickPrompt : Overlay {
  discovery_request_t req;
  explicit QuickPrompt(const discovery_request_t &r) : req(r) {}
  OverlayKind kind() override { return OV_PROMPT; }

  bool input(Action a) override {
    if(a == ACT_CROSS) {
      quick_allow(req);
      return false;
    }
    if(a == ACT_CIRCLE) {
      quick_deny(req);
      return false;
    }
    return true;
  }

  // a: 0 Deny, 1 Allow
  bool click(int a, int b, bool right) override {
    (void)b;
    if(right) return true;
    if(a == 1) quick_allow(req);
    else quick_deny(req);
    return false;
  }

  void draw() override {
    discovery_request_t now;
    if(discovery_pending(&now) && !strcmp(now.nonce, req.nonce)) req.seconds_left = now.seconds_left;
#if CHIRAL_UI_DESKTOP
    if(desktop_ui()) {
      draw_quick_prompt_desktop(req);
      return;
    }
#endif
#if !CHIRAL_UI_DESKTOP || CHIRAL_UI_MOBILE  // a TV, from the Android build, draws the console's
    const float w = 960, h = 560, x = (W - w) / 2, y = (H - h) / 2;
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 160));
    draw::rect(x, y, w, h, draw::PANEL, 18);
    hit_add(HIT_OVERLAY_BODY, x, y, w, h);
    kind_square(x + 44, y + 40, 78, req.kind);
    draw::text_clip(x + 144, y + 44, FS_TITLE, draw::TEXT, trf("{0} wants to connect", {req.name}).c_str(), w - 190, true);
    draw::text(x + 144, y + 92, FS_SMALL, draw::MUTED, req.ip);
    float ty = y + 150;
    for(const std::string &l : wrap_text(tr("It will be able to browse, download, upload and delete everything this server shares. Allow it only if it shows the same code."), FS_READ, w - 88)) {
      draw::text(x + 44, ty, FS_READ, draw::TEXT, l.c_str());
      ty += 38;
    }
    code_boxes(x + w / 2, ty + 18, 84, req.code, draw::BG);
    const float by = y + h - 100;
    const char *deny = tr("Deny"), *allow = tr("Allow");
    const float bw = 230;
    /* Each button: its key's glyph, then the word, centred together. The
       glyph's width differs by platform (a round button, a key cap). */
    auto key_button = [&](float bx, Action key, const char *word, ImU32 fill, int item) {
      hit_add(HIT_MENU_ITEM, bx, by, bw, 64, item);
      draw::rect(bx, by, bw, 64, fill, 12);
      const float glyph = draw::button(-1000, -1000, 38, key);  // measured off screen
      const float total = glyph + 14 + draw::text_size(FS, word, true).x;
      const float gx = bx + (bw - total) / 2;
      draw::button(gx, by + 13, 38, key);
      draw::text(gx + glyph + 14, by + 16, FS, draw::TEXT, word, true);
    };
    key_button(x + w - 44 - 2 * bw - 20, ACT_CIRCLE, deny, draw::PANEL2, 0);
    key_button(x + w - 44 - bw, ACT_CROSS, allow, draw::ACCENT, 1);
    draw::text(x + 44, by + 18, FS_SMALL, draw::MUTED, trf("Refused on its own in {0} s", {std::to_string(req.seconds_left)}).c_str());
#endif
  }
};

/* This side asking: the devices nearby, then the wait for an answer. The
   answer itself is acted on by quick_outcome(), whatever is on screen. */
struct QuickList : Overlay {
  int cursor = 0;
  std::vector<discovery_peer_t> peers;
  QuickList() { discovery_probe(); }
  OverlayKind kind() override { return OV_QUICK; }

  void refresh() {
    discovery_peer_t buf[DISCOVERY_MAX_PEERS];
    const int n = discovery_peers(buf, DISCOVERY_MAX_PEERS);
    peers.assign(buf, buf + n);
    if(cursor >= n) cursor = n - 1;
    if(cursor < 0) cursor = 0;
  }

  static bool asking(discovery_outcome_t *o) {
    discovery_get_outcome(o);
    return o->state == DISCOVERY_ASKING;
  }

  void focus(int a, int b) override {
    (void)b;
    cursor = a;
  }

  bool input(Action a) override {
    discovery_outcome_t o;
    if(asking(&o)) {
      if(a == ACT_CIRCLE) discovery_cancel();
      return true;
    }
    if(a == ACT_CIRCLE) return false;
    refresh();
    const int n = (int)peers.size();
    if(a == ACT_UP && n) cursor = (cursor + n - 1) % n;
    if(a == ACT_DOWN && n) cursor = (cursor + 1) % n;
    if(a == ACT_TRIANGLE) discovery_probe();
    if(a == ACT_CROSS && n) discovery_ask(&peers[cursor]);
    return true;
  }

  void draw() override {
    refresh();
    const float w = 1180, h = 940, x = (W - w) / 2, y = (H - h) / 2;
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 150));
    draw::rect(x, y, w, h, draw::PANEL, 18);
    hit_add(HIT_OVERLAY_BODY, x, y, w, h);
    draw::text(x + 44, y + 34, FS_TITLE, draw::TEXT, tr("Quick connect"), true);
    discovery_outcome_t o;
    if(asking(&o)) {
      draw::text_clip(x + 44, y + 110, 40, draw::TEXT, trf("Asking {0}...", {o.name}).c_str(), w - 88, true);
      draw::text_clip(x + 44, y + 172, FS_READ, draw::MUTED,
                      trf("Accept on {0} to connect. Check that it shows the same code:", {o.name}).c_str(), w - 88);
      code_boxes(x + w / 2, y + 250, 110, o.code, draw::BG);
      draw::spinner(x + 62, y + 470, 14, draw::ACCENT);
      draw::text(x + 90, y + 455, FS_SMALL, draw::MUTED,
                 trf("Waiting · {0} · gives up in {1} s", {o.ip, std::to_string(o.seconds_left)}).c_str());
      hint(x + 44, y + h - 74, ACT_CIRCLE, tr("Cancel"));
      return;
    }
    draw::text_clip(x + 44, y + 86, FS_SMALL, draw::MUTED,
                    tr("chiral-ftp on other devices on this network. Ask one to share, then browse it here."), w - 88);
    float yy = y + 140;
    const float row = 92;
    if(peers.empty()) {
      if(!discovery_running() && discovery_problem()[0]) {
        draw::text(x + 44, yy + 20, FS, draw::ERR, tr("Quick connect isn't available here: the network refused it (see the message log)."));
      } else {
        draw::spinner(x + 62, yy + 34, 14, draw::ACCENT);
        draw::text(x + 92, yy + 18, FS, draw::TEXT, tr("Looking for chiral-ftp on your network..."));
        draw::text(x + 92, yy + 60, FS_SMALL, draw::MUTED, tr("Open chiral-ftp on the other device: it shows up here by itself."));
      }
    }
    const float band = y + h - 380;  // where to get chiral-ftp, below the list
    for(int i = 0; i < (int)peers.size() && yy + row < band - 10; i++, yy += row) {
      const discovery_peer_t &p = peers[i];
      const bool on = i == cursor;
      hit_add(HIT_MENU_ITEM, x + 24, yy, w - 48, row - 10, i);
      if(on) draw::rect(x + 24, yy, w - 48, row - 10, draw::SEL, 12);
      kind_square(x + 44, yy + 14, 54, p.kind);
      draw::text_clip(x + 118, yy + 12, FS, draw::TEXT, p.name, w - 420, true);
      const std::string sub = std::string(kind_label(p.kind)) + "  \xc2\xb7  " + p.ip + "  \xc2\xb7  chiral-ftp " + p.version;
      draw::text_clip(x + 118, yy + 48, FS_SMALL, draw::MUTED, sub.c_str(), w - 420);
    }
    draw::rect(x + 44, band, w - 88, 2, draw::LINE);
    const float qs = 196, tx = x + 44 + qs + 36, tw = x + w - 44 - tx;
    download_qr(x + 44, band + 24, qs);
    float ty = band + 28;
    draw::text_clip(tx, ty, FS, draw::TEXT, tr("chiral-ftp on your other devices"), tw, true);
    ty += 50;
    for(const std::string &l : wrap_text(tr("Free for Windows, Mac, Linux, Android, Nintendo Switch and PS5. Scan the code, or go to:"), FS_SMALL, tw)) {
      draw::text(tx, ty, FS_SMALL, draw::MUTED, l.c_str());
      ty += 34;
    }
    draw::text_clip(tx, ty + 8, FS_SMALL, draw::ACCENT2, kDownloadPage, tw, true);
    draw::text_clip(tx, ty + 46, FS_SMALL, draw::ACCENT2, kReleasesPage, tw);
    char me[64];
    system_device_name(me, sizeof(me));
    draw::text_clip(x + 44, y + h - 130, FS_SMALL, draw::MUTED,
                    trf(device_text(N_("This console shows up on the others as \"{0}\"."),
                                    N_("This computer shows up on the others as \"{0}\".")), {me}).c_str(), w - 88);
    float hx = x + 44, hy = y + h - 74;
    if(!peers.empty()) hx += hint(hx, hy, ACT_CROSS, tr("Ask to connect"));
    hx += hint(hx, hy, ACT_TRIANGLE, tr("Refresh"));
    hint(hx, hy, ACT_CIRCLE, tr("Close"));
  }
};

bool open_mode(Mode m);  // with the start screen, further down

/* Connected by quick connect: the login the other side handed out, and the
   client to see it in -- here, or on a computer that keeps its client in
   the web app, there, in the browser. */
void quick_connected(const discovery_outcome_t &o) {
  json::Object site;
  json::set_str(site, "protocol", o.protocol);
  json::set_str(site, "host", o.ip);
  json::set_num(site, "port", o.port);
  json::set_str(site, "user", o.user);
  json::set_str(site, "pass", o.pass);
  json::set_str(site, "name", o.name);
  json::set_bool(site, "quick", true);
  connect(site);
  if(mode_shown(MODE_CLIENT)) {
    open_mode(MODE_CLIENT);
  } else {
    if(!websrv_running()) start_web(false);
    if(websrv_running()) platform::open_url((web_address() + "index.html").c_str());
  }
}

// An answer to our request, whatever screen it arrives on.
void quick_outcome() {
  discovery_outcome_t o;
  discovery_get_outcome(&o);
  if(o.state == DISCOVERY_IDLE || o.state == DISCOVERY_ASKING) return;
  discovery_cancel();  // seen: back to idle
  for(size_t i = g.overlays.size(); i-- > 0;) {
    if(g.overlays[i]->kind() == OV_QUICK) g.overlays.erase(g.overlays.begin() + i);
  }
  if(o.state == DISCOVERY_ALLOWED) quick_connected(o);
  else if(o.state == DISCOVERY_BUSY) show_toast(trf("{0} is answering someone else; try again in a moment", {o.name}), true);
  else if(o.state == DISCOVERY_TIMEOUT) show_toast(trf("{0} didn't answer", {o.name}), true);
  else if(o.state == DISCOVERY_FAILED) show_toast(trf("{0} said yes, but its server wouldn't start", {o.name}), true);
  else show_toast(trf("{0} said no", {o.name}), true);
}

// Being asked: the prompt comes up over whatever is open, and goes when the request does.
void quick_requests() {
  discovery_request_t r;
  const bool pending = discovery_pending(&r);
  for(size_t i = 0; i < g.overlays.size(); i++) {
    if(g.overlays[i]->kind() != OV_PROMPT) continue;
    QuickPrompt *p = static_cast<QuickPrompt *>(g.overlays[i].get());
    if(!pending || strcmp(r.nonce, p->req.nonce)) g.overlays.erase(g.overlays.begin() + i);  // withdrawn, or timed out
    return;
  }
  if(!pending) return;
  /* A text entry open underneath would keep the keys from the prompt. */
  for(size_t i = g.overlays.size(); i-- > 0;) {
    if(g.overlays[i]->kind() == OV_TEXT) g.overlays.erase(g.overlays.begin() + i);
  }
  push(new QuickPrompt(r));
  g_touch_dirty = true;
}

/* ---------- file manager ---------- */

Pane &fm_other(Pane &p) {
  return &p == &g.fm[0] ? g.fm[1] : g.fm[0];
}

void fm_copy(Pane &from, bool move) {
  Pane &to = fm_other(from);
  if(from.path.empty()) return;
  if(to.path.empty()) {
    show_toast(move ? tr("Open the folder to move into on the other side first")
                    : tr("Open the folder to copy into on the other side first"), true);
    return;
  }
  if(to.path == from.path) {
    show_toast(tr("Both sides show the same folder; open another one on the other side"), true);
    return;
  }
  std::vector<Entry> items = selection(from);
  int added = 0;
  for(auto &e : items) {
    if(!fsjob_add(move, join(from.path, e.name).c_str(), to.path.c_str())) added++;
  }
  from.marked.clear();
  if(added) show_toast(trf(move ? N_("Moving {0} items to {1}") : N_("Copying {0} items to {1}"), {std::to_string(added), to.path}));
}

void fm_menu() {
  Pane &p = g.fm[g.fm_active];
  bool in_folder = !p.path.empty();
  bool on_item = in_folder && p.cursor < (int)p.entries.size();
  fsjob_stats_t js;
  fsjob_get_stats(&js);
  Menu *m = new Menu;
  m->title = tr("File manager");
  Pane *pp = &p;
  m->items.push_back({tr("Start screen"), [] { go_mode(MODE_HOME); }});
  m->items.push_back({tr("Copy to the other side"), [pp] { fm_copy(*pp, false); }, on_item});
  m->items.push_back({tr("Move to the other side"), [pp] { fm_copy(*pp, true); }, on_item});
  m->items.push_back({tr("New folder"), [pp] { make_folder(*pp); }, in_folder});
  if(on_item) {
    Entry e = p.entries[p.cursor];
    m->items.push_back({trf("Rename \"{0}\"", {e.name}), [pp, e] { rename_item(*pp, e); }});
  }
  m->items.push_back({tr("Change permissions"), [pp] { chmod_items(*pp); }, on_item});
  if(on_item && selection_has_folder(p)) m->items.push_back({tr("Change permissions recursively"), [pp] { chmod_items(*pp, true); }});
  m->items.push_back({tr("Mark all"), [pp] { for(auto &e : pp->entries) if(!e.drive) pp->marked.insert(e.name); }, in_folder});
  m->items.push_back({tr("Clear marks"), [pp] { pp->marked.clear(); }, !p.marked.empty()});
  m->items.push_back({tr("Refresh"), [pp] { refresh(*pp); }, in_folder});
  m->items.push_back({tr("Drives"), [pp] { load_drives(*pp); }});
  if(js.running || js.queued) m->items.push_back({tr("Stop copying"), [] { fsjob_cancel_all(); }, true, true});
  m->items.push_back({tr("Delete"), [pp] { delete_items(*pp); }, on_item, true});
  m->items.push_back({tr("Settings"), [] { push(new Settings); }});
  m->items.push_back({tr("About chiral-ftp"), [] { push(new About); }});
  push(m);
}

void handle_fm(Action a) {
  Pane &p = g.fm[g.fm_active];
  int count = (int)p.entries.size();
  int page = std::max(1, visible_rows() - 1);
  switch(a) {
  case ACT_UP: move_cursor(p, count, -1); break;
  case ACT_DOWN: move_cursor(p, count, 1); break;
  case ACT_L2: move_cursor(p, count, -page); break;
  case ACT_R2: move_cursor(p, count, page); break;
  case ACT_LEFT: g.fm_active = 0; break;
  case ACT_RIGHT: g.fm_active = 1; break;
  case ACT_L1: case ACT_R1: g.fm_active = 1 - g.fm_active; break;
  case ACT_OPTIONS: fm_menu(); break;
  case ACT_CROSS: open_entry(p); break;
  case ACT_TRIANGLE: fm_copy(p, false); break;
  case ACT_SQUARE:
    if(p.cursor < count && !p.entries[p.cursor].drive) {
      const std::string &n = p.entries[p.cursor].name;
      if(p.marked.count(n)) p.marked.erase(n);
      else p.marked.insert(n);
      move_cursor(p, count, 1);
    }
    break;
  case ACT_CIRCLE:
    if(p.path.empty()) go_mode(MODE_HOME);
    else go_up(p);
    break;
  default: break;
  }
}

void draw_fm() {
  draw::rect(0, 0, W, H, draw::BG);
  draw_top();
  float ph = H - PANE_Y - STATUS_H - HINT_H - 16;
  float pw = (W - 2 * MARGIN - PANE_GAP) / 2;
  draw_pane(g.fm[0], 0, true, MARGIN, PANE_Y, pw, ph);
  draw_pane(g.fm[1], 1, true, MARGIN + pw + PANE_GAP, PANE_Y, pw, ph);

  // Status line: the copy in progress, else the last message.
  fsjob_stats_t js;
  fsjob_get_stats(&js);
  float sy = H - HINT_H - STATUS_H;
  if(js.running) {
    double pct = js.total > 0 ? std::min(1.0, (double)js.done / (double)js.total) : 0;
    char line[600];
    snprintf(line, sizeof(line), "%s  ·  %d%%  ·  %s%s%s", trf(js.move ? N_("Moving {0}") : N_("Copying {0}"), {js.current}).c_str(),
             (int)(pct * 100), trf("{0} of {1}", {fmt_size(js.done), fmt_size(js.total)}).c_str(),
             js.speed > 0 ? ("  ·  " + fmt_size((long long)js.speed) + "/s").c_str() : "",
             js.queued ? ("  ·  " + trf("{0} more waiting", {std::to_string(js.queued)})).c_str() : "");
    draw::rect(MARGIN, sy + 6, 300, 28, draw::PANEL2, 14);
    draw::rect(MARGIN, sy + 6, (float)(300 * pct), 28, draw::ACCENT, 14);
    draw::text_clip(MARGIN + 320, sy + 8, FS_SMALL, draw::ACCENT2, line, W - 2 * MARGIN - 320);
  } else if(now() < g.toast_until) {
    draw::text_clip(MARGIN + 8, sy + 8, FS_SMALL, g.toast_err ? draw::ERR : draw::MUTED, g.toast.c_str(), W - 2 * MARGIN);
  }

  float hy = H - HINT_H;
  draw::rect(0, hy, W, HINT_H, draw::PANEL2);
  float x = MARGIN, y = hy + (HINT_H - 38) / 2;
  if(!g.overlays.empty()) {
    x += hint(x, y, ACT_CROSS, tr("Select"));
    hint(x, y, ACT_CIRCLE, tr("Back"));
    return;
  }
  Pane &p = g.fm[g.fm_active];
  bool on_dir = p.cursor < (int)p.entries.size() && p.entries[p.cursor].dir;
  x += hint(x, y, ACT_CROSS, on_dir ? tr("Open") : tr("Actions"));
  x += hint(x, y, ACT_SQUARE, tr("Mark"));
  if(!p.path.empty()) {
    char t[64];
    size_t n = p.marked.empty() ? 1 : p.marked.size();
    snprintf(t, sizeof(t), "%s", trf("Copy {0} to the other side", {std::to_string(n)}).c_str());
    x += hint(x, y, ACT_TRIANGLE, t);
  }
  x += hint(x, y, ACT_CIRCLE, p.path.empty() ? tr("Start screen") : tr("Back"));
  if(!p.path.empty()) x += hint(x, y, ACT_CIRCLE_LONG, tr("Hold: start screen"));
  x += hint_lr(x, y, tr("Switch side"));
  hint(x, y, ACT_OPTIONS, tr("Menu: move, rename, delete..."));
}

// The FTP / SFTP client: this console on the left, the server on the right.
void draw_client() {
  draw::rect(0, 0, W, H, draw::BG);
  draw_top();
  float ph = H - PANE_Y - STATUS_H - HINT_H - 16;
  float pw = (W - 2 * MARGIN - PANE_GAP) / 2;
  draw_pane(g.panes[0], 0, false, MARGIN, PANE_Y, pw, ph);
  draw_pane(g.panes[1], 1, false, MARGIN + pw + PANE_GAP, PANE_Y, pw, ph);
  draw_bottom();
}

struct HomeCard {
  const char *title, *text, *computer_text;  // the description on a console, and on a computer
  Mode mode;
};

const int kCardCount = 4;
const HomeCard kCards[kCardCount] = {
    {N_("FTP server"), N_("Turn the console into an FTP server. Connect to it from your PC, phone or another console."),
     N_("Share a folder of this computer over SFTP or FTP, with a phone, a console or another computer."), MODE_FTPD},
    {N_("Web app"), N_("Open chiral-ftp in a browser on your PC: manage the console's files, upload and download."),
     N_("The file manager and the FTP / SFTP client, in your browser."), MODE_WEB},
    {N_("File manager"), N_("Copy and move files on the console: the SD card, the homebrew folder, anywhere."),
     N_("Copy and move files on this computer: your home folder, other drives, anywhere."), MODE_FILES},
    {N_("FTP / SFTP client"), N_("Connect from the console to your laptop, PC or NAS and transfer files both ways."),
     N_("Connect to a server, a NAS or a console and transfer files both ways."), MODE_CLIENT},
};

void draw_card_icon(Mode m, float cx, float cy) {
  ImDrawList *d = draw::dl();
  d->AddCircleFilled(ImVec2(cx, cy), 62, IM_COL32(20, 50, 96, 255), 40);
  if(m == MODE_FTPD) {  // server: stacked boxes
    for(int k = 0; k < 3; k++) {
      d->AddRectFilled(ImVec2(cx - 34, cy - 34 + k * 24), ImVec2(cx + 34, cy - 14 + k * 24), draw::TEXT, 5);
      d->AddCircleFilled(ImVec2(cx + 22, cy - 24 + k * 24), 4, draw::OK, 8);
    }
  } else if(m == MODE_WEB) {  // browser window
    d->AddRectFilled(ImVec2(cx - 40, cy - 30), ImVec2(cx + 40, cy + 30), draw::TEXT, 6);
    d->AddRectFilled(ImVec2(cx - 40, cy - 30), ImVec2(cx + 40, cy - 16), draw::ACCENT, 6);
    d->AddRectFilled(ImVec2(cx - 32, cy - 6), ImVec2(cx - 4, cy + 22), IM_COL32(20, 50, 96, 255), 3);
    d->AddRectFilled(ImVec2(cx + 4, cy - 6), ImVec2(cx + 32, cy + 22), IM_COL32(20, 50, 96, 255), 3);
  } else if(m == MODE_FILES) {  // two folders
    draw::folder_icon(cx - 40, cy - 30, 48, draw::ACCENT2);
    draw::folder_icon(cx - 6, cy - 6, 48, draw::TEXT);
  } else {  // client: the app's own mark (upload and download)
    draw::brand_mark(cx - 46, cy - 46, 92);
  }
}

// Set once at startup by main, where the platform has something to say.
bool g_launch_known = false, g_launch_full = false;
std::string g_launch_line, g_launch_detail;  // English, translated when drawn
int g_launch_status = 0;

/* How the app was launched, in the platform's own words
   (backend::startup_notice): on the PS5 whether it got the console's
   storage and how, on the Switch whether it got a full application slot.
   Worth saying out loud on the start screen, since it is the one thing the
   user can still change. */
void draw_launch_banner(float y) {
  if(!g_launch_known || g_launch_line.empty()) return;
  const std::string text = trf(g_launch_line.c_str(), {tr(g_launch_detail.c_str()), std::to_string(g_launch_status)});
  ImVec2 ts = draw::text_size(FS_SMALL, text.c_str(), true);
  float w = std::min(W - 2 * MARGIN, ts.x + 70);
  draw::rect(MARGIN, y, w, 48, g_launch_full ? IM_COL32(24, 64, 44, 255) : IM_COL32(80, 56, 18, 255), 24);
  draw::dl()->AddCircleFilled(ImVec2(MARGIN + 26, y + 24), 8, g_launch_full ? draw::OK : draw::ACCENT2, 12);
  draw::text_clip(MARGIN + 46, y + 11, FS_SMALL, g_launch_full ? draw::OK : draw::ACCENT2, text.c_str(), w - 66, true);
}

void draw_home() {
  draw::rect(0, 0, W, H, draw::BG);
  draw_top();
  draw::text(MARGIN + 8, TOP_H + 50, 52, draw::TEXT, tr("What would you like to do?"), true);
  {
    /* Quick connect, above the cards: Up reaches it. Level with the
       heading, out of the way of the Switch's launch banner at the bottom. */
    discovery_peer_t peers[DISCOVERY_MAX_PEERS];
    const int n = discovery_peers(peers, DISCOVERY_MAX_PEERS);
    const char *label = tr("Quick connect");
    const std::string count = n ? trf("{0} nearby", {std::to_string(n)}) : std::string();
    const float lw = draw::text_size(FS, label, true).x, cwid = n ? draw::text_size(FS_SMALL, count.c_str(), true).x + 36 : 0;
    const float bh = 64, bw = 92 + lw + (n ? cwid + 16 : 0), bx = W - MARGIN - bw, by = TOP_H + 44;
    const bool on = g_home_quick && g.overlays.empty();
    hit_add(HIT_QUICK, bx, by, bw, bh);
    draw::rect(bx, by, bw, bh, on ? draw::SEL : draw::PANEL2, 16);
    if(on) draw::frame(bx, by, bw, bh, draw::ACCENT, 16, 3);
    /* A small radar: a dot and two arcs. */
    ImDrawList *d = draw::dl();
    const ImVec2 c(bx + 40, by + bh / 2 + 8);
    d->AddCircleFilled(c, 5, draw::ACCENT2, 12);
    for(int k = 1; k <= 2; k++) {
      d->PathArcTo(c, 11.0f * k + 2, -2.4f, -0.74f, 12);
      d->PathStroke(draw::ACCENT2, ImDrawFlags_None, 3.0f);
    }
    draw::text(bx + 70, by + (bh - FS) / 2 - 2, FS, draw::TEXT, label, true);
    if(n) {
      const float px = bx + 70 + lw + 16;
      draw::rect(px, by + 16, cwid, bh - 32, IM_COL32(24, 64, 44, 255), (bh - 32) / 2);
      draw::text(px + 18, by + (bh - FS_SMALL) / 2 - 2, FS_SMALL, draw::OK, count.c_str(), true);
    }
  }
  draw::text(MARGIN + 8, TOP_H + 124, FS_CARD, draw::MUTED,
             tr(device_text(N_("Servers keep running while you use the other modes or go to the home screen. Closing chiral-ftp stops them."),
                            N_("Servers keep running while you use the other modes. Quitting chiral-ftp stops them."))));
  draw_launch_banner(H - HINT_H - 64);
  std::vector<int> shown;
  for(int i = 0; i < kCardCount; i++) if(mode_shown(kCards[i].mode) || !mode_enabled(kCards[i].mode)) shown.push_back(i);
  const int count = (int)shown.size();
  /* Fewer cards (a computer, without its file manager and client here)
     are no wider than four would be, a row centred under the heading. */
  float gap = 30, cw = std::min((W - 2 * MARGIN - (count - 1) * gap) / count, 720.0f), ch = 580, cy = TOP_H + 210;
  const float row_x = count < kCardCount ? (W - count * cw - (count - 1) * gap) / 2 : MARGIN;
  for(int k = 0; k < count; k++) {
    const int i = shown[k];
    Mode mode = kCards[i].mode;
    float cx = row_x + k * (cw + gap);
    const bool have = mode_enabled(mode);
    if(have) hit_add(HIT_CARD, cx, cy, cw, ch, i);  // a: the card in kCards
    bool on = i == g_home_cursor && g.overlays.empty() && !g_home_quick;
    draw::rect(cx, cy, cw, ch, on ? draw::PANEL2 : draw::PANEL, 22);
    if(on) draw::frame(cx, cy, cw, ch, draw::ACCENT, 22, 4);
    /* A mode this build was compiled without: still shown, so the start
       screen reads the same, but dimmed and inert. */
    const ImU32 title_col = have ? draw::TEXT : IM_COL32(90, 100, 120, 255);
    const ImU32 body_col = have ? draw::MUTED : IM_COL32(70, 78, 96, 255);
    if(have) draw_card_icon(mode, cx + cw / 2, cy + 120);
    else draw::dl()->AddCircleFilled(ImVec2(cx + cw / 2, cy + 120), 62, IM_COL32(24, 30, 46, 255), 40);
    const char *card_title = mode == MODE_FTPD && CHIRAL_HAS_SFTPD ? tr("FTP / SFTP server") : tr(kCards[i].title);
    ImVec2 ts = draw::text_size(36, card_title, true);
    draw::text(cx + (cw - ts.x) / 2, cy + 220, 36, title_col, card_title, true);
    // The description, over a few lines.
    float ty = cy + 290;
    for(const std::string &line : wrap_text(tr(device_text(kCards[i].text, kCards[i].computer_text)), FS_CARD, cw - 60)) {
      draw::text(cx + 30, ty, FS_CARD, body_col, line.c_str());
      ty += 44;
    }
    if(!have) {
      draw::text(cx + 30, ty + 10, FS_SMALL, IM_COL32(120, 92, 40, 255),
                 tr("Not in this build"));
    }
    // What's running now.
    std::string status;
    bool active = false;
    fsjob_stats_t js;
    fsjob_get_stats(&js);
    if(mode == MODE_FTPD && server_running()) status = server_address(), active = true;
    if(mode == MODE_WEB && websrv_running()) status = web_address(), active = true;
    if(mode == MODE_FILES && (js.running || js.queued)) status = trf(js.move ? N_("Moving {0}") : N_("Copying {0}"), {js.current}), active = true;
    if(mode == MODE_CLIENT && g.connected) status = trf("Connected to {0}", {g.server.host}), active = true;
    if(mode == MODE_CLIENT && (g.qs.running || g.qs.queued)) status += (status.empty() ? "" : "  ·  ") + trf("Transfers: {0}", {std::to_string(g.qs.running + g.qs.queued)}), active = true;
    if(active) {
      draw::rect(cx + 24, cy + ch - 84, cw - 48, 56, IM_COL32(24, 64, 44, 255), 14);
      draw::dl()->AddCircleFilled(ImVec2(cx + 52, cy + ch - 56), 8, draw::OK, 12);
      draw::text_clip(cx + 72, cy + ch - 71, FS_SMALL, draw::OK, status.c_str(), cw - 120, true);
    }
  }
  draw_hints({{ACT_LEFT, tr("Choose")}, {ACT_CROSS, tr("Open")}, {ACT_OPTIONS, tr("Settings & about")}});
}

// Plus on the start screen, and on the FTP server and web app screens
// (with "Start screen" first, like every mode's menu).
void options_menu(bool in_mode) {
  Menu *m = new Menu;
  m->title = in_mode ? tr("Menu") : "chiral-ftp";
  if(in_mode) m->items.push_back({tr("Start screen"), [] { go_mode(MODE_HOME); }});
  m->items.push_back({tr("Settings"), [] { push(new Settings); }});
  m->items.push_back({tr("Message log"), [] { push(new LogView); }});
  m->items.push_back({tr("About chiral-ftp"), [] { push(new About); }});
  if(platform::can_quit()) {
    m->items.push_back({tr("Exit chiral-ftp"), [] {
      confirm(tr("Exit"), tr("Close chiral-ftp? Running transfers stop."), tr("Exit"), [] { g.quit = true; });
    }, true, true});
  }
  push(m);
}

// Opens one of the four modes, from its card or from the menu bar.
bool open_mode(Mode m) {
  if(!mode_enabled(m)) {
    show_toast(tr("Not in this build"), true);
    return false;
  }
  if(!mode_shown(m)) return false;  // a computer's, kept to the web app
  for(int i = 0; i < kCardCount; i++) if(kCards[i].mode == m) g_home_cursor = i;
  if(m == MODE_FILES && g.fm[0].path.empty() && g.fm[0].entries.empty()) {
    load_drives(g.fm[0]);
    load_drives(g.fm[1]);
  }
  go_mode(m);
  return true;
}

bool handle_home(Action a) {
#if CHIRAL_UI_MOBILE
  /* A TV's Back on the start screen: behind the home screen, as any app,
     with the servers still running. */
  if(a == ACT_CIRCLE) {
    platform::to_background();
    return true;
  }
#endif
  if(a == ACT_OPTIONS) {
    options_menu(false);
    return true;
  }
  // Up to Quick connect, above the cards, and Down back to them.
  if(a == ACT_UP) {
    g_home_quick = true;
    return true;
  }
  if(a == ACT_DOWN) {
    g_home_quick = false;
    return true;
  }
  if(g_home_quick && a == ACT_CROSS) {
    push(new QuickList);
    return true;
  }
  if(g_home_quick && (a == ACT_LEFT || a == ACT_RIGHT || a == ACT_L1 || a == ACT_R1)) {
    g_home_quick = false;  // back to the cards, where it was
    return true;
  }
  // Step over the modes this build hasn't got rather than stopping on them.
  if(a == ACT_LEFT || a == ACT_L1 || a == ACT_RIGHT || a == ACT_R1) {
    const int step = (a == ACT_LEFT || a == ACT_L1) ? kCardCount - 1 : 1;
    for(int i = 0; i < kCardCount; i++) {
      g_home_cursor = (g_home_cursor + step) % kCardCount;
      if(mode_shown(kCards[g_home_cursor].mode)) break;
    }
  } else if(a == ACT_CROSS) {
    open_mode(kCards[g_home_cursor].mode);
  }
  return true;
}

#if CHIRAL_UI_DESKTOP
#include "ui_desktop.inc"
#endif
#if CHIRAL_UI_MOBILE
#include "ui_mobile.inc"
#endif

}  // namespace

void init() {
  memset(&g.qs, 0, sizeof(g.qs));
  memset(&g.server, 0, sizeof(g.server));
  g.panes[0].remote = false;
  g.panes[1].remote = true;
  startup_trace("ui::init: loading saved sites and history");
  load_all();
  startup_trace("ui::init: loading server settings");
  load_server_prefs();
  if(mode_enabled(MODE_CLIENT)) {
    startup_trace("ui::init: building the server list");
    rebuild_servers();
  }
  if(panes_enabled()) {
    startup_trace("ui::init: listing drives");
    load_drives();
    startup_trace("ui::init: starting the background worker");
    if(!g_jobs.start()) {
      startup_trace("ui::init: the background worker would not start");
      log_msg(0, LOG_ERROR, "Could not start the background worker; "
                            "listing and transfers will not run");
    }
  }
  if(mode_enabled(MODE_CLIENT)) {
    startup_trace("ui::init: checking for an open session");
    ftp_server_t s;
    if(session_server(&s)) {
      g.connected = true;
      g.server = s;
      load_remote("");
    }
  }
  /* Put the cursor on something this build can actually open. */
  for(int i = 0; i < kCardCount; i++) {
    if(mode_shown(kCards[i].mode)) {
      g_home_cursor = i;
      break;
    }
  }
  startup_trace("ui::init: done");
}

/* A tap, a drag, or nothing. Called from update(), so it runs on the UI
   thread between frames like every other input. */
void handle_touch() {
  if(!platform::has_touch()) return;
  platform::Touch t = platform::take_touch();
  if(t.held || t.pressed || t.released) g_touch_dirty = true;

  // Dragging a list scrolls it, a row at a time, and never counts as a tap.
  if(t.held && t.moved && t.drag_y != 0) {
    const Hit *z = hit_at(t.x, t.y, HIT_SCROLL);
    if(z) {
      Pane *p = z->b ? &g.fm[z->a] : &g.panes[z->a];
      const int n = (int)p->entries.size();
      const int rows = (int)(z->h / ROW_H);
      static float carried = 0;
      carried -= t.drag_y;
      while(carried >= ROW_H && p->scroll < std::max(0, n - rows)) { p->scroll++; carried -= ROW_H; }
      while(carried <= -ROW_H && p->scroll > 0) { p->scroll--; carried += ROW_H; }
      if(p->cursor < p->scroll) p->cursor = p->scroll;
      if(p->cursor >= p->scroll + rows) p->cursor = p->scroll + rows - 1;
    }
    return;
  }
  if(!t.released || t.moved) return;

  // Menus and dialogs come first: they are on top, and they own the screen.
  if(!g.overlays.empty()) {
    if(const Hit *item = hit_at(t.x, t.y, HIT_MENU_ITEM)) {
      Overlay *top = g.overlays.back().get();
      if(top->kind() == OV_MENU) {  // no RTTI in this build; kind() is the tag
        static_cast<Menu *>(top)->cursor = item->a;  // tap and highlight agree
        handle(ACT_CROSS);
        return;
      }
    }
    // Outside the panel: the same as backing out of it.
    if(!hit_at(t.x, t.y, HIT_OVERLAY_BODY)) handle(ACT_CIRCLE);
    return;
  }

  if(g.mode == MODE_HOME) {
    if(hit_at(t.x, t.y, HIT_QUICK)) {
      handle(ACT_CMD_QUICK);
      return;
    }
    if(const Hit *card = hit_at(t.x, t.y, HIT_CARD)) {
      // A mode is a harmless place to end up, so one tap opens it -- the
      // card tapped, not Quick connect if the pad had left the focus there.
      g_home_cursor = card->a;
      g_home_quick = false;
      handle(ACT_CROSS);
    }
    return;
  }

  // Disconnect, before the rows it sits over.
  if(hit_at(t.x, t.y, HIT_DISCONNECT)) {
    g.disconnect_focus = false;
    disconnect();
    return;
  }

  // The FTP server and web app screens: plain lists of controls.
  if(const Hit *set = hit_at(t.x, t.y, HIT_SETTING)) {
    g_row_cursor = set->a;
    handle(ACT_CROSS);
    return;
  }

  // A saved site or a recent connection on the client's server side.
  if(const Hit *srv = hit_at(t.x, t.y, HIT_SERVER)) {
    g.active = 1;
    remote().cursor = srv->a;
    handle(ACT_CROSS);
    return;
  }

  if(const Hit *row = hit_at(t.x, t.y, HIT_ROW)) {
    const bool fm = row->a >= 2;
    const int idx = fm ? row->a - 2 : row->a;
    Pane &p = fm ? g.fm[idx] : g.panes[idx];
    int &active = fm ? g.fm_active : g.active;
    active = idx;
    p.cursor = row->b;
    handle(ACT_CROSS);
  }
}

/* The top overlay's result, as handle() treats it: input() may have pushed
   new overlays, so only the one that closed is removed. */
void overlay_click(const Hit *item, bool right) {
  size_t depth = g.overlays.size();
  if(!g.overlays.back()->click(item->a, item->b, right)) g.overlays.erase(g.overlays.begin() + (depth - 1));
}

/* Rows of a pane the wheel has scrolled, kept between events so a trackpad's
   small steps add up. */
float g_wheel_carry = 0;

/* The mouse. What it is over takes the highlight, the way the d-pad would
   move it there; a click acts on it as Enter does (Cmd-click marks a file);
   a right click is the menu key; the wheel scrolls. While a panel is open
   only the panel counts, and a click outside it backs out, as Esc does.
   Called from update(), on the UI thread, like every other input. */
void handle_pointer() {
  if(!platform::has_pointer()) return;
  platform::Pointer p = platform::take_pointer();
  if(!p.moved && !p.clicked && !p.right_clicked && p.wheel == 0) return;
#if CHIRAL_UI_DESKTOP
  if(desktop_ui() && desk::pointer(p)) return;  // the desktop interface's own controls
#endif
  if(g_hits.empty()) g_touch_dirty = true;  // no rectangles yet: draw some
  const int top = (int)g.overlays.size();
  const float x = p.x, y = p.y;

  // What is under it, top layer first.
  const Hit *item = nullptr, *hint = nullptr, *card = nullptr, *row = nullptr, *server = nullptr,
            *setting = nullptr, *disc = nullptr, *body = nullptr, *quick = nullptr;
  if(p.inside) {
    if(top) {
      body = hit_at(x, y, HIT_OVERLAY_BODY, top);
      item = body ? hit_at(x, y, HIT_MENU_ITEM, top) : nullptr;
      hint = body ? hit_at(x, y, HIT_HINT, top) : hit_at(x, y, HIT_HINT, 0);
    } else {
      hint = hit_at(x, y, HIT_HINT, 0);
      card = hit_at(x, y, HIT_CARD, 0);
      row = hit_at(x, y, HIT_ROW, 0);
      server = hit_at(x, y, HIT_SERVER, 0);
      setting = hit_at(x, y, HIT_SETTING, 0);
      disc = hit_at(x, y, HIT_DISCONNECT, 0);
      quick = hit_at(x, y, HIT_QUICK, 0);
    }
  }

  // Hover: move the highlight, and redraw only when something changed.
  static const Hit *last = nullptr;
  static Hit last_hit{};
  const Hit *over = item ? item : hint ? hint : quick ? quick : card ? card : disc ? disc : server ? server : row ? row : setting;
  const bool same = (!over && !last) || (over && last && over->kind == last_hit.kind && over->a == last_hit.a &&
                                         over->b == last_hit.b && over->layer == last_hit.layer);
  if(p.moved && !same) {
    g_touch_dirty = true;
    if(item) g.overlays.back()->focus(item->a, item->b);
    if(quick) g_home_quick = true;
    if(card && mode_shown(kCards[card->a].mode)) {
      g_home_cursor = card->a;
      g_home_quick = false;
    }
    if(setting) g_row_cursor = setting->a;
    if(server) {
      g.active = 1;
      remote().cursor = server->a;
    }
    if(row) {
      const bool fm = row->a >= 2;
      const int idx = fm ? row->a - 2 : row->a;
      (fm ? g.fm_active : g.active) = idx;
      (fm ? g.fm[idx] : g.panes[idx]).cursor = row->b;
    }
  }
  last = over;
  if(over) last_hit = *over;
  platform::pointer_hand(over != nullptr);

  if(p.wheel != 0) {
    g_touch_dirty = true;
    g_wheel_carry -= p.wheel * 0.6f;  // up scrolls toward the top
    const int steps = (int)g_wheel_carry;
    g_wheel_carry -= (float)steps;
    if(top) {
      for(int k = 0; k < std::abs(steps); k++) handle(steps < 0 ? ACT_UP : ACT_DOWN);
    } else if(const Hit *z = p.inside ? hit_at(x, y, HIT_SCROLL, 0) : nullptr) {
      Pane *pn = z->b ? &g.fm[z->a] : &g.panes[z->a];
      const int n = (int)pn->entries.size();
      const int rows = (int)(z->h / ROW_H);
      pn->scroll = std::max(0, std::min(std::max(0, n - rows), pn->scroll + steps));
      if(pn->cursor < pn->scroll) pn->cursor = pn->scroll;
      if(pn->cursor >= pn->scroll + rows) pn->cursor = std::max(0, pn->scroll + rows - 1);
    }
  }

  if(p.clicked || p.right_clicked) {
    g_touch_dirty = true;
    const bool right = p.right_clicked && !p.clicked;
    if(top) {
      if(!body) {
        if(hint && !right) handle((Action)hint->a);  // the bar below the panel
        else handle(ACT_CIRCLE);                     // outside the panel: back out of it
      } else if(hint && !right) {
        handle((Action)hint->a);
      } else if(item) {
        overlay_click(item, right);
      }
      return;
    }
    if(right) {
      // The menu, for what is under the pointer when that is a file or a site.
      if(row) {
        const bool fm = row->a >= 2;
        const int idx = fm ? row->a - 2 : row->a;
        (fm ? g.fm_active : g.active) = idx;
        (fm ? g.fm[idx] : g.panes[idx]).cursor = row->b;
      }
      handle(ACT_OPTIONS);
      return;
    }
    if(hint) {
      handle((Action)hint->a);
    } else if(quick) {
      handle(ACT_CMD_QUICK);
    } else if(card) {
      g_home_cursor = card->a;
      handle(ACT_CROSS);
    } else if(disc) {
      g.disconnect_focus = false;
      disconnect();
    } else if(setting) {
      g_row_cursor = setting->a;
      handle(ACT_CROSS);
    } else if(server) {
      g.active = 1;
      remote().cursor = server->a;
      handle(ACT_CROSS);
    } else if(row) {
      const bool fm = row->a >= 2;
      const int idx = fm ? row->a - 2 : row->a;
      Pane &pn = fm ? g.fm[idx] : g.panes[idx];
      (fm ? g.fm_active : g.active) = idx;
      pn.cursor = row->b;
      if(p.modifier) {
        // Cmd-click: mark it or unmark it, as a Finder selection would.
        if(pn.cursor < (int)pn.entries.size() && !pn.entries[pn.cursor].drive) {
          const std::string &n = pn.entries[pn.cursor].name;
          if(pn.marked.count(n)) pn.marked.erase(n);
          else pn.marked.insert(n);
        }
      } else {
        handle(ACT_CROSS);
      }
    }
  }
}

/* When quick connect runs. On a computer: for as long as the app is open,
   whatever page or window is in front. On a console, only while someone
   could use it: the app in front, on the start screen, or with an ask or a
   request still going; anywhere else it stops, and the others stop listing
   it, until it is back. This is the one place that starts it -- the network
   thread never restarts itself -- so a socket that died (the network
   changed, the console slept) is replaced here too, a few seconds on. */
void quick_lifecycle() {
  static double retry_at = 0;
#if CHIRAL_DEVICE_COMPUTER
  const bool want = !g.quit;
#else
  discovery_outcome_t o;
  discovery_request_t r;
  discovery_get_outcome(&o);
  const bool want = !g.quit && platform::in_foreground() &&
                    (g.mode == MODE_HOME || o.state == DISCOVERY_ASKING || discovery_pending(&r));
#endif
  if(!want) {
    if(discovery_running()) discovery_stop();
    retry_at = 0;  // back in front: at once
    return;
  }
  if(discovery_running() || now() < retry_at) return;
  char name[64];
  system_device_name(name, sizeof(name));
  /* Started: a thread that ends by itself again waits five seconds for the
     next. Refused: the same. Only still stopping from a moment ago (no
     problem to report) tries again on the next frame. */
  const bool started = !discovery_start(name, system_device_kind());
  retry_at = started || discovery_problem()[0] ? now() + 5 : 0;
}

void update() {
  handle_touch();
  handle_pointer();
  quick_requests();
  quick_outcome();
  quick_lifecycle();
  watch_connection();
  g_jobs.poll();
  // Copies finished: show the new files on both sides.
  fsjob_stats_t js;
  fsjob_get_stats(&js);
  if(js.finished != g.fm_finished) {
    if(js.failed && js.error[0] && js.finished - g.fm_finished == 1) show_toast(js.error, true);
    g.fm_finished = js.finished;
    for(Pane &p : g.fm) if(!p.path.empty()) load_local(p.path, true, &p);
    if(!local().path.empty()) load_local(local().path, true);
  }
  g.log_seq = log_each(g.log_seq, on_log, nullptr);
  queue_view_t none[1];
  queue_snapshot("queued", none, 0, &g.qs);
  // Refresh both sides after transfers finish, like FileZilla.
  if(g.last_done >= 0 && g.qs.done != g.last_done) g.refresh_at = now() + 1.0;
  g.last_done = g.qs.done;
  if(g.refresh_at && now() >= g.refresh_at) {
    g.refresh_at = 0;
    if(!local().path.empty()) load_local(local().path, true);
    if(g.connected) refresh(remote());
  }
}

void handle(Action a) {
  if(a == ACT_QUIT) {
    g.quit = true;
    return;
  }
#if CHIRAL_UI_MOBILE
  if(a == ACT_NAV_LEFT || a == ACT_NAV_RIGHT) {
    if(desktop_ui()) mob::show_nav(a == ACT_NAV_RIGHT, 5);
    return;
  }
#endif
  /* The menu bar: straight there, from whatever is open. A text entry left
     open underneath would keep the keyboard for itself, so everything open
     closes first. */
  // View > Zoom: the whole window, whatever is open in it.
  if(a == ACT_CMD_ZOOM_IN || a == ACT_CMD_ZOOM_OUT || a == ACT_CMD_ZOOM_RESET) {
    const float z = platform::zoom();
    platform::set_zoom(a == ACT_CMD_ZOOM_RESET ? 1.0f : a == ACT_CMD_ZOOM_IN ? z * 1.125f : z / 1.125f);
    save_prefs();
    return;
  }
#if CHIRAL_UI_DESKTOP
  if(a >= ACT_CMD_HOME && desktop_ui() && desk::command(a)) return;  // pages, on the desktop
#endif
  if(a >= ACT_CMD_HOME) {
    g.overlays.clear();
    switch(a) {
    case ACT_CMD_HOME: go_mode(MODE_HOME); break;
    case ACT_CMD_FTPD: open_mode(MODE_FTPD); break;
    case ACT_CMD_WEB: open_mode(MODE_WEB); break;
    case ACT_CMD_FILES: open_mode(MODE_FILES); break;
    case ACT_CMD_CLIENT: open_mode(MODE_CLIENT); break;
    case ACT_CMD_CONNECT:
      if(open_mode(MODE_CLIENT)) push(new Form(json::Object(), -1));
      break;
    case ACT_CMD_SETTINGS: push(new Settings); break;
    case ACT_CMD_ABOUT: push(new About); break;
    case ACT_CMD_QUEUE: push(new QueueView); break;
    case ACT_CMD_LOG: push(new LogView); break;
    case ACT_CMD_QUICK: push(new QuickList); break;
    default: break;
    }
    return;
  }
  /* Back, held down: straight out to the start screen, however deep in a
     folder tree or however many menus down. */
  if(a == ACT_CIRCLE_LONG) {
    g.overlays.clear();
    if(g.mode != MODE_HOME) go_mode(MODE_HOME);
    return;
  }
  if(!g.overlays.empty()) {
    Overlay *top = g.overlays.back().get();
    size_t depth = g.overlays.size();
    bool keep = top->input(a);
    // input() may have pushed new overlays; only remove the one that closed.
    if(!keep) g.overlays.erase(g.overlays.begin() + (depth - 1));
    return;
  }
#if CHIRAL_UI_MOBILE
  if(desktop_ui() && mob::key(a)) return;  // the mobile shell, which hands the pages' keys on to them
#elif CHIRAL_UI_DESKTOP
  if(desk::key(a)) return;  // the desktop pages; the file manager and client take their own
#endif
  switch(g.mode) {
  case MODE_HOME:
    handle_home(a);
    return;
  case MODE_FILES:
    handle_fm(a);
    return;
  case MODE_FTPD:
  case MODE_WEB:
    if(a == ACT_CIRCLE) go_mode(MODE_HOME);
    else if(a == ACT_OPTIONS) options_menu(true);
    else rows_input(g.mode == MODE_FTPD ? server_rows() : web_rows(), a);
    return;
  default:
    break;
  }
  Pane &p = active();
  bool servers = p.remote && !g.connected;
  int count = servers ? (int)g.servers.size() : (int)p.entries.size();
  int page = std::max(1, visible_rows() - 1);

  /* The Disconnect button in the server pane's header. Only reachable from
     the top of that pane's list, and anything that leaves the pane drops
     it, so it can never hold focus somewhere it isn't drawn. */
  const bool can_disconnect = p.remote && g.connected;
  if(!can_disconnect) g.disconnect_focus = false;
  if(g.disconnect_focus) {
    switch(a) {
    case ACT_DOWN:
      g.disconnect_focus = false;
      return;
    case ACT_CROSS:
      g.disconnect_focus = false;
      disconnect();
      return;
    case ACT_LEFT: case ACT_L1: case ACT_R1:
      g.disconnect_focus = false;
      g.active = 0;
      return;
    case ACT_CIRCLE:
      g.disconnect_focus = false;
      return;
    case ACT_OPTIONS:
      g.disconnect_focus = false;
      main_menu();
      return;
    case ACT_UP:
      return;  // already as far up as it goes
    default:
      g.disconnect_focus = false;
      break;  // anything else behaves as usual
    }
  }

  switch(a) {
  case ACT_UP:
    if(can_disconnect && p.cursor == 0) {
      g.disconnect_focus = true;  // up off the first row lands on Disconnect
      break;
    }
    move_cursor(p, count, -1);
    break;
  case ACT_DOWN: move_cursor(p, count, 1); break;
  case ACT_L2: move_cursor(p, count, -page); break;
  case ACT_R2: move_cursor(p, count, page); break;
  case ACT_LEFT: g.active = 0; break;
  case ACT_RIGHT: g.active = 1; break;
  case ACT_L1: case ACT_R1: g.active = 1 - g.active; break;
  case ACT_OPTIONS: main_menu(); break;
  case ACT_TOUCHPAD: push(new QueueView); break;
  case ACT_CROSS:
    if(servers) server_action(a);
    else open_entry(p);
    break;
  case ACT_TRIANGLE:
    if(servers) server_action(a);
    else if(!p.path.empty()) transfer(p);
    break;
  case ACT_SQUARE:
    if(servers) {
      server_action(a);
    } else if(p.cursor < (int)p.entries.size() && !p.entries[p.cursor].drive) {
      const std::string &n = p.entries[p.cursor].name;
      if(p.marked.count(n)) p.marked.erase(n);
      else p.marked.insert(n);
      move_cursor(p, count, 1);
    }
    break;
  case ACT_CIRCLE:
    // At the top (drive list, or the server list) Circle goes back to the start screen.
    if(servers || (!p.remote && p.path.empty())) g.mode = MODE_HOME;
    else go_up(p);
    break;
  default: break;
  }
}

void draw_overlays();

void render() {
  W = platform::view_width();
  H = platform::view_height();
  /* Settings just took this mode off a computer's start screen. */
  if(!mode_shown(g.mode)) go_mode(MODE_HOME);
  if(!mode_shown(kCards[g_home_cursor].mode)) {
    for(int i = 0; i < kCardCount; i++) if(mode_shown(kCards[i].mode)) { g_home_cursor = i; break; }
  }
  platform::set_command_shown(ACT_CMD_FILES, mode_shown(MODE_FILES));
  platform::set_command_shown(ACT_CMD_CLIENT, mode_shown(MODE_CLIENT));
  platform::set_command_shown(ACT_CMD_CONNECT, mode_shown(MODE_CLIENT));
  g_hits.clear();
  g_hit_layer = 0;
#if CHIRAL_UI_MOBILE
  /* A phone or tablet: the mobile shell, fitted to the window (ui_mobile.inc).
     A TV goes on to the console layout below. */
  if(desktop_ui()) {
    mob::render();
    draw_overlays();
    return;
  }
#elif CHIRAL_UI_DESKTOP
  /* A sidebar and pages instead of the start screen (ui_desktop.inc); the
     dialogs are the same ones, over it. */
  desk::render();
  draw_overlays();
  return;
#endif
  if(g.mode != MODE_CLIENT) {
    if(g.mode == MODE_HOME) draw_home();
    else if(g.mode == MODE_FILES) draw_fm();
    else if(g.mode == MODE_FTPD) draw_ftpd();
    else draw_web();
    draw_overlays();
    return;
  }
  draw_client();
  draw_overlays();
}

void draw_overlays() {
  for(size_t i = 0; i < g.overlays.size(); i++) {
    g_hit_layer = (int)i + 1;
    g.overlays[i]->draw();
  }
  g_hit_layer = 0;
  // Host text entry consumes keystrokes while it's on top.
  if(!g.overlays.empty() && g.overlays.back()->kind() == OV_TEXT) {
    TextEntry *t = static_cast<TextEntry *>(g.overlays.back().get());
    {
      if(!t->typing()) {
        for(size_t i = 0; i < g.overlays.size(); i++) {
          if(g.overlays[i].get() == t) {
            g.overlays.erase(g.overlays.begin() + i);
            break;
          }
        }
      }
    }
  }
  if(now() < g.toast_until && !g.toast.empty() && g.toast_err) {
    // Errors also flash on top so they're not missed behind an overlay.
    ImVec2 ts = draw::text_size(FS, g.toast.c_str());
    float tw = std::min(W - 200, ts.x + 60), tx = (W - tw) / 2, ty = H - HINT_H - STATUS_H - 90;
    draw::rect(tx, ty, tw, 64, IM_COL32(70, 24, 30, 240), 14);
    draw::text_clip(tx + 30, ty + 16, FS, draw::TEXT, g.toast.c_str(), tw - 60);
  }
}

void set_startup_notice(bool ok, const std::string &line, const std::string &detail, int status) {
  g_launch_known = true;
  g_launch_full = ok;
  g_launch_line = line;
  g_launch_detail = detail;
  g_launch_status = status;
}

void stop_workers() {
  g_jobs.stop();
}

bool quit_requested() {
  return g.quit;
}

bool busy() {
  {
    discovery_outcome_t o;
    discovery_get_outcome(&o);
    for(auto &ov : g.overlays) if(ov->kind() == OV_PROMPT || ov->kind() == OV_QUICK) return true;
    if(o.state == DISCOVERY_ASKING) return true;
  }
#if CHIRAL_UI_DESKTOP
  if(desk::editing()) return true;  // the caret blinks
#endif
#if CHIRAL_UI_MOBILE
  if(desktop_ui() && mob::busy()) return true;  // a list still settling after a scroll
#endif
  ftpd_stats_t st;
  server_stats(&st);
  if(g.mode == MODE_FTPD && st.transfers) return true;
  fsjob_stats_t js;
  fsjob_get_stats(&js);
  if(g.mode == MODE_FILES && js.running) return true;
  if(g_touch_dirty) {
    // One more frame, so the last tap is drawn before we go quiet again.
    g_touch_dirty = false;
    return true;
  }
  return g.qs.running > 0 || g_jobs.pending() > 0 || now() < g.toast_until ||
         (!g.overlays.empty() && g.overlays.back()->kind() == OV_TEXT);
}

}  // namespace ui
