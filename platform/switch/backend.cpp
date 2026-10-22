// Nintendo Switch backend (libnx, homebrew .nro):
// - video: a linear 1920x1080 RGBA framebuffer on the default NWindow,
//   drawn into directly by the software renderer. The compositor scales it
//   to whatever the display is, so one frame serves docked and handheld;
// - input: the standard pad, with the sticks read through libnx's
//   HidNpadButton_Any* pseudo-buttons so a stick behaves like the D-pad,
//   plus the touchscreen;
// - text: the system software keyboard (swkbd), as NX-Shell uses it.
//
// Layered the same way as the PS5 backend it was ported from: everything
// above backend.h is shared, unchanged code.

#include "backend.h"

#include <switch.h>

#include "i18n.h"
#include "switch_init.h"
#include "usb_drives.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace backend {
namespace {

/* ---------- video ---------- */

/* Matches the space the UI is laid out in, so nothing is resampled.
   Changing these two numbers (and RENDER_* in platform_common.cpp) is all
   it takes to trade sharpness for speed. */
constexpr unsigned kWidth = 1920, kHeight = 1080;

NWindow *g_window = nullptr;
Framebuffer g_fb;
bool g_fb_ready = false;
bool g_headless = false;

/* ---------- input ---------- */

struct ButtonMap {
  std::uint64_t bits;
  Action action;
};

// The PS5 layout mapped onto the Switch by position, not by letter: Cross
// (confirm) is A, Circle (back) is B, Square (mark) is Y and Triangle
// (transfer) is X -- the same four corners under the same thumb. OPTIONS
// becomes Plus and the touchpad (the transfer queue) becomes Minus.
const ButtonMap kButtons[] = {
    {HidNpadButton_A, ACT_CROSS},        {HidNpadButton_B, ACT_CIRCLE},
    {HidNpadButton_Y, ACT_SQUARE},       {HidNpadButton_X, ACT_TRIANGLE},
    {HidNpadButton_Plus, ACT_OPTIONS},   {HidNpadButton_Minus, ACT_TOUCHPAD},
    {HidNpadButton_L, ACT_L1},           {HidNpadButton_R, ACT_R1},
    {HidNpadButton_ZL, ACT_L2},          {HidNpadButton_ZR, ACT_R2},
    // D-pad and both sticks: libnx folds the sticks into these masks, so a
    // stick repeats and releases exactly like the D-pad does.
    {HidNpadButton_AnyUp, ACT_UP},       {HidNpadButton_AnyDown, ACT_DOWN},
    {HidNpadButton_AnyLeft, ACT_LEFT},   {HidNpadButton_AnyRight, ACT_RIGHT},
};

PadState g_pad;
std::uint64_t g_held = 0;  // one bit per kButtons entry, not per hardware button
bool g_touch_ready = false;
bool g_keyboard_failed = false;

/* Collapses the raw button mask into one bit per action, so overlapping
   masks (D-pad Up and stick Up) can't press the same action twice. */
std::uint64_t actions_held(std::uint64_t buttons) {
  std::uint64_t held = 0;
  for(std::size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++) {
    if(buttons & kButtons[i].bits) held |= UINT64_C(1) << i;
  }
  return held;
}

bool pad_poll() {
  padUpdate(&g_pad);
  std::uint64_t held = actions_held(padGetButtons(&g_pad));
  std::uint64_t changed = held ^ g_held;
  if(!changed) return false;
  for(std::size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++) {
    if(!(changed & (UINT64_C(1) << i))) continue;
    if(held & (UINT64_C(1) << i)) platform::detail::press(kButtons[i].action);
    else platform::detail::release(kButtons[i].action);
  }
  g_held = held;
  return true;
}

/* ---------- system keyboard ---------- */

/* swkbd counts its length limit in characters; the app's fields are sized in
   bytes, so leave room for multi-byte input. */
constexpr std::size_t kMaxTextBytes = 512;

}  // namespace

std::uint64_t ticks_ms() {
  return armTicksToNs(armGetSystemTick()) / 1000000ull;
}

