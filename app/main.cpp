// chiral-ftp: a FileZilla-style FTP/SFTP client for the PS5, built as a native
// ShadowMountPlus title. The same code runs on a computer for development.
//
// Automated UI tests on a computer:
//   chiral-ftp-host --headless --script "DOWN CROSS wait=500 shot=a.bmp ..."
// Tokens are action names (UP, CROSS, R1, OPTIONS...), wait=<ms>, shot=<file>,
// type=<text> (host text entry; _ for spaces) and quit.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#ifdef __PROSPERO__
#include <ucontext.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

#include "backend.h"
#include "i18n.h"
#include "json.h"
#include "platform.h"
#include "ui.h"
#ifdef __PROSPERO__
#include "elevation/elevation.hpp"
#endif

extern "C" {
#include "config.h"
#include "log.h"
#include "session.h"
#include "settings.h"
#include "strbuf.h"
}

namespace {

struct Script {
  std::vector<std::string> tokens;
  size_t next = 0;
  std::uint64_t wait_until = 0;
};

Action action_named(const std::string &n) {
  static const struct { const char *name; Action a; } map[] = {
    {"UP", ACT_UP}, {"DOWN", ACT_DOWN}, {"LEFT", ACT_LEFT}, {"RIGHT", ACT_RIGHT},
    {"CROSS", ACT_CROSS}, {"CIRCLE", ACT_CIRCLE}, {"SQUARE", ACT_SQUARE}, {"TRIANGLE", ACT_TRIANGLE},
    {"L1", ACT_L1}, {"R1", ACT_R1}, {"L2", ACT_L2}, {"R2", ACT_R2},
    {"OPTIONS", ACT_OPTIONS}, {"TOUCHPAD", ACT_TOUCHPAD},
  };
  for(auto &m : map) {
    if(n == m.name) return m.a;
  }
  return ACT_NONE;
}

// One script token per frame; false when the script says quit.
bool step(Script &s) {
  if(s.next >= s.tokens.size() || backend::ticks_ms() < s.wait_until) return true;
  const std::string &t = s.tokens[s.next++];
  if(t == "quit") return false;
  if(!t.compare(0, 5, "wait=")) {
    s.wait_until = backend::ticks_ms() + (std::uint64_t)atoi(t.c_str() + 5);
  } else if(!t.compare(0, 5, "shot=")) {
    platform::save_screenshot(t.c_str() + 5);
  } else if(!t.compare(0, 5, "type=")) {
    std::string text = t.substr(5);
    for(auto &c : text) if(c == '_') c = ' ';
    platform::inject_text(text, true);
  } else {
    platform::push_action(action_named(t));
  }
  return true;
}

#ifdef __PROSPERO__
/* A crash leaves its place in the startup log: the instruction, the address
   it touched, and code addresses found on the stack (the build has no frame
   pointers to walk). Relative to this handler, they map back to the source
   with llvm-symbolizer on .build/ps5-app/llvm-pie.elf. */
void crash_handler(int sig, siginfo_t *info, void *context) {
  ucontext_t *uc = static_cast<ucontext_t *>(context);
  std::uintptr_t main_at = reinterpret_cast<std::uintptr_t>(&crash_handler);
  std::uintptr_t rip = (std::uintptr_t)uc->uc_mcontext.mc_rip;
  startup_trace("Crash: signal %d at handler%+lld, touching %p", sig, (long long)(rip - main_at), info->si_addr);
  const std::uintptr_t *sp = reinterpret_cast<const std::uintptr_t *>(uc->uc_mcontext.mc_rsp);
  char line[600];
  int len = 0, found = 0;
  for(int i = 0; i < 512 && found < 24; i++) {
    std::uintptr_t v = sp[i];
    if(v > main_at - (64u << 20) && v < main_at + (64u << 20)) {
      len += snprintf(line + len, sizeof(line) - (size_t)len, " %+lld", (long long)(v - main_at));
      found++;
    }
  }
  startup_trace("Crash stack (relative to the handler):%s", line);
  signal(sig, SIG_DFL);
  raise(sig);
}

void install_crash_handler() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  for(int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
}

#endif

}  // namespace

