// Controller-first FileZilla-style UI: PS5 storage on the left, the server on
// the right, a transfer queue on the touchpad, menus on OPTIONS.

#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <thread>
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
#include "strbuf.h"
#include "websrv.h"
}

namespace ui {
namespace {

const float W = 1920, H = 1080;
const float TOP_H = 96, HINT_H = 76, STATUS_H = 46;
const float PANE_Y = TOP_H + 12, PANE_GAP = 20, MARGIN = 36;
const float ROW_H = 54, PANE_HEAD = 84, PANE_FOOT = 46;
const float FS = 28, FS_SMALL = 23, FS_TITLE = 34;

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
  void start() {
    thread_ = std::thread([this] { loop(); });
    thread_.detach();
  }

  void run(std::function<void()> work, std::function<void()> then) {
    std::lock_guard<std::mutex> lock(mutex_);
    todo_.push_back({work, then});
    pending_++;
    cv_.notify_one();
  }

  void poll() {
    std::deque<std::function<void()>> ready;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ready.swap(done_);
    }
    for(auto &f : ready) f();
  }

  int pending() {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_;
  }

 private:
  struct Job {
    std::function<void()> work, then;
  };

  void loop() {
    for(;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !todo_.empty(); });
        job = todo_.front();
        todo_.pop_front();
      }
      job.work();
      std::lock_guard<std::mutex> lock(mutex_);
      done_.push_back([this, then = job.then] {
        {
          std::lock_guard<std::mutex> l(mutex_);
          pending_--;
        }
        if(then) then();
      });
    }
  }

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
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

enum OverlayKind { OV_OTHER, OV_QUEUE, OV_LOG, OV_TEXT };

struct Overlay {
  virtual ~Overlay() {}
  virtual OverlayKind kind() { return OV_OTHER; }
  virtual bool input(Action a) = 0;  // false closes the overlay
  virtual void draw() = 0;
};

enum Mode { MODE_HOME, MODE_CLIENT, MODE_FILES, MODE_FTPD, MODE_WEB };

struct App {
  Mode mode = MODE_HOME;
  Pane panes[2];
  Pane fm[2];  // the file manager: two PS5 panes
  int fm_active = 0;
  unsigned long fm_finished = 0;
  int active = 0;
  bool connected = false;
  ftp_server_t server;
  std::vector<json::Object> sites, history;
  bool show_hidden = true, save_pass = true;
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
  if(store_write(name, s.c_str(), s.size())) show_toast(tr("Could not save to the PS5"), true);
}

void load_all() {
  g.sites = load_store("sites");
  g.history = load_store("history");
  std::vector<json::Object> prefs = load_store("prefs");
  if(!prefs.empty()) {
    g.show_hidden = json::get_bool(prefs[0], "show_hidden", true);
    g.save_pass = json::get_bool(prefs[0], "save_pass", true);
  }
}

void save_prefs() {
  json::Object o;
  json::set_bool(o, "show_hidden", g.show_hidden);
  json::set_bool(o, "save_pass", g.save_pass);
  json::set_str(o, "language", i18n_choice());
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

void sort_entries(std::vector<Entry> &v) {
  std::sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) {
    if(a.dir != b.dir) return a.dir;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
}

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

// Lists a PS5 folder into a pane: the client's left side by default, or a
// file manager pane.
void load_local(const std::string &path, bool keep = false, Pane *target = nullptr) {
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

void load_remote(const std::string &path, bool keep = false) {
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
        json::set_str(site, "protocol", srv->protocol == FTP_PROTO_SFTP ? "sftp" : "ftp");
        json::set_num(site, "port", srv->port);
        add_history(site);
        show_toast(trf("Connected to {0}", {srv->host}));
        std::string dir = json::get_str(site, "remote_dir");
        remote().path = "\x01";  // force a fresh pane state
        load_remote(dir.empty() ? *home : dir);
        std::string ldir = json::get_str(site, "local_dir");
        if(!ldir.empty()) load_local(ldir);
        g.active = 1;
      });
}