bool init(bool headless) {
  g_headless = headless;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  hidInitializeTouchScreen();
  g_touch_ready = true;
  if(headless) return true;  // frames go to screenshots only

  g_window = nwindowGetDefault();
  if(!g_window) return false;
  if(R_FAILED(framebufferCreate(&g_fb, g_window, kWidth, kHeight, PIXEL_FORMAT_RGBA_8888, 2))) {
    return false;
  }
  // A linear shadow buffer: the software renderer writes plain rows and libnx
  // swizzles them into the Tegra block-linear layout on framebufferEnd.
  if(R_FAILED(framebufferMakeLinear(&g_fb))) {
    framebufferClose(&g_fb);
    return false;
  }
  g_fb_ready = true;
  return true;
}

void shutdown() {
  /* framebufferClose releases both buffers, the linear shadow buffer and
     the window's hold on them. Without it the process exits still owning
     them, which is what made leaving chiral-ftp fail where other homebrew
     exits cleanly -- and in applet mode wedged the compositor badly enough
     to need a hard reboot. */
  if(g_fb_ready) {
    framebufferClose(&g_fb);
    g_fb_ready = false;
  }
  g_window = nullptr;
  /* Last, like Goldleaf's Finalize(): by now main() has stopped the servers
     and the workers, so nothing still has a file open on a drive. */
  usb_drives_stop();
}

