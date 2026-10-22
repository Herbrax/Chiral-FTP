// chiral-ftp: a FileZilla-style FTP/SFTP client for the Nintendo Switch,
// built as homebrew (.nro) on libnx. The same code runs on a computer for
// development, which is where most of the UI is tested.
//
// Automated UI tests on a computer:
//   chiral-ftp-host --headless --script "DOWN CROSS wait=500 shot=a.bmp ..."
// Tokens are action names (UP, CROSS, R1, OPTIONS... named after the PS5
// buttons the UI was built around; see app/backend_switch.cpp for how they
// map onto A/B/X/Y), wait=<ms>, shot=<file>,
// type=<text> (host text entry; _ for spaces) and quit.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "backend.h"
#include "i18n.h"
#include "json.h"
#include "platform.h"
#include "ui.h"

extern "C" {
#include "config.h"
#include "ftpd.h"
#include "log.h"
#include "queue.h"
#include "session.h"
#include "fsjobs.h"
#include "settings.h"
#include "strbuf.h"
#include "websrv.h"
#include "discovery.h"
#if CHIRAL_HAS_SFTPD
#include "sftpd.h"
#endif
}

/* The SFTP server, where the platform has one, stops alongside the FTP
   server; elsewhere these are nothing. */
#if CHIRAL_HAS_SFTPD
#define SFTPD_STOP() sftpd_stop()
#define SFTPD_RUNNING() sftpd_running()
#else
#define SFTPD_STOP() ((void)0)
#define SFTPD_RUNNING() 0
#endif

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

  /* Before anything opens a file: on the PS5 this is where the title asks
     the jailbreak for the real filesystem, and until it has, /data looks
     empty. A no-op elsewhere. */
  backend::early_init();

  data_dir_init();
  ensure_data_dir();
  {
    char path[600];
    unlink(data_path(path, sizeof(path), STARTUP_NAME));
  }
  startup_trace("%s %s starting, data in %s", APP_NAME, VERSION_TAG, data_dir());

  /* What the platform managed to bring up before main ran. It has no log
     to write to that early, so it reports here. */
  backend::report_startup_services(&startup_trace);

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
    /* Even a failed start may have taken a framebuffer before it gave up,
       and exiting while holding one is exactly what we are trying not to
       do. shutdown() is safe to call either way. */
    platform::shutdown();
    return 1;  // homebrew may return; libnx takes us back to hbmenu
  }
  startup_trace("Display ready");
  ui::init();
  /* Whatever the platform wants said about how we got here -- full storage
     access on the PS5, an application slot rather than a library applet on
     the Switch, nothing at all on a computer. */
  {
    bool ok = false;
    const char *line = "", *detail = "";
    int status = 0;
    if(backend::startup_notice(&ok, &line, &detail, &status)) {
      startup_trace("Launch: %s (%s, status %d)", ok ? "ok" : "limited", detail, status);
      ui::set_startup_notice(ok, line, detail, status);
    }
  }

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
  /* Shut the servers down before returning, and wait for them to actually
     be down.
     
     Returning from main hands control back to libnx, which runs
     userAppExit and closes the socket driver, pl:u and the rest. Anything
     still running at that point -- the FTP listener and its client
     threads, the web server's thread and libmicrohttpd's own, a transfer
     worker mid-write -- is left holding sockets and handles that have just
     been taken away, and the process falls over on the way out instead of
     returning to hbmenu. ftpd_stop() and websrv_stop() only ask; their
     threads notice within a few hundred milliseconds. */
  startup_trace("Stopping the servers");
  ftpd_stop();
  SFTPD_STOP();
  websrv_stop();
  discovery_stop();  // quick connect's thread, which owns a socket too
  queue_stop();      // the client's transfer workers: mid-transfer, or idle for up to 30 s more
  auto still_up = [] {
    return ftpd_running() || SFTPD_RUNNING() || websrv_running() || discovery_running() || queue_workers();
  };
  /* By the clock: pump() comes back early for input, so counting its calls
     would cut the wait short. */
  for(const std::uint64_t until = backend::ticks_ms() + 4000; still_up() && backend::ticks_ms() < until;) {
    backend::pump(50);
  }
  if(still_up()) {
    /* Not worth hanging on a thread that will not come back: say so, and
       let the exit happen anyway. */
    startup_trace("A server did not stop in time (ftp %d, sftp %d, web %d, quick connect %d, transfers %d)",
                  ftpd_running(), SFTPD_RUNNING(), websrv_running(), discovery_running(), queue_workers());
  }
  session_disconnect();

  /* Then the threads that outlive everything else.

     Returning from main runs the static destructors, and those take apart
     the UI state, the job queues and the translation tables. Any thread of
     ours still alive at that point is reading objects as they are being
     destroyed, which is why leaving the full app crashed while the UI-only
     build (no engine, no threads) exited cleanly. The queue's transfer
     workers were stopped and waited for above; these three have stops of
     their own. */
  startup_trace("Stopping the background threads");
  ui::stop_workers();
  fsjob_stop();
  session_stop_keepalive();

  platform::shutdown();
  startup_trace("Exiting");
  return 0;
}
