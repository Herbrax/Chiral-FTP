// PS5 backend for a native title (ShadowMountPlus):
// - video: the boilerplate's hardware-verified VideoOut setup (two 1080p
//   buffers in direct memory, tiled layout), fed from the software renderer;
// - input: ScePad state for the foreground user, as ProsperoEden's launcher reads it;
// - text: the system keyboard, after loading its module the way ProsperoEden does.
// Based on ps5-native-app-boilerplate (C) 2026 BlackBearReloaded, GPL-3.0-or-later.

#include "backend.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

extern "C" {
int sceSystemServiceHideSplashScreen(void);
std::size_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(std::int64_t search_start, std::int64_t search_end, std::size_t length,
                                  std::size_t alignment, int memory_type, std::int64_t *physical_address);
int sceKernelMapDirectMemory(void **address, std::size_t length, int protection, int flags,
                             std::int64_t physical_address, std::size_t alignment);
int sceKernelUsleep(std::uint32_t microseconds);
int sceVideoOutOpen(std::int32_t user_id, std::int32_t bus_type, std::int32_t index, const void *param);
int sceVideoOutSetFlipRate(std::int32_t handle, std::int32_t rate);
int sceVideoOutSubmitFlip(std::int32_t handle, std::int32_t buffer_index, std::uint32_t flip_mode,
                          std::int64_t flip_argument);
int sceVideoOutWaitVblank(std::int32_t handle);

int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(std::int32_t *user_id);
int scePadInit(void);
int scePadOpen(std::int32_t user_id, std::int32_t port_type, std::int32_t index, const void *param);
int scePadReadState(std::int32_t handle, void *data);

int sceSysmoduleLoadModule(std::uint16_t module_id);
int sceCommonDialogInitialize(void);
int sceImeDialogInit(const void *param, const void *extended);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(void *result);
int sceImeDialogTerm(void);
}

extern "C" void startup_trace(const char *fmt, ...);

namespace backend {
namespace {

/* ---------- video (from the boilerplate's demo_renderer.cpp) ---------- */

constexpr unsigned kWidth = 1920, kHeight = 1080;
constexpr std::size_t kFrameBytes = 0x1000000;
constexpr std::size_t kMemoryBytes = kFrameBytes * 2;
constexpr std::size_t kMemoryAlignment = 0x200000;
constexpr int kMemoryTypeWcGarlic = 3;
constexpr int kMapProtection = 0x33;
constexpr std::uint64_t kPixelFormatRgba8Srgb = UINT64_C(0x8000000022000000);

struct VideoBuffer {
  void *data;
  void *metadata;
  void *reserved0;
  void *reserved1;
};

struct VideoAttribute {
  std::uint8_t reserved[80];
};

}  // namespace
}  // namespace backend

extern "C" void sceVideoOutSetBufferAttribute2(backend::VideoAttribute *attribute, std::uint64_t pixel_format,
                                               std::uint32_t tiling_mode, std::uint32_t width, std::uint32_t height,
                                               std::uint64_t option, std::uint32_t dcc_control,
                                               std::uint64_t dcc_clear_color);
extern "C" int sceVideoOutRegisterBuffers2(std::int32_t handle, std::int32_t set_index,
                                           std::int32_t buffer_index_start, backend::VideoBuffer *buffers,
                                           std::int32_t buffer_count, backend::VideoAttribute *attribute,
                                           std::int32_t category, void *option);

