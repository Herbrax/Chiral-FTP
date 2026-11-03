// Updates: at startup, a question to chiral-ftp.com's update API -- is there
// a newer version for this platform? -- and, when the user says yes, the new
// version downloaded and put in place of this copy, then a restart.
//
// Only where the platform can replace itself (CHIRAL_HAS_UPDATER): for now
// the computers -- the .exe, the .app, the AppImage. Everywhere else these
// do nothing, so the UI calls them without asking.
//
// Test builds never ask: a version with -dev, -beta or -alpha in it
// (1.2-beta) is not counted as a device and offered nothing.
#pragma once

#include <atomic>
#include <cstddef>
#include <string>

namespace update {

/* Where an update stands, for the UI (poll()). */
enum State {
  IDLE,        // nothing asked, or the answer was taken
  CHECKING,
  AVAILABLE,   // offer() says what
  UP_TO_DATE,  // a check the user asked for: nothing newer
  CHECK_FAILED,
  DOWNLOADING, // progress()
  INSTALLING,
  FAILED,      // error() says why; this copy is unchanged
  RESTARTING,  // the new version is in place: quit, and it starts at exit
};

struct Offer {
  std::string version, file, url, page;
  long long size = 0;
  /* Empty when this copy can replace itself; otherwise why not (English, to
     translate), and the dialog offers the download page instead. */
  std::string cannot;
};

/* Called once, early in main: makes the device id on a first start, and
   finishes what the last update left (args: the command line). */
void init(int argc, char **argv);
/* The question, on a thread of its own. manual: from Settings' "Check now",
   which says what it found either way and ignores "Not now" and "Skip". */
void check(bool manual);
State poll();
bool manual();         // the last check was asked for
Offer offer();
void progress(long long *done, long long *total);
std::string error();
/* What the user chose about the offer. */
void install();        // download it and put it in place
void not_now();        // again in 7 days
void skip_version();   // not this version; the next one, yes
void cancel();         // the download
void dismiss();        // the answer was seen: back to IDLE
/* Settings' switch: ask at startup or not. */
bool enabled();
void set_enabled(bool on);
/* A test build (-dev, -beta, -alpha): it never asks. */
bool test_build();
/* Where the platform has an updater at all. */
bool available();
/* From main, on the way out: stops the threads, then, after an update,
   starts the new version. */
void stop();
void finish();

/* What each platform provides (platform/<name>/update.cpp), where
   CHIRAL_HAS_UPDATER is 1. Messages are English, for tr(). */
namespace os {
/* The update API's name for this build: "windows-x64", "mac",
   "linux-x86_64"... */
const char *platform_key();
/* n bytes from the system's random source; false when there is none. */
bool random_bytes(void *out, size_t n);
/* HTTPS POST of a JSON body. False when no answer came back at all;
   otherwise the status and the body. */
bool post_json(const std::string &url, const std::string &body, int timeout_s, long *status, std::string *reply);
/* GET url (redirects followed) into path. received counts the bytes as they
   come; cancel stops it. False, with why, when it didn't complete. */
bool download(const std::string &url, const std::string &path, const std::atomic<bool> &cancel,
              std::atomic<long long> &received, std::string *why);
/* What an update to file (the release's file name) replaces: this copy's
   .exe, .app or AppImage. Empty, with why, when it can't be replaced where it
   is, or not by that kind of file. */
std::string target(const std::string &file, std::string *why);
/* Where to download file (the release's file name) to, beside target. */
std::string download_path(const std::string &target, const std::string &file);
/* Puts the download in place of target, under name (target's own name, or
   the release's when target's had the old version in it). Sets launch to
   what to start at exit. On failure target is left as it was. */
bool put_in_place(const std::string &target, const std::string &download, const std::string &name,
                  std::string *launch, std::string *why);
/* Starts launch, the new version, as this process exits. */
bool relaunch(const std::string &launch);
/* In the new version: removes what the old one left behind (path, from
   --updated-from), once it has gone. */
void clean_up(const std::string &path);
}  // namespace os

}  // namespace update
