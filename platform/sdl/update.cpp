// Updates on a computer (app/update.h): the update API and the download over
// HTTPS, and the new version put in place of this copy.
//
//   Windows  WinHTTP. The running .exe can't be deleted or overwritten, but it
//            can be renamed: it becomes <name>.old, the new one takes its
//            place, and the new version deletes the .old once this one has
//            gone (--updated-from).
//   Mac      NSURLSession (macos.mm). The .app, wherever it is (Applications
//            or anywhere else), is swapped for the one in the release's zip.
//   Linux    curl, or wget where there is no curl. The AppImage ($APPIMAGE)
//            is replaced in one rename; the running one stays mounted until
//            it exits.
//
// Wherever the copy can't be replaced -- a folder this account can't write
// to, a build that isn't the released file, a Mac app still in Downloads,
// which macOS runs from a hidden read-only copy -- target() says why, and
// the user gets the download page instead.

#include "update.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "i18n.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#endif

namespace update {
namespace os {
namespace {

std::string dir_of(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
}

bool ends_with(const std::string &s, const char *suffix) {
  const size_t n = strlen(suffix);
  return s.size() >= n && !strcasecmp(s.c_str() + s.size() - n, suffix);
}

[[maybe_unused]] bool exists(const std::string &path) {
  struct stat st;
  return !stat(path.c_str(), &st);
}

/* What the new version is to delete when it starts (Windows: the old .exe). */
std::string g_left;

}  // namespace

#ifdef _WIN32

/* ---------- Windows ---------- */

namespace {

std::wstring wide(const std::string &s) {
  std::wstring out(s.size() + 1, L'\0');
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], (int)out.size());
  out.resize(n > 0 ? n - 1 : 0);
  return out;
}

std::wstring win_path(const std::string &posix) {
  wchar_t buf[PATH_MAX];
  return chiral_win_path(posix.c_str(), buf, PATH_MAX) > 0 ? std::wstring(buf) : std::wstring();
}

/* MoveFileExW itself: <cstdio> takes away win32/posix.h's rename(), and
   the C library's own neither replaces a file nor reads "/c/..." paths. */
bool move(const std::string &from, const std::string &to) {
  return MoveFileExW(win_path(from).c_str(), win_path(to).c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::string self_path() {
  wchar_t path[PATH_MAX];
  const DWORD n = GetModuleFileNameW(nullptr, path, PATH_MAX);
  if(!n || n >= PATH_MAX) return "";
  char out[PATH_MAX];
  chiral_posix_path(path, out, sizeof(out));
  return out;
}

/* One request: its status, and the body -- into reply, or, with file, into
   that file as it comes. */
bool request(const std::string &url, const wchar_t *verb, const std::string *body, int timeout_s, long *status,
             std::string *reply, HANDLE file, const std::atomic<bool> *cancel, std::atomic<long long> *received) {
  std::wstring w = wide(url);
  URL_COMPONENTS u;
  memset(&u, 0, sizeof(u));
  u.dwStructSize = sizeof(u);
  wchar_t host[256], path[2048];
  u.lpszHostName = host;
  u.dwHostNameLength = sizeof(host) / sizeof(host[0]);
  u.lpszUrlPath = path;
  u.dwUrlPathLength = sizeof(path) / sizeof(path[0]);
  wchar_t extra[2048];
  u.lpszExtraInfo = extra;
  u.dwExtraInfoLength = sizeof(extra) / sizeof(extra[0]);
  if(!WinHttpCrackUrl(w.c_str(), 0, 0, &u) || u.nScheme != INTERNET_SCHEME_HTTPS) return false;
  const std::wstring object = std::wstring(path) + extra;

  const std::wstring agent = wide(std::string("chiral-ftp/") + VERSION_TAG);
  HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if(!session) {
    session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if(!session) return false;
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
  if(!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // Windows 10 before TLS 1.3
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
  }
  const int ms = timeout_s * 1000;
  WinHttpSetTimeouts(session, ms, ms, ms, ms);
  bool ok = false;
  HINTERNET conn = WinHttpConnect(session, host, u.nPort, 0);
  HINTERNET req = conn ? WinHttpOpenRequest(conn, verb, object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                       : nullptr;
  if(req) {
    const wchar_t *headers = body ? L"Content-Type: application/json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    void *data = body ? (void *)body->data() : WINHTTP_NO_REQUEST_DATA;
    const DWORD len = body ? (DWORD)body->size() : 0;
    if(WinHttpSendRequest(req, headers, body ? (DWORD)-1L : 0, data, len, len, 0) && WinHttpReceiveResponse(req, nullptr)) {
      DWORD code = 0, size = sizeof(code);
      WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                          &code, &size, WINHTTP_NO_HEADER_INDEX);
      *status = (long)code;
      ok = true;
      char buf[64 * 1024];
      for(;;) {
        DWORD got = 0;
        if(cancel && *cancel) {
          ok = false;
          break;
        }
        if(!WinHttpReadData(req, buf, sizeof(buf), &got)) {
          ok = false;
          break;
        }
        if(!got) break;
        if(file) {
          DWORD wrote = 0;
          if(!WriteFile(file, buf, got, &wrote, nullptr) || wrote != got) {
            ok = false;
            break;
          }
        } else if(reply->size() < 64 * 1024) {
          reply->append(buf, got);
        }
        if(received) *received += got;
      }
    }
    WinHttpCloseHandle(req);
  }
  if(conn) WinHttpCloseHandle(conn);
  WinHttpCloseHandle(session);
  return ok;
}

}  // namespace

bool random_bytes(void *out, size_t n) {
  return BCryptGenRandom(nullptr, (PUCHAR)out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

const char *build_key() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "windows-arm64";
#else
  return "windows-x64";
#endif
}

/* What the hardware is, which the program's own build can't say: Windows on
   ARM runs the x64 program in emulation, and inside it GetNativeSystemInfo()
   and PROCESSOR_ARCHITECTURE both answer x64. IsWow64Process2 (Windows 10
   1709 on; emulation came later) gives the real machine either way. */
const char *platform_key() {
  typedef BOOL(WINAPI * Wow64Process2)(HANDLE, USHORT *, USHORT *);
  const Wow64Process2 query =
      (Wow64Process2)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
  USHORT process = 0, machine = 0;
  if(query && query(GetCurrentProcess(), &process, &machine)) {
    return machine == 0xAA64 /* IMAGE_FILE_MACHINE_ARM64 */ ? "windows-arm64" : "windows-x64";
  }
  return build_key();
}

std::string file_key(const std::string &file) {
  if(file.find("-windows-arm64") != std::string::npos) return "windows-arm64";
  if(file.find("-windows-x64") != std::string::npos) return "windows-x64";
  return build_key();
}

bool post_json(const std::string &url, const std::string &body, int timeout_s, long *status, std::string *reply) {
  return request(url, L"POST", &body, timeout_s, status, reply, nullptr, nullptr, nullptr);
}

bool download(const std::string &url, const std::string &path, const std::atomic<bool> &cancel,
              std::atomic<long long> &received, std::string *why) {
  *why = N_("The download failed. Check the connection and try again.");
  const std::wstring w = win_path(path);
  HANDLE f = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if(f == INVALID_HANDLE_VALUE) {
    *why = N_("chiral-ftp could not write next to itself, so it can't update there.");
    return false;
  }
  long status = 0;
  const bool ok = request(url, L"GET", nullptr, 60, &status, nullptr, f, &cancel, &received) && status == 200;
  CloseHandle(f);
  return ok;
}

std::string target(const std::string &file, std::string *why) {
  const std::string self = self_path();
  if(self.empty() || !ends_with(self, ".exe") || !ends_with(file, ".exe")) {
    *why = N_("This copy can't update itself: only the released program can.");
    return "";
  }
  /* Can a file be made beside it? (Program Files, for one, says no.) */
  const std::wstring probe = win_path(dir_of(self) + "/.chiral-ftp-update-test");
  HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if(h == INVALID_HANDLE_VALUE) {
    *why = N_("chiral-ftp is in a folder this account can't change, so it can't update there.");
    return "";
  }
  CloseHandle(h);
  return self;
}

std::string download_path(const std::string &target, const std::string &file) {
  return dir_of(target) + "/" + file + ".download";
}

bool put_in_place(const std::string &target, const std::string &download, const std::string &name,
                  std::string *launch, std::string *why) {
  *why = N_("chiral-ftp could not put the new version in place. This copy is unchanged.");
  const std::string old = target + ".old", final_path = dir_of(target) + "/" + name;
  /* The running program can't be overwritten, but it can be moved aside. */
  if(!move(target, old)) return false;
  if(!move(download, final_path)) {
    move(old, target);
    return false;
  }
  g_left = old;
  *launch = final_path;
  return true;
}

bool relaunch(const std::string &launch) {
  std::wstring cmd = L"\"" + win_path(launch) + L"\"";
  if(!g_left.empty()) cmd += L" --updated-from \"" + wide(g_left) + L"\"";
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  const std::wstring dir = win_path(dir_of(launch));
  if(!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

void clean_up(const std::string &given) {
  /* Only the old copy beside this one: nothing else, whatever is passed. The
     path may come as "C:\..." as well as "/c/...". */
  char posix[PATH_MAX];
  chiral_posix_path_utf8(given.c_str(), posix, sizeof(posix));
  const std::string path = posix, self = self_path();
  if(!ends_with(path, ".exe.old") || strcasecmp(dir_of(path).c_str(), dir_of(self).c_str())) return;
  const std::wstring w = win_path(path);
  for(int i = 0; i < 40; i++) {  // the old copy may take a moment to exit
    if(DeleteFileW(w.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) return;
    Sleep(250);
  }
}

#else

/* ---------- Mac and Linux ---------- */

namespace {

/* Runs argv[0] (looked up in PATH), its stdin from in, and returns its exit
   status, or -1 when it couldn't be started. out, if given, gets its stdout.
   Every 100 ms, tick() may stop it by returning false. */
template <typename Tick>
int run(const std::vector<std::string> &argv, const std::string *in, std::string *out, Tick tick) {
  int to_child[2] = {-1, -1}, from_child[2] = {-1, -1};
  if(pipe(to_child)) return -1;
  if(pipe(from_child)) {
    close(to_child[0]);
    close(to_child[1]);
    return -1;
  }
  std::vector<char *> args;
  for(auto &a : argv) args.push_back((char *)a.c_str());
  args.push_back(nullptr);
  const pid_t pid = fork();
  if(pid < 0) {
    for(int fd : {to_child[0], to_child[1], from_child[0], from_child[1]}) close(fd);
    return -1;
  }
  if(pid == 0) {
    dup2(to_child[0], 0);
    dup2(from_child[1], 1);
    const int null = ::open("/dev/null", O_WRONLY);
    if(null >= 0) dup2(null, 2);
    for(int fd = 3; fd < 1024; fd++) close(fd);
    /* The system's tools, not linked against the AppImage's own libraries. */
    const char *ld = getenv("LD_LIBRARY_PATH"), *appdir = getenv("APPDIR");
    if(ld && appdir && *appdir && strstr(ld, appdir)) unsetenv("LD_LIBRARY_PATH");
    execvp(args[0], args.data());
    _exit(127);
  }
  close(to_child[0]);
  close(from_child[1]);
  if(in) {
    size_t sent = 0;
    while(sent < in->size()) {
      const ssize_t n = write(to_child[1], in->data() + sent, in->size() - sent);
      if(n <= 0) break;
      sent += (size_t)n;
    }
  }
  close(to_child[1]);
  fcntl(from_child[0], F_SETFL, fcntl(from_child[0], F_GETFL) | O_NONBLOCK);
  int status = 0;
  for(;;) {
    char buf[4096];
    ssize_t n;
    while((n = read(from_child[0], buf, sizeof(buf))) > 0) {
      if(out && out->size() < 64 * 1024) out->append(buf, (size_t)n);
    }
    const pid_t done = waitpid(pid, &status, WNOHANG);
    if(done == pid) break;
    if(done < 0 && errno != EINTR) {
      status = -1;
      break;
    }
    if(!tick()) {
      kill(pid, SIGTERM);
      waitpid(pid, &status, 0);
      close(from_child[0]);
      return -1;
    }
    usleep(100 * 1000);
  }
  char buf[4096];
  ssize_t n;
  while((n = read(from_child[0], buf, sizeof(buf))) > 0) {
    if(out && out->size() < 64 * 1024) out->append(buf, (size_t)n);
  }
  close(from_child[0]);
  return status >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

[[maybe_unused]] int run(const std::vector<std::string> &argv) {
  return run(argv, nullptr, nullptr, [] { return true; });
}

/* Starts argv detached from this process, which is about to exit. */
bool spawn_detached(const std::vector<std::string> &argv) {
  std::vector<char *> args;
  for(auto &a : argv) args.push_back((char *)a.c_str());
  args.push_back(nullptr);
  const pid_t pid = fork();
  if(pid < 0) return false;
  if(pid == 0) {
    setsid();
    /* The grandchild is the new version, so this exits at once and nothing
       is left for the old one to wait for. */
    if(fork() != 0) _exit(0);
    for(int fd = 3; fd < 1024; fd++) close(fd);
    execv(args[0], args.data());
    _exit(127);
  }
  int status;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool writable_here(const std::string &path) {
  return !access(dir_of(path).c_str(), W_OK) && !access(path.c_str(), W_OK);
}

#ifndef __APPLE__
/* curl, or wget where there is no curl (127: not found). */
bool fetch(const std::vector<std::string> &curl, const std::vector<std::string> &wget, const std::string *in,
           std::string *out, const std::atomic<bool> *cancel, const std::string *watch, std::atomic<long long> *received) {
  auto tick = [&] {
    if(watch && received) {
      struct stat st;
      if(!stat(watch->c_str(), &st)) *received = (long long)st.st_size;
    }
    return !(cancel && *cancel);
  };
  int code = run(curl, in, out, tick);
  if(code == 127) {
    if(out) out->clear();
    code = run(wget, nullptr, out, tick);
  }
  if(watch && received) tick();
  return code == 0;
}
#endif

}  // namespace

bool random_bytes(void *out, size_t n) {
  const int fd = ::open("/dev/urandom", O_RDONLY);
  if(fd < 0) return false;
  const bool ok = read(fd, out, n) == (ssize_t)n;
  close(fd);
  return ok;
}

#ifdef __APPLE__
}  // namespace os
}  // namespace update
/* macos.mm: NSURLSession, and where the app is. */
bool macos_post_json(const std::string &url, const std::string &body, int timeout_s, long *status, std::string *reply);
bool macos_download(const std::string &url, const std::string &path, const std::atomic<bool> &cancel,
                    std::atomic<long long> &received);
std::string macos_bundle_path();
namespace update {
namespace os {

const char *platform_key() {
  return "mac";  // Apple silicon only: there is no Intel build to be on the wrong one
}

const char *build_key() {
  return platform_key();
}

std::string file_key(const std::string &file) {
  (void)file;
  return platform_key();
}

bool post_json(const std::string &url, const std::string &body, int timeout_s, long *status, std::string *reply) {
  return macos_post_json(url, body, timeout_s, status, reply);
}

bool download(const std::string &url, const std::string &path, const std::atomic<bool> &cancel,
              std::atomic<long long> &received, std::string *why) {
  *why = N_("The download failed. Check the connection and try again.");
  return macos_download(url, path, cancel, received);
}

std::string target(const std::string &file, std::string *why) {
  const std::string app = macos_bundle_path();
  if(app.empty() || !ends_with(app, ".app") || !ends_with(file, ".zip")) {
    *why = N_("This copy can't update itself: only the released program can.");
    return "";
  }
  /* Opened where it was downloaded, without being moved: macOS runs it from
     a hidden read-only copy (App Translocation), which can't be replaced. */
  if(app.find("/AppTranslocation/") != std::string::npos) {
    *why = N_("macOS is running chiral-ftp from a temporary copy. Move it to Applications, open it from there, and it can update itself.");
    return "";
  }
  if(!writable_here(app)) {
    *why = N_("chiral-ftp is in a folder this account can't change, so it can't update there.");
    return "";
  }
  return app;
}

std::string download_path(const std::string &target, const std::string &file) {
  (void)file;
  return dir_of(target) + "/.chiral-ftp-update.zip";
}

namespace {
std::string g_staging;
}

bool put_in_place(const std::string &target, const std::string &download, const std::string &name,
                  std::string *launch, std::string *why) {
  (void)name;  // the release's is the zip's; the app keeps its own
  *why = N_("chiral-ftp could not put the new version in place. This copy is unchanged.");
  const std::string staging = dir_of(target) + "/.chiral-ftp-update";
  run({"/bin/rm", "-rf", staging});
  if(run({"/usr/bin/ditto", "-x", "-k", download, staging})) {
    run({"/bin/rm", "-rf", staging});
    *why = N_("The download was not the expected file. Nothing was changed.");
    return false;
  }
  unlink(download.c_str());
  /* The zip holds one chiral-ftp.app (or whatever the release called it). */
  std::string fresh;
  if(DIR *d = opendir(staging.c_str())) {
    while(struct dirent *e = readdir(d)) {
      if(e->d_name[0] != '.' && ends_with(e->d_name, ".app")) fresh = staging + "/" + e->d_name;
    }
    closedir(d);
  }
  if(fresh.empty() || !exists(fresh + "/Contents/MacOS/chiral-ftp")) {
    run({"/bin/rm", "-rf", staging});
    *why = N_("The download was not the expected file. Nothing was changed.");
    return false;
  }
  /* A zip made anywhere but on a Mac (v1.1's was made on Windows) stores no
     Unix permissions, and the program comes out of it without its x: macOS
     then says the app "can't be opened". The code signature doesn't cover
     modes, so setting it back is all it takes. */
  run({"/bin/chmod", "-R", "a+x", fresh + "/Contents/MacOS"});
  run({"/usr/bin/xattr", "-dr", "com.apple.quarantine", fresh});
  const std::string old = dir_of(target) + "/.chiral-ftp-old.app";
  run({"/bin/rm", "-rf", old});
  if(rename(target.c_str(), old.c_str())) {
    run({"/bin/rm", "-rf", staging});
    return false;
  }
  if(rename(fresh.c_str(), target.c_str())) {
    rename(old.c_str(), target.c_str());
    run({"/bin/rm", "-rf", staging});
    return false;
  }
  g_left = old;
  g_staging = staging;
  *launch = target;
  return true;
}

bool relaunch(const std::string &launch) {
  /* What this copy was is no longer needed even by itself: everything it
     uses is loaded, and the files stay readable while it runs. */
  if(!g_left.empty()) run({"/bin/rm", "-rf", g_left});
  if(!g_staging.empty()) run({"/bin/rm", "-rf", g_staging});
  return spawn_detached({"/usr/bin/open", "-n", launch});
}

void clean_up(const std::string &path) {
  (void)path;  // the old copy cleaned up after itself
}

#else

/* ---------- Linux ---------- */

const char *platform_key() {
#if defined(__aarch64__)
  return "linux-aarch64";
#else
  return "linux-x86_64";
#endif
}

/* An AppImage runs only on its own architecture (no Windows-style
   emulation to speak of), so the build is the machine. */
const char *build_key() {
  return platform_key();
}

std::string file_key(const std::string &file) {
  if(file.find("-aarch64.") != std::string::npos) return "linux-aarch64";
  if(file.find("-x86_64.") != std::string::npos) return "linux-x86_64";
  return platform_key();
}

bool post_json(const std::string &url, const std::string &body, int timeout_s, long *status, std::string *reply) {
  const std::string t = std::to_string(timeout_s);
  std::string out;
  /* curl writes the status after the body; wget says nothing of it, only
     whether it was a success. */
  const bool ok = fetch({"curl", "-sS", "--max-time", t, "-X", "POST", "-H", "Content-Type: application/json",
                         "--data-binary", "@-", "-w", "\n%{http_code}", url},
                        {"wget", "-q", "-O", "-", "-T", t, "--header=Content-Type: application/json",
                         "--post-data=" + body, url},
                        &body, &out, nullptr, nullptr, nullptr);
  if(!ok) return false;
  const size_t nl = out.find_last_of('\n');
  const std::string last = nl == std::string::npos ? "" : out.substr(nl + 1);
  if(!last.empty() && last.find_first_not_of("0123456789") == std::string::npos) {
    *status = atol(last.c_str());
    *reply = out.substr(0, nl);
  } else {
    *status = 200;  // wget: it succeeded
    *reply = out;
  }
  return true;
}

bool download(const std::string &url, const std::string &path, const std::atomic<bool> &cancel,
              std::atomic<long long> &received, std::string *why) {
  *why = N_("The download failed. Check the connection and try again.");
  return fetch({"curl", "-fsSL", "--connect-timeout", "20", "--speed-limit", "1", "--speed-time", "60", "-o", path, url},
               {"wget", "-q", "-T", "60", "-O", path, url},
               nullptr, nullptr, &cancel, &path, &received);
}

std::string target(const std::string &file, std::string *why) {
  const char *appimage = getenv("APPIMAGE");
  if(!appimage || !*appimage || !ends_with(file, ".AppImage")) {
    *why = N_("This copy can't update itself: only the released program can.");
    return "";
  }
  if(!writable_here(appimage)) {
    *why = N_("chiral-ftp is in a folder this account can't change, so it can't update there.");
    return "";
  }
  return appimage;
}

std::string download_path(const std::string &target, const std::string &file) {
  return dir_of(target) + "/." + file + ".download";
}

bool put_in_place(const std::string &target, const std::string &download, const std::string &name,
                  std::string *launch, std::string *why) {
  *why = N_("chiral-ftp could not put the new version in place. This copy is unchanged.");
  const std::string final_path = dir_of(target) + "/" + name;
  if(chmod(download.c_str(), 0755) || rename(download.c_str(), final_path.c_str())) return false;
  if(final_path != target) unlink(target.c_str());  // renamed for its version
  *launch = final_path;
  return true;
}

bool relaunch(const std::string &launch) {
  return spawn_detached({launch});
}

void clean_up(const std::string &path) {
  (void)path;  // a rename over the old one left nothing
}

#endif
#endif

const char *restart_note() {
  return N_("chiral-ftp starts again in a moment.");
}

}  // namespace os
}  // namespace update
