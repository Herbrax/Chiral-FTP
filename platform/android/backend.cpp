// Android backend: an SDL window on a phone, tablet, foldable or TV.
//
// A phone or tablet hands its window to the layout (window_size), measured
// in dp, and the mobile shell (app/ui_mobile.inc) arranges itself to fit:
// a cover screen, one pane, two side by side or stacked. A finger arrives
// as the computer interface's mouse: a tap clicks, a drag scrolls (and
// keeps going when flung), a long press is the right click that opens a
// menu or starts marking. A TV keeps the console's 1920x1080 layout, driven
// by the remote: the D-pad, OK (SELECT), Back and Menu.

#include "backend.h"
#include "i18n.h"
#include "platform.h"
#include "platform_config.h"

#include <SDL.h>
#include <jni.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace backend {
namespace {

SDL_Window *g_window = nullptr;
SDL_Renderer *g_renderer = nullptr;
SDL_Texture *g_texture = nullptr;
bool g_text_input = false;
bool g_tv = false;
float g_density = 1.0f;  // pixels per dp

/* The frame's size in pixels, as window_size() last handed it out. */
int g_frame_w = CHIRAL_RENDER_W, g_frame_h = CHIRAL_RENDER_H;
int g_tex_w = 0, g_tex_h = 0;

/* One finger, turned into the mouse the computer interface reads. */
struct Finger {
  bool down = false;
  SDL_FingerID id = 0;
  float start_x = 0, start_y = 0;  // dp
  float x = 0, y = 0;              // dp
  Uint64 start_ms = 0, last_ms = 0;
  bool moved = false;              // past the slop: a drag, not a tap
  bool long_done = false;          // the long press has fired
  /* Where the finger was lately, for its speed when it lets go: from the
     movement over the last tenth of a second, as Android's own lists do,
     not from the last two samples (which can arrive almost together). */
  struct Sample {
    Uint64 ms;
    float y;
  };
  Sample recent[16];
  int recent_n = 0;
  bool edge = false;               // went down at a side edge, sideways: may be a swipe for the tabs
  bool edge_right = false;
  bool swiped = false;             // it was: nothing else of this touch counts
};
Finger g_finger;
/* The remote's OK, held: the Menu (most remotes have no Menu button any
   more). So OK counts when it comes up, as Back does in platform_common. */
Uint64 g_ok_since = 0;
bool g_ok_long_sent = false;
float g_fling = 0;          // dp per second still to scroll after letting go
Uint64 g_fling_ms = 0;
Uint64 g_leave_at = 0;      // when a tap's hover should go away
Uint64 g_last_present = 0;  // ms
int g_refresh_ms = 16;      // one refresh of the display (8 at 120 Hz)

void remember(Uint64 ms, float y) {
  Finger &f = g_finger;
  if(f.recent_n == 16) {
    for(int i = 1; i < 16; i++) f.recent[i - 1] = f.recent[i];
    f.recent_n = 15;
  }
  f.recent[f.recent_n++] = {ms, y};
}

/* dp per second over the last 100 ms of movement; 0 if the finger had
   stopped before it let go. */
float release_velocity(Uint64 now_ms) {
  const Finger &f = g_finger;
  if(f.recent_n < 2 || now_ms - f.recent[f.recent_n - 1].ms > 60) return 0;
  int first = f.recent_n - 1;
  while(first > 0 && f.recent[f.recent_n - 1].ms - f.recent[first - 1].ms <= 100) first--;
  const float dt = (float)(f.recent[f.recent_n - 1].ms - f.recent[first].ms) / 1000.0f;
  if(dt < 0.015f) return 0;
  return (f.recent[f.recent_n - 1].y - f.recent[first].y) / dt;
}

const float SLOP_DP = 8;            // movement still counted as a tap
const float EDGE_DP = 28;           // how near a side a swipe for the tabs starts
const float EDGE_SWIPE_DP = 24;     // and how far in it has to come
const Uint64 LONG_PRESS_MS = 450;
const float WHEEL_DP = 40;          // one wheel notch scrolls this far (ui_desktop.inc: P(40))
const float FLING_MIN = 60;         // dp/s below which a fling stops
const float FLING_TAU = 0.325f;     // seconds for a fling to slow to a third

float dp(float px) {
  return px / g_density;
}

Action key_action(SDL_Keycode k) {
  switch(k) {
  case SDLK_UP: return ACT_UP;
  case SDLK_DOWN: return ACT_DOWN;
  case SDLK_LEFT: return ACT_LEFT;
  case SDLK_RIGHT: return ACT_RIGHT;
  case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SELECT: return ACT_CROSS;  // SELECT: the remote's OK
  case SDLK_ESCAPE: case SDLK_AC_BACK: return ACT_CIRCLE;
  case SDLK_SPACE: case SDLK_AUDIOPLAY: return ACT_SQUARE;  // AUDIOPLAY: a remote's Play/Pause
  case SDLK_MENU: return ACT_OPTIONS;
  case SDLK_PAGEUP: case SDLK_AUDIOREWIND: return ACT_L2;
  case SDLK_PAGEDOWN: case SDLK_AUDIOFASTFORWARD: return ACT_R2;
  default: return ACT_NONE;
  }
}

Action pad_button(int b) {
  switch(b) {
  case SDL_CONTROLLER_BUTTON_A: return ACT_CROSS;
  case SDL_CONTROLLER_BUTTON_B: return ACT_CIRCLE;
  case SDL_CONTROLLER_BUTTON_X: return ACT_SQUARE;
  case SDL_CONTROLLER_BUTTON_Y: return ACT_TRIANGLE;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return ACT_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return ACT_R1;
  case SDL_CONTROLLER_BUTTON_DPAD_UP: return ACT_UP;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return ACT_DOWN;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return ACT_LEFT;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return ACT_RIGHT;
  case SDL_CONTROLLER_BUTTON_START: return ACT_OPTIONS;
  case SDL_CONTROLLER_BUTTON_BACK: case SDL_CONTROLLER_BUTTON_TOUCHPAD: return ACT_TOUCHPAD;
  default: return ACT_NONE;
  }
}

void click(bool right) {
  using namespace platform::detail;
  pointer_button(right, true, false);
  pointer_button(right, false, false);
}

void finger_down(const SDL_TouchFingerEvent &f) {
  if(g_finger.down) return;  // one finger at a time: nothing here wants two
  int w = 0, h = 0;
  SDL_GetRendererOutputSize(g_renderer, &w, &h);
  g_fling = 0;
  g_finger = Finger();
  g_finger.down = true;
  g_finger.id = f.fingerId;
  g_finger.x = g_finger.start_x = dp(f.x * (float)w);
  g_finger.y = g_finger.start_y = dp(f.y * (float)h);
  g_finger.start_ms = g_finger.last_ms = SDL_GetTicks64();
  remember(g_finger.start_ms, g_finger.y);
  g_leave_at = 0;
  /* Sideways, a touch at the left or right edge may be a swipe for the
     tabs (ui_mobile.inc); upright, the tabs are always there. */
  const float wdp = dp((float)w), hdp = dp((float)h);
  if(wdp > hdp && (g_finger.x < EDGE_DP || g_finger.x > wdp - EDGE_DP)) {
    g_finger.edge = true;
    g_finger.edge_right = g_finger.x > wdp / 2;
  }
  platform::detail::pointer_move(g_finger.x, g_finger.y);
}

void finger_motion(const SDL_TouchFingerEvent &f) {
  if(!g_finger.down || f.fingerId != g_finger.id) return;
  int w = 0, h = 0;
  SDL_GetRendererOutputSize(g_renderer, &w, &h);
  const float x = dp(f.x * (float)w), y = dp(f.y * (float)h);
  const float dy = y - g_finger.y;
  const Uint64 t = SDL_GetTicks64();
  if(g_finger.swiped) return;
  if(g_finger.edge) {
    const float tx = x - g_finger.start_x, ty = y - g_finger.start_y;
    const float inward = g_finger.edge_right ? -tx : tx;
    if(inward > EDGE_SWIPE_DP && std::fabs(tx) > 1.5f * std::fabs(ty)) {
      g_finger.swiped = true;
      g_finger.long_done = true;
      const Action a = g_finger.edge_right ? ACT_NAV_RIGHT : ACT_NAV_LEFT;
      platform::detail::press(a);
      platform::detail::release(a);
      return;
    }
    if(std::fabs(ty) > EDGE_SWIPE_DP) g_finger.edge = false;  // up or down: an ordinary scroll after all
    else return;  // not decided yet: neither a scroll nor a tap
  }
  if(!g_finger.moved && std::hypot(x - g_finger.start_x, y - g_finger.start_y) > SLOP_DP) g_finger.moved = true;
  if(g_finger.moved && !g_finger.long_done) platform::detail::pointer_wheel(dy / WHEEL_DP);
  remember(t, y);
  g_finger.x = x;
  g_finger.y = y;
  g_finger.last_ms = t;
}

void finger_up(const SDL_TouchFingerEvent &f) {
  if(!g_finger.down || f.fingerId != g_finger.id) return;
  g_finger.down = false;
  const Uint64 t = SDL_GetTicks64();
  if(g_finger.swiped) {
    g_leave_at = t + 120;
  } else if(!g_finger.moved && !g_finger.long_done) {
    click(false);
    g_leave_at = t + 120;  // the click lands first, then the highlight goes
  } else if(g_finger.moved && !g_finger.long_done && std::fabs(release_velocity(t)) > FLING_MIN * 4) {
    g_fling = release_velocity(t);
    g_fling_ms = t;
  } else {
    g_leave_at = t + 120;
  }
}

/* The long press, and the fling after a drag, happen between events. */
void tick() {
  const Uint64 t = SDL_GetTicks64();
  if(g_finger.down && !g_finger.moved && !g_finger.long_done && t - g_finger.start_ms >= LONG_PRESS_MS) {
    g_finger.long_done = true;
    click(true);
  }
  if(g_ok_since && !g_ok_long_sent && t - g_ok_since >= LONG_PRESS_MS) {
    g_ok_long_sent = true;
    platform::detail::press(ACT_OPTIONS);
    platform::detail::release(ACT_OPTIONS);
  }
  if(g_fling != 0) {
    const float dt = (float)(t - g_fling_ms) / 1000.0f;
    g_fling_ms = t;
    platform::detail::pointer_wheel(g_fling * dt / WHEEL_DP);
    g_fling *= std::exp(-dt / FLING_TAU);
    if(std::fabs(g_fling) < FLING_MIN) {
      g_fling = 0;
      g_leave_at = t;
    }
  }
  if(g_leave_at && t >= g_leave_at) {
    g_leave_at = 0;
    platform::detail::pointer_leave();
  }
}

bool animating() {
  return g_fling != 0 || g_leave_at || (g_finger.down && !g_finger.moved && !g_finger.long_done) ||
         (g_ok_since && !g_ok_long_sent);
}

void handle(const SDL_Event &e) {
  using namespace platform::detail;
  switch(e.type) {
  case SDL_QUIT: quit(); break;
  case SDL_TEXTINPUT: typed(e.text.text); break;
  case SDL_KEYDOWN:
    if(g_text_input) {
      if(e.key.keysym.sym == SDLK_BACKSPACE) key_backspace();
      if(e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) key_enter();
      if(e.key.keysym.sym == SDLK_AC_BACK || e.key.keysym.sym == SDLK_ESCAPE) key_escape();
    } else if(!e.key.repeat) {
      const SDL_Keycode k = e.key.keysym.sym;
      // Back and Escape leave the glyphs alone: a gamepad's Back can arrive as a key.
      if(k != SDLK_AC_BACK && k != SDLK_ESCAPE && key_action(k) != ACT_NONE) input_from_pad(false);
      if(k == SDLK_SELECT) {
        g_ok_since = SDL_GetTicks64();
        g_ok_long_sent = false;
      } else {
        press(key_action(k));
      }
    }
    break;
  case SDL_KEYUP:
    if(e.key.keysym.sym == SDLK_SELECT) {
      if(g_ok_since && !g_ok_long_sent && !g_text_input) {
        press(ACT_CROSS);
        release(ACT_CROSS);
      }
      g_ok_since = 0;
    } else if(!g_text_input) {
      release(key_action(e.key.keysym.sym));
    }
    break;
  case SDL_FINGERDOWN: finger_down(e.tfinger); break;
  case SDL_FINGERMOTION: finger_motion(e.tfinger); break;
  case SDL_FINGERUP: finger_up(e.tfinger); break;
  /* A real mouse: Samsung DeX, a Chromebook, a mouse plugged into a phone.
     (Touches don't make mouse events: SDL_HINT_TOUCH_MOUSE_EVENTS is off.) */
  case SDL_MOUSEMOTION: pointer_move(dp((float)e.motion.x), dp((float)e.motion.y)); break;
  case SDL_MOUSEBUTTONDOWN:
  case SDL_MOUSEBUTTONUP:
    if(e.button.button != SDL_BUTTON_LEFT && e.button.button != SDL_BUTTON_RIGHT) break;
    pointer_move(dp((float)e.button.x), dp((float)e.button.y));
    pointer_button(e.button.button == SDL_BUTTON_RIGHT, e.type == SDL_MOUSEBUTTONDOWN, false);
    break;
  case SDL_MOUSEWHEEL: pointer_wheel(e.wheel.preciseY); break;
  case SDL_CONTROLLERDEVICEADDED: SDL_GameControllerOpen(e.cdevice.which); break;
  case SDL_CONTROLLERBUTTONDOWN:
    input_from_pad(true);
    press(pad_button(e.cbutton.button));
    break;
  case SDL_CONTROLLERBUTTONUP: release(pad_button(e.cbutton.button)); break;
  default: break;
  }
}

}  // namespace