int main(int argc, char **argv) {
  bool headless = false;
  Script script;

  for(int i = 1; i < argc; i++) {
    if(!strcmp(argv[i], "--headless")) headless = true;
    else if(!strcmp(argv[i], "--script") && i + 1 < argc) {
      // Split on spaces (no iostreams: they pull in locale code).
      std::string all = argv[++i], t;
      for(char c : all) {
        if(c == ' ') {
          if(!t.empty()) script.tokens.push_back(t);
          t.clear();
        } else {
          t += c;
        }
      }
      if(!t.empty()) script.tokens.push_back(t);
    }
  }

#ifdef __PROSPERO__
  /* A title only sees a sandboxed view of the console's storage (/data
     looks empty). Like ProsperoEden, ask the jailbreak's resident service
     for full access, or send the packaged upstream Lapy helper to elfldr on
     port 9021. Single-threaded startup, before anything opens a file. */
  /* The keyboard's module first: after the access request the app may no
     longer load it, and every text field (new folder, rename, passwords)
     would silently do nothing (ProsperoEden loads it first too). */
  backend::prepare_keyboard();
  const elevation::Status access = elevation::request(elevation::Capability::filesystem);
  /* Elevation leaves the effective group (1) apart from the real one (0);
     match them, as ProsperoEden does. Root may set its group. */
  if(geteuid() == 0 && getegid() != getgid()) (void)setegid(getgid());
  /* What the request left behind, for the start screen's access banner:
     root (uid 0) and the system fonts folder mean the sandbox is gone. */
  char access_probe[160];
  {
    int fonts = open("/preinst/common/font", O_RDONLY | O_DIRECTORY);
    int fonts_err = fonts < 0 ? errno : 0;
    if(fonts >= 0) close(fonts);
    snprintf(access_probe, sizeof(access_probe), "uid %d/%d, fonts %s, path %s", (int)getuid(), (int)geteuid(),
             fonts_err ? strerror(fonts_err) : "visible", elevation::path());
  }
#endif
  data_dir_init();
  ensure_data_dir();
#ifdef __PROSPERO__
  install_crash_handler();
#endif
  {
    char path[600];
    unlink(data_path(path, sizeof(path), STARTUP_NAME));
  }
  startup_trace("%s %s starting, data in %s", APP_NAME, VERSION_TAG, data_dir());

#ifdef __PROSPERO__
  std::string access_detail;  // English; the banner translates it
  int access_code = static_cast<int>(access);
  bool full_access = access == elevation::Status::ok && strcmp(elevation::path(), "data-only");
  if(full_access) {
    access_detail = !strcmp(elevation::path(), "existing") ? "already granted"
                    : !strcmp(elevation::path(), "resident") ? "granted by the jailbreak's resident service"
                                                             : "granted by the Lapy helper (elfldr, port 9021)";
    startup_trace("Full storage access: %s", access_detail.c_str());
  } else {
    switch(access) {
    case elevation::Status::ok: access_detail = N_("nothing granted full access; only /data is writable"); break;
    case elevation::Status::unavailable: access_detail = N_("lapy.elf is missing from the app folder"); break;
    case elevation::Status::transport_error: access_detail = N_("couldn't reach elfldr on port 9021 (is it running?)"); break;
    case elevation::Status::timeout: access_detail = N_("the jailbreak didn't answer in time"); break;
    case elevation::Status::target_mismatch: access_detail = N_("lapy.elf was built for another title ID"); break;
    case elevation::Status::unsupported_version: access_detail = N_("the jailbreak's Lapy is a different version"); break;
    case elevation::Status::apply_failed: access_detail = N_("the helper ran but access didn't change"); break;
    default: access_detail = N_("elevation failed"); break;
    }
    log_msg(0, LOG_ERROR, "Limited storage access: %s (status %d)", access_detail.c_str(), access_code);
  }
  startup_trace("Storage access: %s (%s)", access_detail.c_str(), access_probe);
#endif
  settings_load();
  {
    /* The language, before the fonts load (they put its script first) and
       before any thread can ask for a translation. */
    strbuf_t b = {0, 0, 0};
    std::string lang = "auto";
    std::vector<json::Object> prefs;
    if(!store_read("prefs", &b) && b.data && json::parse_array(std::string(b.data, b.len), prefs) && !prefs.empty()) {
      lang = json::get_str(prefs[0], "language", "auto");
    }
    strbuf_free(&b);
    i18n_init(lang.c_str());
  }
  session_start_keepalive();

  if(!platform::init(headless)) {
    startup_trace("Could not start the display");
    for(;;) backend::pump(1000);  // a native PS5 title must not return from main
  }
  startup_trace("Display ready");
  ui::init();
#ifdef __PROSPERO__
  ui::set_storage_access(full_access, access_detail, access_code);
#endif

  bool running = true;
  std::uint64_t last_draw = 0;
  for(;;) {
    bool changed = false;
    // Wait for input, but wake up often enough for progress and spinners.
    Action a = platform::poll(ui::busy() ? 16 : 100);
    while(a != ACT_NONE) {
      ui::handle(a);
      changed = true;
      a = platform::poll(0);
    }
    if(!script.tokens.empty()) {
      running = step(script);
      changed = true;
    }
    ui::update();
    if((!running || ui::quit_requested()) && platform::can_quit()) break;
    std::uint64_t t = backend::ticks_ms();
    // Software rendering: redraw on input, while busy, and at least once a second.
    if(changed || ui::busy() || t - last_draw > 1000) {
      platform::begin_frame();
      ui::render();
      platform::end_frame();
      last_draw = t;

    }
  }
  session_disconnect();
  platform::shutdown();
  return 0;
}