namespace backend {
namespace {

int g_video = -1;
std::uint8_t *g_buffers[2] = {nullptr, nullptr};
int g_back = 1;
std::int64_t g_flip = 1;
// Tiled layout: offset = ((ybits[y] ^ xbits[x]) + yblock[y] + xblock[x]).
std::uint32_t g_xbits[kWidth], g_xblock[kWidth], g_ybits[kHeight], g_yblock[kHeight];

void build_tile_tables() {
  const std::uint32_t blocks_per_row = (kWidth + 127U) >> 7;
  for(std::uint32_t x = 0; x < kWidth; x++) {
    g_xbits[x] = ((x << 2) & 0xcU) ^ ((x << 5) & 0x380U) ^ ((x << 4) & 0x400U) ^ ((x << 6) & 0x800U) ^
                 ((x << 9) & 0xa000U);
    g_xblock[x] = (x >> 7) << 16;
  }
  for(std::uint32_t y = 0; y < kHeight; y++) {
    g_ybits[y] = ((y << 4) & 0x70U) ^ ((y << 5) & 0xf00U) ^ ((y << 9) & 0x1000U) ^ ((y << 8) & 0x4000U);
    g_yblock[y] = ((y >> 7) * blocks_per_row) << 16;
  }
}

void flush_range(void *address, std::size_t length) {
  auto *at = static_cast<std::uint8_t *>(address);
  const auto *end = at + length;
  for(; at < end; at += 64) __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
  __asm__ volatile("mfence" ::: "memory");
}

bool video_init() {
  if((g_video = sceVideoOutOpen(0xff, 0, 0, nullptr)) < 0) return false;
  const std::size_t pool = sceKernelGetDirectMemorySize();
  if(pool < kMemoryBytes) return false;
  std::int64_t physical = 0;
  if(sceKernelAllocateDirectMemory(0, (std::int64_t)pool, kMemoryBytes, kMemoryAlignment, kMemoryTypeWcGarlic,
                                   &physical) < 0) {
    return false;
  }
  void *mapped = nullptr;
  if(sceKernelMapDirectMemory(&mapped, kMemoryBytes, kMapProtection, 0, physical, kMemoryAlignment) < 0) {
    return false;
  }
  g_buffers[0] = static_cast<std::uint8_t *>(mapped);
  g_buffers[1] = g_buffers[0] + kFrameBytes;
  std::memset(mapped, 0, kMemoryBytes);
  flush_range(mapped, kMemoryBytes);
  VideoBuffer buffers[2] = {{g_buffers[0], nullptr, nullptr, nullptr}, {g_buffers[1], nullptr, nullptr, nullptr}};
  VideoAttribute attribute{};
  (void)sceVideoOutSetFlipRate(g_video, 0);
  sceVideoOutSetBufferAttribute2(&attribute, kPixelFormatRgba8Srgb, 0, kWidth, kHeight, 0, 0, 0);
  if(sceVideoOutRegisterBuffers2(g_video, 0, 0, buffers, 2, &attribute, 0, nullptr) < 0) return false;
  if(sceVideoOutSubmitFlip(g_video, 0, 1, g_flip++) < 0) return false;
  (void)sceVideoOutWaitVblank(g_video);
  build_tile_tables();
  return true;
}

/* ---------- input ---------- */

constexpr std::uint32_t kButtonIntercepted = UINT32_C(0x80000000);  // the system overlay has the pad

struct ButtonMap {
  std::uint32_t bit;
  Action action;
};

const ButtonMap kButtons[] = {
  {0x00004000, ACT_CROSS},   {0x00002000, ACT_CIRCLE}, {0x00008000, ACT_SQUARE},
  {0x00001000, ACT_TRIANGLE}, {0x00000008, ACT_OPTIONS}, {0x00100000, ACT_TOUCHPAD},
  {0x00000400, ACT_L1},      {0x00000800, ACT_R1},     {0x00000100, ACT_L2},
  {0x00000200, ACT_R2},      {0x00000010, ACT_UP},     {0x00000040, ACT_DOWN},
  {0x00000080, ACT_LEFT},    {0x00000020, ACT_RIGHT},
};

int g_pad = -1;
std::int32_t g_user = -1;
std::uint32_t g_buttons = 0;
Action g_stick = ACT_NONE;
std::uint64_t g_next_pad_try = 0;

void pad_open() {
  if(g_user < 0 && sceUserServiceGetForegroundUser(&g_user) < 0) {
    g_user = -1;
    return;
  }
  g_pad = scePadOpen(g_user, 0, 0, nullptr);
}

Action stick_action(std::uint8_t x, std::uint8_t y) {
  int dx = (int)x - 128, dy = (int)y - 128;
  int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
  if(ax < 64 && ay < 64) return ACT_NONE;
  if(ax > ay) return x < 64 ? ACT_LEFT : x > 192 ? ACT_RIGHT : ACT_NONE;
  return y < 64 ? ACT_UP : y > 192 ? ACT_DOWN : ACT_NONE;
}

bool pad_poll() {
  alignas(16) std::uint8_t state[256];
  std::uint32_t buttons;
  bool any = false;

  if(g_pad < 0) {
    if(ticks_ms() >= g_next_pad_try) {
      pad_open();
      g_next_pad_try = ticks_ms() + 500;
    }
    return false;
  }
  std::memset(state, 0, sizeof(state));
  if(scePadReadState(g_pad, state) < 0) return false;
  std::memcpy(&buttons, state, sizeof(buttons));
  // byte 76: connected; the high bit: input taken by the system (PS menu).
  if(state[76] == 0 || (buttons & kButtonIntercepted)) buttons = 0;
  std::uint32_t changed = g_buttons ^ buttons;
  for(const ButtonMap &b : kButtons) {
    if(!(changed & b.bit)) continue;
    any = true;
    if(buttons & b.bit) platform::detail::press(b.action);
    else platform::detail::release(b.action);
  }
  g_buttons = buttons;
  Action stick = (buttons == 0 && state[76] == 0) ? ACT_NONE : stick_action(state[4], state[5]);
  if(stick != g_stick) {
    if(g_stick != ACT_NONE) platform::detail::release(g_stick);
    if(stick != ACT_NONE) platform::detail::press(stick);
    g_stick = stick;
    any = true;
  }
  return any;
}

/* ---------- system keyboard (from ProsperoEden's system_keyboard.cpp) ---------- */

struct ImeParam {
  std::int32_t user_id, type;
  std::uint64_t supported_languages;
  std::int32_t enter_label, input_method;
  void *filter;
  std::uint32_t option, max_text_length;
  std::uint16_t *input_text_buffer;
  float pos_x, pos_y;
  std::int32_t horizontal_alignment, vertical_alignment;
  const std::uint16_t *placeholder;
  const std::uint16_t *title;
  std::int8_t reserved[16];
};
struct ImeResult {
  std::int32_t outcome;
  std::int8_t reserved[12];
};
static_assert(sizeof(ImeParam) == 96, "ImeParam layout");
static_assert(sizeof(ImeResult) == 16, "ImeResult layout");

constexpr std::uint16_t kImeDialogModule = 0x0096;
constexpr std::uint32_t kCommonDialogAlreadyInitialized = 0x80b80002u;
constexpr std::int32_t kTypeBasicLatin = 1, kTypeUrl = 2, kTypeNumber = 4;
constexpr std::uint32_t kOptionPassword = 0x4;
constexpr std::int32_t kAlignCenter = 1;
constexpr int kStatusNone = 0, kStatusRunning = 1, kStatusFinished = 2;

int g_ime_module = -1;
bool g_dialogs_ready = false;
bool g_keyboard_failed = false;

std::vector<std::uint16_t> to_utf16(const std::string &in, std::size_t limit) {
  std::vector<std::uint16_t> out;
  for(std::size_t i = 0; i < in.size() && out.size() + 2 < limit;) {
    unsigned char c = (unsigned char)in[i];
    unsigned cp;
    int n;
    if(c < 0x80) { cp = c; n = 1; }
    else if((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else { cp = c & 0x07; n = 4; }
    for(int k = 1; k < n && i + k < in.size(); k++) cp = cp << 6 | (in[i + k] & 0x3F);
    i += n;
    if(cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back((std::uint16_t)(0xD800 + (cp >> 10)));
      out.push_back((std::uint16_t)(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back((std::uint16_t)cp);
    }
  }
  out.resize(limit + 1, 0);
  return out;
}

std::string from_utf16(const std::uint16_t *in) {
  std::string out;
  for(std::size_t i = 0; in[i]; i++) {
    unsigned cp = in[i];
    if(cp >= 0xD800 && cp < 0xDC00 && in[i + 1]) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
      i++;
    }
    if(cp < 0x80) out += (char)cp;
    else if(cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 0x3F)); }
    else if(cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | (cp >> 12 & 0x3F)); out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
  }
  return out;
}

}  // namespace

std::uint64_t ticks_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (std::uint64_t)ts.tv_sec * 1000 + (std::uint64_t)ts.tv_nsec / 1000000;
}

bool init(bool headless) {
  (void)headless;
  (void)sceSystemServiceHideSplashScreen();
  if(!video_init()) return false;
  (void)sceUserServiceInitialize(nullptr);
  (void)scePadInit();
  pad_open();
  return true;
}

void pump(int timeout_ms) {
  std::uint64_t until = ticks_ms() + (std::uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
  for(;;) {
    if(pad_poll()) return;
    if(ticks_ms() >= until) return;
    (void)sceKernelUsleep(8000);
  }
}

void present(const std::uint32_t *pixels) {
  std::uint8_t *dst = g_buffers[g_back];
  if(!dst) return;
  for(unsigned y = 0; y < kHeight; y++) {
    const std::uint32_t *row = pixels + (std::size_t)y * kWidth;
    const std::uint32_t yb = g_ybits[y], yk = g_yblock[y];
    for(unsigned x = 0; x < kWidth; x++) {
      *reinterpret_cast<std::uint32_t *>(dst + ((yb ^ g_xbits[x]) + yk + g_xblock[x])) = row[x];
    }
  }
  flush_range(dst, kFrameBytes);
  while(sceVideoOutSubmitFlip(g_video, g_back, 1, g_flip) < 0) {
    (void)sceKernelUsleep(4000);  // the previous flip is still queued
  }
  g_flip++;
  (void)sceVideoOutWaitVblank(g_video);
  g_back ^= 1;
}

bool has_native_keyboard() {
  return true;
}

void prepare_keyboard() {
  if(g_ime_module < 0) g_ime_module = sceSysmoduleLoadModule(kImeDialogModule);
}

bool keyboard_failed() {
  return g_keyboard_failed;
}

bool native_text_prompt(const char *title, const std::string &initial, TextKind kind, std::string &out) {
  g_keyboard_failed = true;  // until the dialog really opens
  if(g_ime_module < 0 && (g_ime_module = sceSysmoduleLoadModule(kImeDialogModule)) < 0) {
    startup_trace("The PS5 keyboard's module did not load (%#x)", (unsigned)g_ime_module);
    return false;
  }
  if(!g_dialogs_ready) {
    int r = sceCommonDialogInitialize();
    if(r < 0 && (std::uint32_t)r != kCommonDialogAlreadyInitialized) {
      startup_trace("The PS5's dialogs did not start (%#x)", (unsigned)r);
      return false;
    }
    g_dialogs_ready = true;
  }
  const std::size_t limit = 255;
  std::vector<std::uint16_t> text = to_utf16(initial, limit);
  std::vector<std::uint16_t> wtitle = to_utf16(title, 127);
  std::vector<std::uint16_t> placeholder = to_utf16("", 1);
  ImeParam param{};
  if(sceUserServiceGetForegroundUser(&param.user_id) < 0) return false;
  // Password masking needs the basic Latin keyboard.
  param.type = kind == TEXT_NUMBER ? kTypeNumber : kind == TEXT_PASSWORD ? kTypeBasicLatin :
               kind == TEXT_URL ? kTypeUrl : 0;
  param.option = kind == TEXT_PASSWORD ? kOptionPassword : 0;
  param.max_text_length = (std::uint32_t)limit;
  param.input_text_buffer = text.data();
  param.horizontal_alignment = param.vertical_alignment = kAlignCenter;
  param.pos_x = 960;
  param.pos_y = 540;
  param.placeholder = placeholder.data();
  param.title = wtitle.data();
  if(int r = sceImeDialogInit(&param, nullptr); r != 0) {
    startup_trace("The PS5 keyboard did not open (%#x)", (unsigned)r);
    return false;
  }
  g_keyboard_failed = false;  // open: from here on, false means cancelled

  bool ok = false;
  const std::uint64_t since = ticks_ms();
  for(;;) {
    (void)sceKernelUsleep(50000);
    const int status = sceImeDialogGetStatus();
    // The status reads "none" for a moment after the dialog was asked for.
    if(status == kStatusRunning || (status == kStatusNone && ticks_ms() - since < 1000)) continue;
    ImeResult result{};
    if(status == kStatusFinished && sceImeDialogGetResult(&result) >= 0 && result.outcome == 0) {
      out = from_utf16(text.data());
      ok = true;
    }
    break;
  }
  for(int tries = 0; tries < 100 && sceImeDialogTerm() < 0; ++tries) (void)sceKernelUsleep(50000);
  // Buttons pressed while the keyboard was open belong to it.
  g_buttons = 0xFFFFFFFFu;
  return ok;
}

void set_text_input(bool) {}

bool can_quit() {
  return false;
}

}  // namespace backend