bool init(bool headless) {
  (void)headless;
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // Back is the app's, not "close the app"
  SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");    // the UI keeps up while another app is in front
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");        // fingers are handled here, not as a fake mouse
  SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait PortraitUpsideDown");
  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)) {
    SDL_Log("SDL_Init: %s", SDL_GetError());
    return false;
  }
  g_tv = SDL_IsAndroidTV();
  float ddpi = 0;
  if(!SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) && ddpi > 0) g_density = ddpi / 160.0f;
  SDL_DisplayMode mode;
  if(!SDL_GetCurrentDisplayMode(0, &mode) && mode.refresh_rate > 0) g_refresh_ms = std::max(4, 1000 / mode.refresh_rate);
  g_window = SDL_CreateWindow("chiral-ftp", 0, 0, 0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_RESIZABLE);
  if(!g_window) return false;
  g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  return g_renderer != nullptr;
}

void shutdown() {
  if(g_texture) SDL_DestroyTexture(g_texture);
  if(g_renderer) SDL_DestroyRenderer(g_renderer);
  if(g_window) SDL_DestroyWindow(g_window);
  g_texture = nullptr;
  g_renderer = nullptr;
  g_window = nullptr;
  SDL_Quit();
}

void pump(int timeout_ms) {
  SDL_Event e;
  /* The wait is counted from the last frame shown, not from now: drawing
     takes time too, and "16 ms after drawing" was a frame every 25-35 ms.
     A fling moves at the display's own rate (8 ms at 120 Hz); anything
     else that animates (the UI asks for 16 or less) at 60 a second, so a
     long transfer's progress doesn't redraw the phone 120 times a second. */
  const int since = (int)(SDL_GetTicks64() - g_last_present);
  if(animating()) timeout_ms = std::max(0, g_refresh_ms - since);
  else if(timeout_ms <= 16) timeout_ms = std::max(0, 16 - since);
  if(timeout_ms > 0 ? SDL_WaitEventTimeout(&e, timeout_ms) : SDL_PollEvent(&e)) {
    handle(e);
    while(SDL_PollEvent(&e)) handle(e);
  }
  tick();
}