void pump(int timeout_ms) {
  const std::uint64_t until = ticks_ms() + (std::uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
  for(;;) {
    // The system asked the app to close (HOME -> Close, or a system update).
    if(!appletMainLoop()) {
      platform::detail::quit();
      return;
    }
    if(pad_poll()) return;
    if(ticks_ms() >= until) return;
    svcSleepThread(4000000ull);  // 4 ms
  }
}

/* PIXEL_FORMAT_RGBA_8888 packs R in the low byte, which is exactly what
   IM_COL32 and the software renderer already produce -- so the renderer can
   write into this buffer as-is, with no conversion and no copy. */
std::uint32_t *frame_begin(int *stride_pixels) {
  if(!g_fb_ready) return nullptr;
  std::uint32_t stride = 0;
  void *dst = framebufferBegin(&g_fb, &stride);
  if(!dst) return nullptr;
  *stride_pixels = (int)(stride / 4);
  return static_cast<std::uint32_t *>(dst);
}

void frame_end() {
  if(g_fb_ready) framebufferEnd(&g_fb);
}

/* Only for the paths that have a finished frame in hand (the screenshots
   the automated tests take); the normal one draws through frame_begin. */
void present(const std::uint32_t *pixels) {
  int stride = 0;
  std::uint32_t *dst = frame_begin(&stride);
  if(!dst) return;
  for(unsigned y = 0; y < kHeight; y++) {
    std::memcpy(dst + (std::size_t)y * (unsigned)stride, pixels + (std::size_t)y * kWidth,
                kWidth * 4);
  }
  frame_end();
}

/* Touch arrives in panel pixels -- 1280x720 whatever the framebuffer is --
   so it is passed up unscaled; platform_common knows the UI's coordinate
   space and does the conversion. Only the first finger is reported. */
bool touch(int *x, int *y, bool *down) {
  if(!g_touch_ready) return false;
  HidTouchScreenState state = {0};
  *down = false;
  if(hidGetTouchScreenStates(&state, 1) && state.count > 0) {
    *down = true;
    *x = (int)state.touches[0].x;
    *y = (int)state.touches[0].y;
  }
  return true;
}

bool startup_notice(bool *ok, const char **line, const char **detail, int *status) {
  /* Launched over a game (R on a title in hbmenu) the app is a full
     application with the console's memory behind it; from the album it is a
     library applet, capped near 448 MB -- enough, but tight once a CJK
     glyph atlas and thirty transfer buffers are live. Worth saying, since
     it is the one thing the user can still change. */
  const AppletType type = appletGetAppletType();
  const bool full = type == AppletType_Application || type == AppletType_SystemApplication;

  *ok = full;
  *line = full ? N_("Full memory: launched over a game")
               : N_("Applet mode: {0} (type {1}). Hold R on a game in hbmenu for the full console.");
  *detail = full ? N_("launched over a game, with the console's memory")
                 : N_("started from the album, so memory is limited");
  *status = (int)type;
  return true;
}

void report_startup_services(void (*trace)(const char *fmt, ...)) {
  int count = 0;
  const chiral_service_result_t *results = chiral_service_results(&count);

  for(int i = 0; i < count; i++) {
    if(R_SUCCEEDED(results[i].rc)) trace("Service %s: ok", results[i].name);
    else trace("Service %s: FAILED, result 0x%x", results[i].name, results[i].rc);
  }
}

void early_init() {
  /* The services come up in platform/switch/init.c before main, and swkbd
     is an applet rather than a module to load. What is left is USB drives
     (usb_drives.c): before anything opens a file, so every path the app
     uses goes through the router from the start. */
  usb_drives_start();
}

bool keyboard_failed() {
  return g_keyboard_failed;
}

bool has_native_keyboard() {
  return !g_headless;
}

bool native_text_prompt(const char *title, const std::string &initial, TextKind kind,
                        std::string &out) {
  if(g_headless) return false;
  SwkbdConfig kbd;
  g_keyboard_failed = false;
  if(R_FAILED(swkbdCreate(&kbd, 0))) {
    g_keyboard_failed = true;
    return false;
  }

  if(kind == TEXT_PASSWORD) swkbdConfigMakePresetPassword(&kbd);
  else swkbdConfigMakePresetDefault(&kbd);
  if(kind == TEXT_NUMBER) swkbdConfigSetType(&kbd, SwkbdType_NumPad);
  else if(kind == TEXT_URL) swkbdConfigSetType(&kbd, SwkbdType_QWERTY);
  if(title && *title) swkbdConfigSetHeaderText(&kbd, title);
  if(!initial.empty()) swkbdConfigSetInitialText(&kbd, initial.c_str());
  swkbdConfigSetStringLenMax(&kbd, (std::uint32_t)kMaxTextBytes - 1);
  swkbdConfigSetInitialCursorPos(&kbd, 1);  // cursor at the end of the initial text

  std::vector<char> buffer(kMaxTextBytes, 0);
  Result r = swkbdShow(&kbd, buffer.data(), buffer.size());
  swkbdClose(&kbd);
  if(R_FAILED(r)) return false;  // also the path the user takes by cancelling

  out.assign(buffer.data());
  // Buttons pressed while the keyboard had the pad belong to it, not to us.
  padUpdate(&g_pad);
  g_held = actions_held(padGetButtons(&g_pad));
  return true;
}

void set_text_input(bool) {}

const char *about_file_manager() {
  return N_("  File manager: copy, move and delete files anywhere on the SD card.");
}

int about_thanks(const char *const **lines) {
  static const char *const kThanks[] = {
      "libnx and devkitPro (switchbrew, fincs, WinterMute), ftpd (Michael Theall), NX-Shell (Joel16),",
      "websrv and ftpsrv (John T\xc3\xb6rnblom), libssh2, Mbed TLS, GNU libmicrohttpd, Dear ImGui, Noto Sans, IBM Plex Mono,",
      "libusbhsfs (DarkMatterCore), NTFS-3G, lwext4.",
  };
  *lines = kThanks;
  return (int)(sizeof(kThanks) / sizeof(kThanks[0]));
}

bool can_quit() {
  // Unlike a PS5 title, homebrew returns to hbmenu when main() returns.
  return true;
}

bool window_size(int *pt_w, int *pt_h, int *px_w, int *px_h) {
  /* No window: the fixed 1920x1080 layout, drawn at CHIRAL_RENDER_W x
     CHIRAL_RENDER_H. */
  (void)pt_w;
  (void)pt_h;
  (void)px_w;
  (void)px_h;
  return false;
}

void pointer_hand(bool on) {
  (void)on;  // no mouse
}

bool open_url(const char *url) {
  (void)url;  // no browser
  return false;
}

/* What appletMainLoop() (called from pump) last heard from the system: in
   focus, or out of it -- the HOME menu, an overlay, the app suspended. A
   read of libnx's own state, not a call to the system. */
bool in_foreground() {
  return appletGetFocusState() == AppletFocusState_InFocus;
}

bool copy_text(const char *text) {
  (void)text;  // no clipboard
  return false;
}

bool choose_folder(const char *start, std::string &out) {
  (void)start;
  (void)out;
  return false;
}

}  // namespace backend
