// Updates on Android (app/update.h): the update API and the download go
// through Java's HttpsURLConnection (ChiralActivity.updatePost and
// updateDownload), and the new APK goes to Android's own installer, the way
// Open hands an .apk to it from the file manager (backend::open_file).
//
// Android then asks the user, replaces the app (which closes this one) and
// offers to open the new version. Nothing here replaces anything itself, so
// put_in_place() leaves launch empty and the dialog says to confirm in the
// installer (update::HANDED_OFF).
//
// The installer only takes an APK signed with the same key as the app
// already installed: the releases are signed with the ANDROID_DEBUG_KEYSTORE
// secret. And it asks once to allow installs from chiral-ftp
// (REQUEST_INSTALL_PACKAGES, in the manifest).

#include "update.h"

#include <cstring>
#include <fcntl.h>
#include <jni.h>
#include <string>
#include <strings.h>
#include <unistd.h>

#include <SDL.h>

#include "backend.h"
#include "i18n.h"

extern "C" {
#include "config.h"
}

namespace {

/* The download in progress, for updateProgress() (one at a time). */
std::atomic<long long> *g_received = nullptr;
const std::atomic<bool> *g_cancel = nullptr;

const char *const APK_NAME = "chiral-ftp-update.apk";

std::string apk_path() {
  return std::string(data_dir()) + "/" + APK_NAME;
}

/* The activity and its class, for one call from this (native) thread;
   SDL attaches the thread to Java on the first call. */
struct Java {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jobject activity = env ? (jobject)SDL_AndroidGetActivity() : nullptr;
  jclass cls = activity ? env->GetObjectClass(activity) : nullptr;
  bool ok() const { return env && activity && cls; }
  ~Java() {
    if(env && env->ExceptionCheck()) env->ExceptionClear();
    if(cls) env->DeleteLocalRef(cls);
    if(activity) env->DeleteLocalRef(activity);
  }
};

jbyteArray bytes_of(JNIEnv *env, const std::string &s) {
  jbyteArray b = env->NewByteArray((jsize)s.size());
  if(b) env->SetByteArrayRegion(b, 0, (jsize)s.size(), (const jbyte *)s.data());
  return b;
}

}  // namespace

/* ChiralActivity.updateProgress(long): from updateDownload, on the updater's
   thread. */
extern "C" JNIEXPORT jboolean JNICALL
Java_io_github_herbrax_chiralftp_ChiralActivity_updateProgress(JNIEnv *, jclass, jlong bytes) {
  if(g_received) *g_received = (long long)bytes;
  return g_cancel && *g_cancel ? JNI_FALSE : JNI_TRUE;
}

namespace update {
namespace os {

const char *platform_key() {
  return "android";  // one APK for every device and architecture
}

const char *build_key() {
  return platform_key();
}

std::string file_key(const std::string &file) {
  (void)file;
  return platform_key();
}

bool random_bytes(void *out, size_t n) {
  const int fd = ::open("/dev/urandom", O_RDONLY);
  if(fd < 0) return false;
  const bool ok = read(fd, out, n) == (ssize_t)n;
  close(fd);
  return ok;
}

bool post_json(const std::string &url, const std::string &body, int timeout_s, long *status, std::string *reply) {
  Java j;
  if(!j.ok()) return false;
  jmethodID m = j.env->GetMethodID(j.cls, "updatePost", "(Ljava/lang/String;[BI)[B");
  if(!m) return false;
  jstring address = j.env->NewStringUTF(url.c_str());
  jbyteArray data = bytes_of(j.env, body);
  jbyteArray answer = (jbyteArray)j.env->CallObjectMethod(j.activity, m, address, data, (jint)timeout_s);
  bool ok = false;
  if(answer && !j.env->ExceptionCheck()) {
    const jsize n = j.env->GetArrayLength(answer);
    std::string text((size_t)n, '\0');
    j.env->GetByteArrayRegion(answer, 0, n, (jbyte *)&text[0]);
    const size_t nl = text.find('\n');
    if(nl != std::string::npos) {
      *status = atol(text.substr(0, nl).c_str());
      *reply = text.substr(nl + 1);
      ok = true;
    }
  }
  if(answer) j.env->DeleteLocalRef(answer);
  if(data) j.env->DeleteLocalRef(data);
  if(address) j.env->DeleteLocalRef(address);
  return ok;
}

bool download(const std::string &url, const std::string &path, const std::atomic<bool> &cancel,
              std::atomic<long long> &received, std::string *why) {
  *why = N_("The download failed. Check the connection and try again.");
  Java j;
  if(!j.ok()) return false;
  jmethodID m = j.env->GetMethodID(j.cls, "updateDownload", "(Ljava/lang/String;[B)I");
  if(!m) return false;
  jstring address = j.env->NewStringUTF(url.c_str());
  jbyteArray file = bytes_of(j.env, path);
  g_received = &received;
  g_cancel = &cancel;
  const jint status = j.env->CallIntMethod(j.activity, m, address, file);
  g_received = nullptr;
  g_cancel = nullptr;
  const bool ok = !j.env->ExceptionCheck() && status == 200;
  if(file) j.env->DeleteLocalRef(file);
  if(address) j.env->DeleteLocalRef(address);
  return ok;
}

std::string target(const std::string &file, std::string *why) {
  const size_t n = file.size();
  if(n < 4 || strcasecmp(file.c_str() + n - 4, ".apk")) {
    *why = N_("This copy can't update itself: only the released program can.");
    return "";
  }
  return data_dir();  // nothing to replace by hand: the installer does it
}

std::string download_path(const std::string &target, const std::string &file) {
  (void)target;
  (void)file;
  return apk_path();
}

bool put_in_place(const std::string &target, const std::string &download, const std::string &name,
                  std::string *launch, std::string *why) {
  (void)target;
  (void)name;
  launch->clear();
  if(!backend::open_file(download.c_str(), 0)) {
    *why = N_("Android's installer could not be opened. Download the APK from the website instead.");
    return false;
  }
  return true;
}

bool relaunch(const std::string &launch) {
  (void)launch;
  return true;  // the installer offers Open
}

void clean_up(const std::string &path) {
  /* At every start: the APK of an update that was installed, or turned
     down, since. */
  if(path.empty()) unlink(apk_path().c_str());
}

const char *restart_note() {
  return N_("chiral-ftp starts again in a moment.");  // not shown: the installer takes over
}

}  // namespace os
}  // namespace update