std::uint32_t *frame_begin(int *stride_pixels) {
  (void)stride_pixels;
  return nullptr;
}

void frame_end() {}

bool window_size(int *pt_w, int *pt_h, int *px_w, int *px_h) {
  /* A TV keeps the console's fixed layout; a phone or tablet lays out in
     its window, in dp. */
  const char *test_tv = SDL_getenv("CHIRAL_TV_LAYOUT");  // Settings' "TV layout (testing)" switch
  if(g_tv || (test_tv && *test_tv == '1') || !g_window || !g_renderer) return false;
  if(SDL_GetRendererOutputSize(g_renderer, px_w, px_h)) return false;
  *pt_w = (int)std::lround(dp((float)*px_w));
  *pt_h = (int)std::lround(dp((float)*px_h));
  g_frame_w = *px_w;
  g_frame_h = *px_h;
  return true;
}

void present(const std::uint32_t *pixels) {
  g_last_present = SDL_GetTicks64();
  if(!g_renderer) return;
  if(!g_texture || g_tex_w != g_frame_w || g_tex_h != g_frame_h) {
    if(g_texture) SDL_DestroyTexture(g_texture);
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, g_frame_w, g_frame_h);
    if(!g_texture) return;
    g_tex_w = g_frame_w;
    g_tex_h = g_frame_h;
  }
  SDL_UpdateTexture(g_texture, nullptr, pixels, g_frame_w * 4);
  SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
  SDL_RenderPresent(g_renderer);
}