void disconnect() {
  g_jobs.run([] { session_disconnect(); }, [] {
    g.connected = false;
    remote().path.clear();
    remote().entries.clear();
    remote().marked.clear();
    rebuild_servers();
    show_toast(tr("Disconnected"));
  });
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
    float w = 1100, h = 250, x = (W - w) / 2, y = (H - h) / 2;
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 150));
    draw::rect(x, y, w, h, draw::PANEL, 18);
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
  std::string title, subtitle;
  std::vector<MenuItem> items;
  int cursor = 0;

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
    float row = 62, w = 760;
    float h = 120 + row * items.size() + (subtitle.empty() ? 0 : 36);
    float x = (W - w) / 2, y = (H - h) / 2;
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 140));
    draw::rect(x, y, w, h, draw::PANEL, 18);
    draw::text_clip(x + 36, y + 28, FS_TITLE, draw::TEXT, title.c_str(), w - 72, true);
    float yy = y + 84;
    if(!subtitle.empty()) {
      draw::text_clip(x + 36, yy - 8, FS_SMALL, draw::MUTED, subtitle.c_str(), w - 72);
      yy += 36;
    }
    for(int i = 0; i < (int)items.size(); i++) {
      bool on = i == cursor;
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
    float w = 980, h = 300, x = (W - w) / 2, y = (H - h) / 2;
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 150));
    draw::rect(x, y, w, h, draw::PANEL, 18);
    draw::text(x + 40, y + 30, FS_TITLE, draw::TEXT, title.c_str(), true);
    draw::text_clip(x + 40, y + 96, FS, draw::MUTED, text.c_str(), w - 80);
    const char *labels[2] = {yes.c_str(), tr("Cancel")};
    for(int i = 0; i < 2; i++) {
      float bx = x + w - 40 - (2 - i) * 250 + 10;
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
    case LOCAL_DIR: return tr("Start in PS5 folder");
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
    float row = 60, w = 1240;
    int text_rows = (int)f.size() - 3;
    float h = 130 + row * text_rows + 110;
    float x = (W - w) / 2, y = (H - h) / 2;
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 150));
    draw::rect(x, y, w, h, draw::PANEL, 18);
    draw::text(x + 40, y + 28, FS_TITLE, draw::TEXT,
               site_index >= 0 ? trf("Edit site: {0}", {json::get_str(site, "name")}).c_str() : tr("Connect to a server"), true);
    float yy = y + 96;
    for(int fi : f) {
      if(fi >= CONNECT) break;
      bool on = fi == cursor;
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
      draw::rect(bx, yy + 24, bw, 64, on ? (i == 0 ? draw::ACCENT : draw::SEL) : draw::PANEL2, 12);
      ImVec2 ts = draw::text_size(FS, labels[i], true);
      draw::text(bx + (bw - ts.x) / 2, yy + 40, FS, draw::TEXT, labels[i], true);
    }
  }
};

/* ---------- settings ---------- */

struct Settings : Overlay {
  int cursor = 0;
  enum { LANG, CONNS, POLICY, RETRIES, MTIME, HIDDEN, SAVEPASS, KEYS, LOGVIEW, COUNT };

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
    switch(cursor) {
    case LANG: change_language(dir); return;
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

  bool input(Action a) override;

  void draw() override {
    settings_t s = settings_get();
    float row = 66, w = 1240, h = 140 + row * (float)COUNT + 74;
    float x = (W - w) / 2, y = (H - h) / 2;
    char buf[64];
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 150));
    draw::rect(x, y, w, h, draw::PANEL, 18);
    draw::text(x + 40, y + 28, FS_TITLE, draw::TEXT, tr("Settings"), true);
    struct { const char *label; std::string value; } rows[COUNT];
    rows[LANG] = {tr("Language"), "<  " + language_value() + "  >"};
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
    float yy = y + 100;
    for(int i = 0; i < COUNT; i++) {
      bool on = i == cursor;
      if(on) draw::rect(x + 20, yy, w - 40, row - 6, draw::SEL, 10);
      draw::text(x + 44, yy + 15, FS, draw::TEXT, rows[i].label, on);
      ImVec2 ts = draw::text_size(FS, rows[i].value.c_str());
      draw::text(x + w - 60 - ts.x, yy + 15, FS, i == CONNS || i == LANG ? draw::ACCENT2 : draw::MUTED, rows[i].value.c_str());
      yy += row;
    }
    const char *hint = cursor == CONNS ? tr("Each transfer logs in with its own connection, like FileZilla. Up to 30; the queue backs off if the server allows fewer.")
                       : cursor == LANG ? tr("Automatic follows the PS5's system language.")
                                        : tr("Left/Right to change. Settings are saved on the PS5 right away.");
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
    hx += draw::hint(hx, hy, ACT_UP, tr("Scroll"));
    hx += draw::hint(hx, hy, ACT_TRIANGLE, tr("Clear"));
    draw::hint(hx, hy, ACT_CIRCLE, tr("Close"));
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
    float w = 1400, h = 900, x = (W - w) / 2, y = (H - h) / 2;
    draw::rect(0, 0, W, H, IM_COL32(0, 0, 0, 160));
    draw::rect(x, y, w, h, draw::PANEL, 18);
    draw::brand_mark(x + 50, y + 34, 84);
    float ww = draw::brand_wordmark(x + 150, y + 44, 56);
    draw::text(x + 150 + ww + 24, y + 66, FS, draw::MUTED, trf("version {0}", {VERSION_TAG}).c_str());
    struct Line { float size; ImU32 col; bool bold; const char *text; };
    const Line lines[] = {
        {FS, draw::TEXT, false, N_("The ultimate file manager and FTP interface for PS5:")},
        {FS_SMALL, draw::MUTED, false, N_("  FTP server: other devices connect to the PS5 with any FTP app.")},
        {FS_SMALL, draw::MUTED, false, N_("  Web app: manage the PS5's files from a browser, upload and download.")},
        {FS_SMALL, draw::MUTED, false, N_("  File manager: copy, move and delete between internal storage and USB.")},
        {FS_SMALL, draw::MUTED, false, N_("  FTP / SFTP client: up to 30 transfers at once with your laptop, PC or NAS.")},
        {12, 0, false, ""},
        {FS, draw::ACCENT2, true, N_("Made by Herbrax (Simo Hakim)")},
        {FS_SMALL, draw::MUTED, false, "simohakim.com  \xc2\xb7  linkedin.com/in/simo-hakim"},
        {FS, draw::TEXT, false, N_("Found a bug? Please report it on GitHub. Pull requests are welcome.")},
        {FS_SMALL, draw::ACCENT2, false, "github.com/Herbrax/Chiral-FTP-PS5"},
        {FS_SMALL, draw::MUTED, false, N_("Tested on PS5 firmware 13.60 with ShadowMountPlus.")},
        {12, 0, false, ""},
        {FS_SMALL, draw::TEXT, true, N_("Built with open-source work by others, thank you:")},
        {FS_SMALL, draw::MUTED, false, "ps5-native-app-boilerplate and ProsperoEden (BlackBearReloaded), Lapy (ArkSama, mpereiraesaa),"},
        {FS_SMALL, draw::MUTED, false, "ftpsrv (John T\xc3\xb6rnblom), libssh2, Mbed TLS, GNU libmicrohttpd, Dear ImGui, Noto Sans, IBM Plex Mono."},
        {FS_SMALL, draw::MUTED, false, N_("chiral-ftp is free software under the GPL-3.0; licenses are in the app's licenses folder.")},
    };
    float yy = y + 140;
    int first = scroll;
    for(int i = first; i < (int)(sizeof(lines) / sizeof(lines[0])); i++) {
      if(lines[i].text[0]) draw::text_clip(x + 50, yy, lines[i].size, lines[i].col, tr(lines[i].text), w - 100, lines[i].bold);
      yy += lines[i].size + 17;
      if(yy > y + h - 100) break;
    }
    draw::hint(x + 50, y + h - 70, ACT_CIRCLE, tr("Close"));
  }
};