bool in_foreground() {
  if(!g_window) return true;
  const Uint32 flags = SDL_GetWindowFlags(g_window);
  return (flags & SDL_WINDOW_INPUT_FOCUS) && !(flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN));
}

bool copy_text(const char *text) {
  return SDL_SetClipboardText(text) == 0;
}

bool choose_folder(const char *start, std::string &out) {
  (void)start;
  (void)out;
  return false;  // the system's picker hands back content URIs, not paths: the UI asks for a path
}

/* ChiralActivity.openFile(byte[] path, int how): the path as UTF-8 bytes,
   not a JNI string, whose "modified UTF-8" mangles emoji in file names. */
bool open_file(const char *path, int how) {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jobject activity = (jobject)SDL_AndroidGetActivity();
  if(!env || !activity) return false;
  bool ok = false;
  jclass cls = env->GetObjectClass(activity);
  jmethodID m = env->GetMethodID(cls, "openFile", "([BI)Z");
  if(m) {
    const jsize n = (jsize)strlen(path);
    jbyteArray bytes = env->NewByteArray(n);
    env->SetByteArrayRegion(bytes, 0, n, (const jbyte *)path);
    ok = env->CallBooleanMethod(activity, m, bytes, (jint)how);
    env->DeleteLocalRef(bytes);
  }
  if(env->ExceptionCheck()) {
    env->ExceptionClear();
    ok = false;
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
  return ok;
}

/* ChiralActivity.toBackground(): Back on the first screen, as on any
   Android app -- the app goes behind the home screen and keeps running
   (SDL_HINT_ANDROID_BLOCK_ON_PAUSE is off), servers and all. */
bool to_background() {
  JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jobject activity = (jobject)SDL_AndroidGetActivity();
  if(!env || !activity) return false;
  jclass cls = env->GetObjectClass(activity);
  jmethodID m = env->GetMethodID(cls, "toBackground", "()V");
  if(m) env->CallVoidMethod(activity, m);
  const bool ok = m && !env->ExceptionCheck();
  if(env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
  return ok;
}

bool open_url(const char *url) {
  return SDL_OpenURL(url) == 0;
}

void pointer_hand(bool on) {
  (void)on;  // no pointer shapes on a touchscreen
}

std::uint64_t ticks_ms() {
  return SDL_GetTicks64();
}

bool touch(int *x, int *y, bool *down) {
  /* Fingers come in as the pointer (above), not the console's touch path. */
  (void)x;
  (void)y;
  (void)down;
  return false;
}

bool startup_notice(bool *ok, const char **line, const char **detail, int *status) {
  (void)ok;
  (void)line;
  (void)detail;
  (void)status;
  return false;
}

void report_startup_services(void (*trace)(const char *fmt, ...)) {
  int w = 0, h = 0;
  if(g_renderer) SDL_GetRendererOutputSize(g_renderer, &w, &h);
  trace("Android: SDL %d.%d.%d, API %d, %s, %dx%d pixels at %.2f per dp", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
        SDL_PATCHLEVEL, SDL_GetAndroidSDKVersion(), g_tv ? "TV" : "phone or tablet", w, h, (double)g_density);
}

void early_init() {}

bool keyboard_failed() {
  return false;
}

bool has_native_keyboard() {
  return false;  // the on-screen keyboard types into the app's own fields
}

bool native_text_prompt(const char *, const std::string &, TextKind, std::string &) {
  return false;
}

void set_text_input(bool on) {
  g_text_input = on;
  if(on) SDL_StartTextInput();
  else SDL_StopTextInput();
}

const char *about_file_manager() {
  // Only the TV's layout shows the About this goes in; the phone's has its own.
  return platform::windowed() ? N_("  File manager: copy, move and delete files on this phone.")
                              : N_("  File manager: copy, move and delete files on the TV.");
}

int about_thanks(const char *const **lines) {
  static const char *const kThanks[] = {
      "websrv and ftpsrv (John T\xc3\xb6rnblom), SDL2, libssh2, libssh, Mbed TLS, GNU libmicrohttpd,",
      "Dear ImGui, Noto Sans, IBM Plex Mono.",
  };
  *lines = kThanks;
  return (int)(sizeof(kThanks) / sizeof(kThanks[0]));
}

bool can_quit() {
  return true;
}

}  // namespace backend