bool Settings::input(Action a) {
  if(a == ACT_CIRCLE || a == ACT_OPTIONS) return false;
  if(a == ACT_UP) cursor = (cursor + COUNT - 1) % COUNT;
  if(a == ACT_DOWN) cursor = (cursor + 1) % COUNT;
  if(a == ACT_LEFT) change(-1);
  if(a == ACT_RIGHT) change(1);
  if(a == ACT_CROSS) {
    if(cursor == KEYS) {
      confirm(tr("Forget host keys"), tr("Forget every saved SFTP server key? The next connection trusts the server's current key."),
              tr("Forget"), [] { show_toast(ftp_forget_host_keys() ? tr("Could not remove the keys") : tr("Saved host keys forgotten")); });
    } else if(cursor == LOGVIEW) {
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
    const char *tabs[3] = {tr("Queued"), tr("Failed"), tr("Successful")};
    int counts[3] = {g.qs.queued, g.qs.failed, g.qs.done};
    float tx = x + 36;
    draw::button(tx, y + 24, 40, ACT_L1);
    tx += 70;
    for(int i = 0; i < 3; i++) {
      char buf[64];
      snprintf(buf, sizeof(buf), "%s  %d", tabs[i], counts[i]);
      ImVec2 ts = draw::text_size(FS, buf, true);
      if(i == tab) draw::rect(tx - 16, y + 18, ts.x + 32, 54, draw::SEL, 12);
      draw::text(tx, y + 28, FS, i == tab ? draw::TEXT : draw::MUTED, buf, true);
      tx += ts.x + 56;
    }
    draw::button(tx - 20, y + 24, 40, ACT_R1);
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
      const char *msg = tab == 0 ? tr("No transfers. Mark files and press triangle to send them to the other side.") :
                        tab == 1 ? tr("No failed transfers.") : tr("Finished transfers appear here.");
      draw::text(x + 40, yy + 20, FS, draw::MUTED, msg);
    }
    for(int i = start; i < (int)items.size() && i < start + rows; i++) {
      queue_view_t &it = items[i];
      bool on = i == cursor;
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
    show_toast(from.remote ? tr("Open a PS5 folder on the left first") : tr("Open a server folder first"), true);
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
  snprintf(buf, sizeof(buf), "%s", trf(from.remote ? N_("Downloading {0} items  ·  press TOUCHPAD to watch")
                                                   : N_("Uploading {0} items  ·  press TOUCHPAD to watch"), {std::to_string(added)}).c_str());
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
                                      : N_("Permanently delete {0} from the PS5? Folders are deleted with everything inside."), {what}),
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

void chmod_items(Pane &p) {
  std::vector<Entry> items = selection(p);
  if(items.empty()) return;
  bool is_remote = p.remote;
  std::string dir = p.path;
  std::string cur = items[0].perm.size() == 4 ? items[0].perm.substr(1) : "644";
  Pane *pp = &p;
  prompt(tr("Permissions (octal, e.g. 755)"), cur, TEXT_NUMBER, [items, is_remote, dir, pp](std::string mode) {
    if(mode.size() < 3 || mode.size() > 4 || mode.find_first_not_of("01234567") != std::string::npos) {
      show_toast(tr("Use an octal mode like 755"), true);
      return;
    }
    unsigned m = (unsigned)strtoul(mode.c_str(), nullptr, 8);
    if(is_remote) {
      auto paths = std::make_shared<std::vector<std::string>>();
      for(auto &e : items) paths->push_back(join(dir, e.name));
      remote_op([paths, m](char *e, size_t n) {
        for(auto &p : *paths) {
          PathCtx x{p, "", m, nullptr};
          if(session_run(run_chmod, &x, e, n)) return -1;
        }
        return 0;
      }, tr("Permissions changed"));
    } else {
      for(auto &e : items) chmod(join(dir, e.name).c_str(), m);
      load_local(dir, true, pp);
    }
  });
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
    m->items.push_back({p.remote ? tr("Download to PS5") : tr("Upload to server"), [pp] { transfer(*pp); }, g.connected});
  }
  m->items.push_back({p.marked.count(e.name) ? tr("Unmark") : tr("Mark"), [pp, e] {
    if(pp->marked.count(e.name)) pp->marked.erase(e.name);
    else pp->marked.insert(e.name);
  }});
  m->items.push_back({tr("Rename"), [pp, e] { rename_item(*pp, e); }});
  m->items.push_back({tr("Permissions"), [pp] { chmod_items(*pp); }});
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

// Draws one pane: the client's PS5 side (idx 0) or server side (idx 1), or a
// file manager pane (fm: both are PS5 folders).
void draw_pane(Pane &p, int idx, bool fm, float x, float y, float w, float h) {
  bool focus = (fm ? g.fm_active : g.active) == idx && g.overlays.empty();
  bool server_side = !fm && idx == 1;
  draw::rect(x, y, w, h, draw::PANEL, 16);
  if(focus) draw::frame(x, y, w, h, draw::ACCENT, 16, 3);

  // Header: which side, and where we are.
  const char *who = !server_side ? "PS5" : (g.connected ? tr("Server") : tr("Servers"));
  draw::text(x + 28, y + 14, FS_TITLE, focus ? draw::TEXT : draw::MUTED, who, true);
  std::string where;
  if(!server_side) where = p.path.empty() ? tr("Choose a drive") : p.path;
  else where = g.connected ? std::string(g.server.protocol == FTP_PROTO_SFTP ? "sftp://" : "ftp://") + g.server.host + "  " + p.path
                           : tr("Connect to your laptop, PC or NAS");
  float wx = x + 32 + draw::text_size(FS_TITLE, who, true).x + 20;
  draw::text_clip(wx, y + 22, FS_SMALL, draw::MUTED, where.c_str(), x + w - wx - 70);
  if(p.loading) draw::spinner(x + w - 40, y + 38, 14, draw::ACCENT);
  draw::rect(x + 16, y + PANE_HEAD - 6, w - 32, 2, draw::LINE);

  float ly = y + PANE_HEAD;
  if(server_side && !g.connected) {
    draw_server_list(x, ly, w);
  } else {
    int rows = visible_rows();
    int n = (int)p.entries.size();
    if(p.cursor < p.scroll) p.scroll = p.cursor;
    if(p.cursor >= p.scroll + rows) p.scroll = p.cursor - rows + 1;
    if(p.scroll > std::max(0, n - rows)) p.scroll = std::max(0, n - rows);
    float yy = ly;
    if(!n && !p.loading) draw::text(x + 28, yy + 20, FS, draw::MUTED, p.path.empty() ? tr("No drives found") : tr("Empty folder"));
    for(int i = p.scroll; i < n && i < p.scroll + rows; i++) {
      Entry &e = p.entries[i];
      bool on = i == p.cursor;
      bool marked = p.marked.count(e.name) > 0;
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
      draw::text_clip(nx, yy + 11, FS, draw::TEXT, e.drive ? e.detail.c_str() : name.c_str(), name_w, on);
      if(e.drive) {
        draw::text_clip(nx + name_w * 0.62f, yy + 15, FS_SMALL, draw::MUTED, e.name.c_str(), name_w * 0.38f);
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
      x += draw::hint(x, y, ACT_CROSS, tr("Options"));
      x += draw::hint(x, y, ACT_TRIANGLE, g.qs.paused ? tr("Resume") : tr("Pause"));
      x += draw::hint(x, y, ACT_SQUARE, tab == 0 ? tr("Cancel all") : tab == 1 ? tr("Retry all") : tr("Clear list"));
      x += draw::hint(x, y, ACT_L1, tr("Tabs"));
      draw::hint(x, y, ACT_CIRCLE, tr("Close"));
    } else if(g.overlays.back()->kind() != OV_LOG) {
      x += draw::hint(x, y, ACT_CROSS, tr("Select"));
      x += draw::hint(x, y, ACT_LEFT, tr("Change"));
      draw::hint(x, y, ACT_CIRCLE, tr("Back"));
    }
    return;
  }
  Pane &p = active();
  if(p.remote && !g.connected) {
    x += draw::hint(x, y, ACT_CROSS, tr("Connect"));
    x += draw::hint(x, y, ACT_TRIANGLE, tr("Edit"));
    x += draw::hint(x, y, ACT_SQUARE, tr("Delete"));
    x += draw::hint(x, y, ACT_CIRCLE, tr("Start screen"));
  } else {
    bool on_dir = p.cursor < (int)p.entries.size() && p.entries[p.cursor].dir;
    x += draw::hint(x, y, ACT_CROSS, on_dir ? tr("Open") : tr("Actions"));
    x += draw::hint(x, y, ACT_SQUARE, tr("Mark"));
    if(g.connected && !p.path.empty()) {
      char t[64];
      size_t n = p.marked.empty() ? 1 : p.marked.size();
      snprintf(t, sizeof(t), "%s", trf(p.remote ? N_("Download {0}") : N_("Upload {0}"), {std::to_string(n)}).c_str());
      x += draw::hint(x, y, ACT_TRIANGLE, t);
    }
    x += draw::hint(x, y, ACT_CIRCLE, !p.remote && p.path.empty() ? tr("Start screen") : tr("Back"));
  }
  x += draw::hint(x, y, ACT_R1, tr("Switch side"));
  x += draw::hint(x, y, ACT_OPTIONS, tr("Menu"));
  draw::hint(x, y, ACT_TOUCHPAD, tr("Transfers"));
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
};

ServerPrefs g_srv;
std::string g_ip = "unknown";
double g_ip_at = -100;

void load_server_prefs() {
  ftpd_config_default(&g_srv.ftp);
#ifndef __PROSPERO__
  // On a computer the server shares a test folder, not the whole disk.
  const char *root = getenv("CHIRAL_FTPD_ROOT");
  snprintf(g_srv.ftp.root, sizeof(g_srv.ftp.root), "%s", root && *root ? root : data_dir());
#endif
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
}

void save_server_prefs() {
  json::Object o;
  json::set_num(o, "ftp_port", g_srv.ftp.port);
  json::set_str(o, "ftp_user", g_srv.ftp.user);
  json::set_str(o, "ftp_pass", g_srv.ftp.pass);
  json::set_bool(o, "ftp_anonymous", g_srv.ftp.anonymous);
  json::set_num(o, "web_port", g_srv.web_port);
  save_store("servers", {o});
  ftpd_set_config(&g_srv.ftp);
}

// The IP can change (Wi-Fi reconnects), so look again every few seconds.
const std::string &local_ip() {
  if(now() - g_ip_at > 5) {
    char buf[64];
    net_local_ip(buf, sizeof(buf));
    g_ip = buf;
    g_ip_at = now();
  }
  return g_ip;
}

std::string ftp_address() {
  return "ftp://" + local_ip() + ":" + std::to_string(g_srv.ftp.port);
}

std::string web_address() {
  return "http://" + local_ip() + ":" + std::to_string(g_srv.web_port) + "/";
}

void start_ftpd() {
  if(ftpd_start(&g_srv.ftp)) {
    show_toast(errno == EADDRINUSE ? trf("Could not start the FTP server: {0} (port in use, choose another one)", {strerror(errno)})
                                   : trf("Could not start the FTP server: {0}", {strerror(errno)}), true);
  } else {
    show_toast(trf("FTP server running at {0}", {ftp_address()}));
  }
}

void start_web() {
  if(websrv_start((unsigned short)g_srv.web_port)) {
    show_toast(errno == EADDRINUSE ? trf("Could not start the web app: {0} (port in use, choose another one)", {strerror(errno)})
                                   : trf("Could not start the web app: {0}", {strerror(errno)}), true);
  } else {
    show_toast(trf("Web app running at {0}", {web_address()}));
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
int g_row_cursor = 0;

void draw_rows(const std::vector<Row> &rows, float x, float y, float w, float row = 70) {
  for(int i = 0; i < (int)rows.size(); i++) {
    const Row &r = rows[i];
    bool on = i == g_row_cursor && g.overlays.empty();
    float yy = y + i * row;
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

std::vector<Row> ftpd_rows() {
  std::vector<Row> rows;
  bool running = ftpd_running();
  rows.push_back({running ? tr("Stop the FTP server") : tr("Start the FTP server"), "",
                  [running] { if(running) ftpd_stop(); else start_ftpd(); }, true});
  rows.push_back({tr("Username"), g_srv.ftp.user, [] {
    prompt(tr("FTP username"), g_srv.ftp.user, TEXT_DEFAULT, [](std::string v) {
      if(v.empty()) return show_toast(tr("The username can't be empty"), true);
      snprintf(g_srv.ftp.user, sizeof(g_srv.ftp.user), "%s", v.c_str());
      save_server_prefs();
      show_toast(tr("Username saved; new logins use it"));
    });
  }});
  rows.push_back({tr("Password"), g_srv.ftp.pass, [] {
    prompt(tr("FTP password"), g_srv.ftp.pass, TEXT_DEFAULT, [](std::string v) {
      if(v.empty()) return show_toast(tr("The password can't be empty; turn on anonymous login instead"), true);
      snprintf(g_srv.ftp.pass, sizeof(g_srv.ftp.pass), "%s", v.c_str());
      save_server_prefs();
      show_toast(tr("Password saved; new logins use it"));
    });
  }});
  rows.push_back({tr("Anonymous login"), g_srv.ftp.anonymous ? tr("On") : tr("Off"), [] {
    g_srv.ftp.anonymous = !g_srv.ftp.anonymous;
    save_server_prefs();
  }});
  rows.push_back({tr("Port"), running ? trf("{0}  (stop to change)", {std::to_string(g_srv.ftp.port)}) : std::to_string(g_srv.ftp.port), [] {
    prompt_port(tr("FTP server port"), g_srv.ftp.port, [](int p) {
      g_srv.ftp.port = (unsigned short)p;
      save_server_prefs();
    });
  }, false, !running});
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
  for(auto &h : hints) x += draw::hint(x, y, h.first, h.second);
}

void draw_ftpd() {
  bool running = ftpd_running();
  draw_header(tr("FTP server"), tr("Other devices connect to the PS5 with FileZilla, a file manager or any FTP app."));
  float x = MARGIN, y = TOP_H + 180, w = 860, h = H - y - HINT_H - STATUS_H - 20;
  draw::rect(x, y, w, h, draw::PANEL, 16);
  draw_status_pill(x + 30, y + 30, running, tr("Running"), tr("Stopped"));
  std::string addr = ftp_address();
  draw::text_clip(x + 30, y + 100, 46, running ? draw::TEXT : draw::MUTED, addr.c_str(), w - 60, true);
  std::string login = g_srv.ftp.anonymous ? trf("Login: {0} / {1}   or anonymous", {g_srv.ftp.user, g_srv.ftp.pass})
                                          : trf("Login: {0} / {1}", {g_srv.ftp.user, g_srv.ftp.pass});
  draw::text_clip(x + 30, y + 170, FS, draw::ACCENT2, login.c_str(), w - 60);
  draw::rect(x + 20, y + 222, w - 40, 2, draw::LINE);
  draw_rows(ftpd_rows(), x, y + 232, w, 60);

  float rx = x + w + PANE_GAP, rw = W - MARGIN - rx;
  draw::rect(rx, y, rw, h, draw::PANEL, 16);
  ftpd_stats_t st;
  ftpd_get_stats(&st);
  char line[200];
  draw::text(rx + 30, y + 28, FS_TITLE, draw::TEXT, tr("Activity"), true);
  snprintf(line, sizeof(line), "%s%s", trf("Connected: {0}  ·  Transfers: {1}", {std::to_string(st.clients), std::to_string(st.transfers)}).c_str(),
           st.transfers && st.speed > 0 ? ("  ·  " + fmt_size((long long)st.speed) + "/s").c_str() : "");
  draw::text(rx + 30, y + 84, FS, st.transfers ? draw::ACCENT2 : draw::MUTED, line);
  snprintf(line, sizeof(line), "%s", trf("Sent {0}  ·  Received {1}", {fmt_size(st.bytes_up), fmt_size(st.bytes_down)}).c_str());
  draw::text(rx + 30, y + 128, FS_SMALL, draw::MUTED, line);
  draw::rect(rx + 20, y + 175, rw - 40, 2, draw::LINE);
  char lines[16][160];
  int n = ftpd_recent(lines, 16);
  float yy = y + 192;
  if(!n) draw::text(rx + 30, yy, FS_SMALL, draw::MUTED, tr("Logins and transfers show here."));
  for(int i = 0; i < n && yy < y + h - 40; i++, yy += 36) draw::text_clip(rx + 30, yy, FS_SMALL, draw::TEXT, lines[i], rw - 60);
  draw_hints({{ACT_CROSS, tr("Select")}, {ACT_CIRCLE, tr("Start screen (the server keeps running)")}, {ACT_OPTIONS, tr("Menu")}});
}

void draw_web() {
  bool running = websrv_running();
  draw_header(tr("Web app"), tr("Use chiral-ftp from a browser on your PC or phone."));
  float x = MARGIN, y = TOP_H + 180, w = W - 2 * MARGIN, h = H - y - HINT_H - STATUS_H - 20;
  draw::rect(x, y, w, h, draw::PANEL, 16);
  draw_status_pill(x + 30, y + 30, running, tr("Running"), tr("Stopped"));
  draw::text(x + 30, y + 100, FS, draw::MUTED, running ? tr("Open this address in a browser on your PC:") : tr("Start it, then open this address in a browser on your PC:"));
  draw::text_clip(x + 30, y + 150, 64, running ? draw::TEXT : draw::MUTED, web_address().c_str(), w - 60, true);
  draw::text_clip(x + 30, y + 236, FS_SMALL, draw::TEXT,
                  tr("PS5 files: browse, download to your PC, upload from it (drag and drop works), copy, move, rename, delete."),
                  w - 60);
  draw::text_clip(x + 30, y + 268, FS_SMALL, draw::MUTED,
                  tr("FTP client tab: the client from the TV, sharing its connection and queue. Anyone on your network can open the page while it runs."),
                  w - 60);
  draw::rect(x + 20, y + 314, w - 40, 2, draw::LINE);
  float rw = 900;
  draw_rows(web_rows(), x, y + 330, rw);
  draw_hints({{ACT_CROSS, tr("Select")}, {ACT_CIRCLE, tr("Start screen (the web app keeps running)")}, {ACT_OPTIONS, tr("Menu")}});
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
  m->items.push_back({tr("Permissions"), [pp] { chmod_items(*pp); }, on_item});
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
    x += draw::hint(x, y, ACT_CROSS, tr("Select"));
    draw::hint(x, y, ACT_CIRCLE, tr("Back"));
    return;
  }
  Pane &p = g.fm[g.fm_active];
  bool on_dir = p.cursor < (int)p.entries.size() && p.entries[p.cursor].dir;
  x += draw::hint(x, y, ACT_CROSS, on_dir ? tr("Open") : tr("Actions"));
  x += draw::hint(x, y, ACT_SQUARE, tr("Mark"));
  if(!p.path.empty()) {
    char t[64];
    size_t n = p.marked.empty() ? 1 : p.marked.size();
    snprintf(t, sizeof(t), "%s", trf("Copy {0} to the other side", {std::to_string(n)}).c_str());
    x += draw::hint(x, y, ACT_TRIANGLE, t);
  }
  x += draw::hint(x, y, ACT_CIRCLE, p.path.empty() ? tr("Start screen") : tr("Back"));
  x += draw::hint(x, y, ACT_R1, tr("Switch side"));
  draw::hint(x, y, ACT_OPTIONS, tr("Menu: move, rename, delete..."));
}

struct HomeCard {
  const char *title, *text;
  Mode mode;
};

const int kCardCount = 4;
const HomeCard kCards[kCardCount] = {
    {N_("FTP server"), N_("Turn the PS5 into an FTP server. Connect to it from your PC, phone or another console."), MODE_FTPD},
    {N_("Web app"), N_("Open chiral-ftp in a browser on your PC: manage the PS5's files, upload and download."), MODE_WEB},
    {N_("File manager"), N_("Copy and move files on the PS5: internal storage, USB drives and extended storage."), MODE_FILES},
    {N_("FTP / SFTP client"), N_("Connect from the PS5 to your laptop, PC or NAS and transfer files both ways."), MODE_CLIENT},
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

// Set once at startup by main (PS5 only).
bool g_access_known = false, g_access_full = false;
std::string g_access_detail;  // English, translated when drawn
int g_access_status = 0;

void draw_access_banner(float y) {
  if(!g_access_known) return;
  // How access was granted only goes to the startup log.
  std::string text = g_access_full ? std::string(tr("Full access to the PS5's storage"))
                                   : trf("Limited to the app's sandbox: {0} (status {1})",
                                         {tr(g_access_detail.c_str()), std::to_string(g_access_status)});
  ImVec2 ts = draw::text_size(FS_SMALL, text.c_str(), true);
  float w = std::min(W - 2 * MARGIN, ts.x + 70);
  draw::rect(MARGIN, y, w, 48, g_access_full ? IM_COL32(24, 64, 44, 255) : IM_COL32(80, 56, 18, 255), 24);
  draw::dl()->AddCircleFilled(ImVec2(MARGIN + 26, y + 24), 8, g_access_full ? draw::OK : draw::ACCENT2, 12);
  draw::text_clip(MARGIN + 46, y + 11, FS_SMALL, g_access_full ? draw::OK : draw::ACCENT2, text.c_str(), w - 66, true);
}

void draw_home() {
  draw::rect(0, 0, W, H, draw::BG);
  draw_top();
  draw::text(MARGIN + 8, TOP_H + 50, 52, draw::TEXT, tr("What would you like to do?"), true);
  draw::text(MARGIN + 8, TOP_H + 124, FS, draw::MUTED,
             tr("Servers keep running while you use the other modes or go to the home screen. Closing chiral-ftp stops them."));
  draw_access_banner(H - HINT_H - 64);
  float gap = 30, cw = (W - 2 * MARGIN - (kCardCount - 1) * gap) / kCardCount, ch = 580, cy = TOP_H + 210;
  for(int i = 0; i < kCardCount; i++) {
    Mode mode = kCards[i].mode;
    float cx = MARGIN + i * (cw + gap);
    bool on = i == g_home_cursor && g.overlays.empty();
    draw::rect(cx, cy, cw, ch, on ? draw::PANEL2 : draw::PANEL, 22);
    if(on) draw::frame(cx, cy, cw, ch, draw::ACCENT, 22, 4);
    draw_card_icon(mode, cx + cw / 2, cy + 120);
    const char *card_title = tr(kCards[i].title);
    ImVec2 ts = draw::text_size(36, card_title, true);
    draw::text(cx + (cw - ts.x) / 2, cy + 220, 36, draw::TEXT, card_title, true);
    // The description, over a few lines.
    float ty = cy + 290;
    for(const std::string &line : wrap_text(tr(kCards[i].text), FS, cw - 60)) {
      draw::text(cx + 30, ty, FS, draw::MUTED, line.c_str());
      ty += 40;
    }
    // What's running now.
    std::string status;
    bool active = false;
    fsjob_stats_t js;
    fsjob_get_stats(&js);
    if(mode == MODE_FTPD && ftpd_running()) status = ftp_address(), active = true;
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

// OPTIONS on the start screen, and on the FTP server and web app screens
// (with "Start screen" first, like every mode's menu).
void options_menu(bool in_mode) {
  Menu *m = new Menu;
  m->title = in_mode ? tr("Menu") : "chiral-ftp";
  if(in_mode) m->items.push_back({tr("Start screen"), [] { go_mode(MODE_HOME); }});
  m->items.push_back({tr("Settings"), [] { push(new Settings); }});
  m->items.push_back({tr("Message log"), [] { push(new LogView); }});
  m->items.push_back({tr("About chiral-ftp"), [] { push(new About); }});
  if(!platform::can_quit()) m->items.push_back({tr("To close chiral-ftp, use the PS button"), [] {}, false});
  push(m);
}

bool handle_home(Action a) {
  if(a == ACT_OPTIONS) {
    options_menu(false);
    return true;
  }
  if(a == ACT_LEFT || a == ACT_L1) g_home_cursor = (g_home_cursor + kCardCount - 1) % kCardCount;
  else if(a == ACT_RIGHT || a == ACT_R1) g_home_cursor = (g_home_cursor + 1) % kCardCount;
  else if(a == ACT_CROSS) {
    Mode m = kCards[g_home_cursor].mode;
    if(m == MODE_FILES && g.fm[0].path.empty() && g.fm[0].entries.empty()) {
      load_drives(g.fm[0]);
      load_drives(g.fm[1]);
    }
    go_mode(m);
  }
  return true;
}

}  // namespace

void init() {
  memset(&g.qs, 0, sizeof(g.qs));
  memset(&g.server, 0, sizeof(g.server));
  g.panes[0].remote = false;
  g.panes[1].remote = true;
  load_all();
  load_server_prefs();
  rebuild_servers();
  load_drives();
  g_jobs.start();
  ftp_server_t s;
  if(session_server(&s)) {
    g.connected = true;
    g.server = s;
    load_remote("");
  }
}

void update() {
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
  if(!g.overlays.empty()) {
    Overlay *top = g.overlays.back().get();
    size_t depth = g.overlays.size();
    bool keep = top->input(a);
    // input() may have pushed new overlays; only remove the one that closed.
    if(!keep) g.overlays.erase(g.overlays.begin() + (depth - 1));
    return;
  }
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
    else rows_input(g.mode == MODE_FTPD ? ftpd_rows() : web_rows(), a);
    return;
  default:
    break;
  }
  Pane &p = active();
  bool servers = p.remote && !g.connected;
  int count = servers ? (int)g.servers.size() : (int)p.entries.size();
  int page = std::max(1, visible_rows() - 1);
  switch(a) {
  case ACT_UP: move_cursor(p, count, -1); break;
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
  if(g.mode != MODE_CLIENT) {
    if(g.mode == MODE_HOME) draw_home();
    else if(g.mode == MODE_FILES) draw_fm();
    else if(g.mode == MODE_FTPD) draw_ftpd();
    else draw_web();
    draw_overlays();
    return;
  }
  draw::rect(0, 0, W, H, draw::BG);
  draw_top();
  float ph = H - PANE_Y - STATUS_H - HINT_H - 16;
  float pw = (W - 2 * MARGIN - PANE_GAP) / 2;
  draw_pane(g.panes[0], 0, false, MARGIN, PANE_Y, pw, ph);
  draw_pane(g.panes[1], 1, false, MARGIN + pw + PANE_GAP, PANE_Y, pw, ph);
  draw_bottom();
  draw_overlays();
}

void draw_overlays() {
  for(auto &o : g.overlays) o->draw();
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

void set_storage_access(bool full, const std::string &reason, int status) {
  g_access_known = true;
  g_access_full = full;
  g_access_detail = reason;
  g_access_status = status;
}

bool quit_requested() {
  return g.quit;
}

bool busy() {
  ftpd_stats_t st;
  ftpd_get_stats(&st);
  if(g.mode == MODE_FTPD && st.transfers) return true;
  fsjob_stats_t js;
  fsjob_get_stats(&js);
  if(g.mode == MODE_FILES && js.running) return true;
  return g.qs.running > 0 || g_jobs.pending() > 0 || now() < g.toast_until ||
         (!g.overlays.empty() && g.overlays.back()->kind() == OV_TEXT);
}

}  // namespace ui
