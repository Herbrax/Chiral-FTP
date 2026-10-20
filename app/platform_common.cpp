// Shared platform code: controller actions with key repeat, Dear ImGui with
// the embedded fonts, software-rendered frames and the drawing helpers.

#include "platform.h"
#include "platform_config.h"  // before the CHIRAL_BIG_ALLOC_VIA_MMAP test below

#include <cerrno>
#include <cstdlib>
#include <new>
#include <fcntl.h>
#if CHIRAL_BIG_ALLOC_VIA_MMAP
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

// A private copy of ImGui's font parser, to check a font before ImGui gets it.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "imstb_truetype.h"
#pragma GCC diagnostic pop

/* The startup log (engine/log.c), weak so the platform tests link without it. */
extern "C" void startup_trace(const char *fmt, ...) __attribute__((weak));
#define TRACE(...) do { if(startup_trace) startup_trace(__VA_ARGS__); } while(0)

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "backend.h"
#include "i18n.h"
#include "platform_config.h"
#include "raster.h"

extern "C" {
#include "system.h"
}

extern "C" {
extern const unsigned char font_regular_ttf[];
extern const unsigned long font_regular_ttf_size;
extern const unsigned char font_bold_ttf[];
extern const unsigned long font_bold_ttf_size;
extern const unsigned char font_mono_ttf[];  // IBM Plex Mono Medium: the wordmark
extern const unsigned long font_mono_ttf_size;
}

namespace platform {

ImFont *font_regular = nullptr;
ImFont *font_bold = nullptr;
ImFont *font_mono = nullptr;

namespace {

/* Extra script fonts (CJK, Arabic) are merged into the app's Noto Sans,
   the language in use first so Han characters that Japanese and Chinese
   share take the right forms. Which fonts those are is the platform's
   answer: see system_script_fonts. */

#if !CHIRAL_BIG_ALLOC_VIA_MMAP

/* The app's memory. Where the C library's heap is the whole heap, and needs none: libnx hands the
   process its whole heap at startup and newlib's malloc draws from all of
   it, so the PS5's workaround below -- big blocks straight from the system
   because its libc heap was capped far under the memory the app was allowed
   -- has nothing left to work around. In applet mode the cap is the
   console's, which no allocator trick can lift (see ui::set_launch_mode). */
void *app_alloc(std::size_t size) {
  return malloc(size);
}

void app_free(void *ptr) {
  free(ptr);
}

#else

/* The app's memory. The C library heap has a cap of its own, below the
   memory the app may use: with the Japanese, Korean, Chinese or Arabic fonts
   the glyph atlas grows past it. So blocks of 32 KB and more come straight
   from the system (mmap); small ones stay on the heap. A 16-byte header in
   front of each block says which, and its size. */
const std::size_t kMappedFrom = 32 * 1024;
const std::size_t kHeader = 16;
const std::size_t kMapped = 0x4d415050u;  // "MAPP"
const std::size_t kHeap = 0x48454150u;    // "HEAP"

void *app_alloc(std::size_t size) {
  std::size_t total = size + kHeader;
  std::size_t *p;
  if(total >= kMappedFrom) {
    void *m = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if(m == MAP_FAILED) return nullptr;
    p = (std::size_t *)m;
    p[1] = kMapped;
  } else {
    if(!(p = (std::size_t *)malloc(total))) return nullptr;
    p[1] = kHeap;
  }
  p[0] = size;
  return (char *)p + kHeader;
}


void app_free(void *ptr) {
  if(!ptr) return;
  std::size_t *p = (std::size_t *)((char *)ptr - kHeader);
  if(p[1] == kMapped) munmap(p, p[0] + kHeader);
  else free(p);
}

#endif  // !CHIRAL_BIG_ALLOC_VIA_MMAP


struct MappedFont {
  const void *data;
  size_t size;
};
std::vector<MappedFont> g_script_fonts;

/* Checks a font with the parser before ImGui gets it, so something we can't
   read is skipped (its script then falls back) instead of failing inside the
   font atlas. */
bool check_font(const void *data, size_t size, const char *what) {
  stbtt_fontinfo info;
  int offset = stbtt_GetFontOffsetForIndex((const unsigned char *)data, 0);
  if(offset < 0 || !stbtt_InitFont(&info, (const unsigned char *)data, offset)) {
    TRACE("Font %s: not a font the renderer can read; skipped", what);
    return false;
  }
  TRACE("Font %s (%lld bytes)", what, (long long)size);
  return true;
}


void load_script_fonts(const char *current_lang) {
  /* Scripts the app's own Noto Sans doesn't cover -- CJK, Arabic -- come
     from the platform, which knows what faces the machine has and what
     order to put them in (engine/system.h). A computer answers with one
     font that covers everything; a console with its own, or with none. */
  system_font_t fonts[8];
  const int n = system_script_fonts(fonts, (int)(sizeof(fonts) / sizeof(fonts[0])), current_lang);

  for(int i = 0; i < n; i++) {
    char what[32];
    snprintf(what, sizeof(what), "script font %d", i + 1);
    if(!fonts[i].data || fonts[i].size < 1024) continue;
    if(!check_font(fonts[i].data, fonts[i].size, what)) continue;
    g_script_fonts.push_back({fonts[i].data, fonts[i].size});
  }
}

// Merges the script fonts into the font just added.
void merge_script_fonts(ImGuiIO &io) {
  for(const MappedFont &font : g_script_fonts) {
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.FontDataOwnedByAtlas = false;  // not ImGui's to free
    // The console's fonts draw larger than Noto Sans at the same size; 26 matches them.
    io.Fonts->AddFontFromMemoryTTF(const_cast<void *>(font.data), (int)font.size, 26.0f, &cfg);
  }
}

}  // namespace

namespace {

/* The UI is written in a 1920x1080 space and ImGui is told so, which is
   what keeps one layout working on both consoles and on a PC window.

   What it is rasterised into is a separate question. On the Switch that is
   a 720p framebuffer: every pixel costs CPU in the rasteriser and again in
   libnx's swizzle to the Tegra layout, and at 1080p a d-pad press took long
   enough to feel broken. The draw data is scaled on its way out, so no
   coordinate in the UI changes. */
const int W = 1920, H = 1080;
/* What this platform actually rasterises into (platform_config.h). Usually
   the same as the layout, and then nothing is scaled. It was 720p on the
   Switch for a while, when a frame cost ~95 ms and almost all of it was the
   rasteriser walking bounding boxes; with that fixed, full size is
   affordable and text is drawn rather than resampled. Keeping these
   separate from W/H is what makes a smaller target a one-line change. */
const int RENDER_W = CHIRAL_RENDER_W, RENDER_H = CHIRAL_RENDER_H;
const std::uint64_t REPEAT_DELAY = 380;
const std::uint64_t REPEAT_RATE = 70;

std::deque<Action> g_queue;
Action g_held = ACT_NONE;
std::uint64_t g_next_repeat = 0;
/* Back, held: a way out of a deep folder without tapping B all the way up.
   The short press still fires on the way down, so a hold goes up one level
   and then home -- which is where it was heading anyway. */
const std::uint64_t BACK_LONG_MS = 600;
std::uint64_t g_back_since = 0;  // 0 when Back isn't down
bool g_back_long_sent = false;
bool g_text_input = false;
std::string g_typed;
bool g_backspace = false, g_enter = false, g_escape = false;
std::vector<std::uint32_t> g_frame;
std::string g_screenshot;
std::uint64_t g_last_frame = 0;

/* The touchscreen. The backend reports a finger in panel pixels; this turns
   that into the events the UI asks about, in the UI's coordinate space.
   A press that travels more than a few pixels becomes a drag and stops
   counting as a tap, so scrolling a list doesn't also open something. */
const float TOUCH_DRAG_SLOP = 12.0f;
bool g_touch_present = false;
bool g_touch_known = false;
bool g_touch_down = false;     // what the hardware last said
Touch g_touch{};               // what the UI has not yet taken
float g_touch_last_y = 0;
float g_touch_start_x = 0, g_touch_start_y = 0;

void poll_touch() {
  int tx = 0, ty = 0;
  bool down = false;
  if(!backend::touch(&tx, &ty, &down)) {
    g_touch_known = true;
    g_touch_present = false;
    return;
  }
  g_touch_known = true;
  g_touch_present = true;

  /* Panel pixels to UI pixels. The panel is 1280x720 regardless of what
     the framebuffer happens to be, so this is the UI size over that. */
  const float x = (float)tx * (float)W / 1280.0f;
  const float y = (float)ty * (float)H / 720.0f;

  if(down) {
    if(!g_touch_down) {
      g_touch.pressed = true;
      g_touch.moved = false;
      g_touch_start_x = x;
      g_touch_start_y = y;
      g_touch_last_y = y;
    } else {
      g_touch.drag_y += y - g_touch_last_y;
      g_touch_last_y = y;
      const float dx = x - g_touch_start_x, dy = y - g_touch_start_y;
      if(dx * dx + dy * dy > TOUCH_DRAG_SLOP * TOUCH_DRAG_SLOP) g_touch.moved = true;
    }
    g_touch.held = true;
    g_touch.x = x;
    g_touch.y = y;
  } else if(g_touch_down) {
    g_touch.released = true;
    g_touch.held = false;
  }
  g_touch_down = down;
}

bool repeats(Action a) {
  return a == ACT_UP || a == ACT_DOWN || a == ACT_LEFT || a == ACT_RIGHT || a == ACT_L2 ||
         a == ACT_R2;
}

// 24-bit BMP, bottom-up rows, so tests need no image library. Writes what
// was actually rasterised, which is the render size, not the UI's.
bool write_bmp(const std::string &path, const std::uint32_t *px) {
  FILE *f = fopen(path.c_str(), "wb");
  if(!f) return false;
  const int row = RENDER_W * 3;
  const unsigned size = 54 + row * RENDER_H;
  unsigned char h[54] = {'B', 'M'};
  auto put32 = [&](int o, unsigned v) { for(int i = 0; i < 4; i++) h[o + i] = (unsigned char)(v >> (8 * i)); };
  put32(2, size);
  put32(10, 54);
  put32(14, 40);
  put32(18, RENDER_W);
  put32(22, RENDER_H);
  h[26] = 1;
  h[28] = 24;
  put32(34, row * RENDER_H);
  fwrite(h, 1, 54, f);
  std::vector<unsigned char> line(row);
  for(int y = RENDER_H - 1; y >= 0; y--) {
    for(int x = 0; x < RENDER_W; x++) {
      std::uint32_t c = px[(size_t)y * RENDER_W + x];
      line[x * 3] = (unsigned char)(c >> 16);
      line[x * 3 + 1] = (unsigned char)(c >> 8);
      line[x * 3 + 2] = (unsigned char)c;
    }
    fwrite(line.data(), 1, row, f);
  }
  return fclose(f) == 0;
}

}  // namespace

namespace detail {

void press(Action a) {
  if(a == ACT_NONE) return;
  if(g_text_input) return;  // keystrokes belong to the text entry
  if(a == ACT_CIRCLE) {
    /* Back is the one action that waits for the button to come up: queuing
       it on the way down meant a hold went up a folder and only then out to
       the start screen. Now a short press goes back and a long one doesn't. */
    g_back_since = backend::ticks_ms();
    g_back_long_sent = false;
    return;
  }
  g_queue.push_back(a);
  if(repeats(a)) {
    g_held = a;
    g_next_repeat = backend::ticks_ms() + REPEAT_DELAY;
  }
}

void release(Action a) {
  if(g_held == a) g_held = ACT_NONE;
  if(a == ACT_CIRCLE) {
    /* Let go before it counted as a hold: that was an ordinary Back. */
    if(g_back_since && !g_back_long_sent) g_queue.push_back(ACT_CIRCLE);
    g_back_since = 0;
  }
}

void typed(const char *text) {
  if(g_text_input) g_typed += text;
}

void key_backspace() { g_backspace = true; }
void key_enter() { g_enter = true; }
void key_escape() { g_escape = true; }
void quit() { g_queue.push_back(ACT_QUIT); }

}  // namespace detail

bool init(bool headless) {
  if(!backend::init(headless)) return false;
  g_frame.assign((size_t)RENDER_W * RENDER_H, 0);
  IMGUI_CHECKVERSION();
  // ImGui's allocations, so a failed one names its size in the startup log.
  // ImGui's memory: big blocks from the system, not the capped heap.
  ImGui::SetAllocatorFunctions(
      [](size_t size, void *) -> void * {
        void *p = app_alloc(size);
        if(!p) {
          TRACE("ImGui could not allocate %zu bytes (%s)", size, strerror(errno));
          return nullptr;
        }
        return p;
      },
      [](void *ptr, void *) { app_free(ptr); });
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.DisplaySize = ImVec2((float)W, (float)H);
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  io.BackendRendererName = "chiral-raster";
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;
  load_script_fonts(i18n_current());
  font_regular = io.Fonts->AddFontFromMemoryTTF((void *)font_regular_ttf, (int)font_regular_ttf_size,
                                                 30.0f, &cfg);
  merge_script_fonts(io);
  font_bold = io.Fonts->AddFontFromMemoryTTF((void *)font_bold_ttf, (int)font_bold_ttf_size, 30.0f,
                                              &cfg);
  merge_script_fonts(io);
  font_mono = io.Fonts->AddFontFromMemoryTTF((void *)font_mono_ttf, (int)font_mono_ttf_size, 30.0f, &cfg);
  g_last_frame = backend::ticks_ms();
  return font_regular && font_bold && font_mono;
}

void shutdown() {
  /* ImGui first -- it owns the font atlas and the draw lists, and nothing
     should be drawing once the framebuffer is gone. */
  ImGui::DestroyContext();
  backend::shutdown();
}

void push_action(Action a) {
  g_queue.push_back(a);
}

void inject_text(const std::string &text, bool enter) {
  g_typed += text;
  g_enter = g_enter || enter;
}

Action poll(int timeout_ms) {
  if(g_queue.empty()) {
    std::uint64_t now = backend::ticks_ms();
    int wait = timeout_ms;

    if(g_held != ACT_NONE) {
      long until = (long)(g_next_repeat - now);
      if(until < wait) wait = until < 0 ? 0 : (int)until;
    }
    if(g_back_since && !g_back_long_sent) {
      long until = (long)(g_back_since + BACK_LONG_MS - now);
      if(until < wait) wait = until < 0 ? 0 : (int)until;
    }
    backend::pump(wait);
    poll_touch();
    if(g_held != ACT_NONE && backend::ticks_ms() >= g_next_repeat) {
      g_queue.push_back(g_held);
      g_next_repeat = backend::ticks_ms() + REPEAT_RATE;
    }
    if(g_back_since && !g_back_long_sent &&
       backend::ticks_ms() - g_back_since >= BACK_LONG_MS) {
      g_back_long_sent = true;
      g_queue.push_back(ACT_CIRCLE_LONG);
    }
  }
  if(!g_touch_known) poll_touch();
  if(g_queue.empty()) return ACT_NONE;
  Action a = g_queue.front();
  g_queue.pop_front();
  return a;
}

void begin_frame() {
  ImGuiIO &io = ImGui::GetIO();
  std::uint64_t now = backend::ticks_ms();
  io.DeltaTime = now > g_last_frame ? (float)(now - g_last_frame) / 1000.0f : 1.0f / 60.0f;
  g_last_frame = now;
  ImGui::NewFrame();
}

void end_frame() {
  ImGui::Render();
  ImDrawData *data = ImGui::GetDrawData();
  raster::update_textures(data);

  if(RENDER_W != W || RENDER_H != H) {
    /* Scale the finished draw data instead of the UI. A few thousand
       vertices, against two million pixels saved. */
    const float kx = (float)RENDER_W / (float)W, ky = (float)RENDER_H / (float)H;
    for(ImDrawList *list : data->CmdLists) {
      for(ImDrawVert &v : list->VtxBuffer) {
        v.pos.x *= kx;
        v.pos.y *= ky;
      }
      for(ImDrawCmd &cmd : list->CmdBuffer) {
        cmd.ClipRect.x *= kx;
        cmd.ClipRect.z *= kx;
        cmd.ClipRect.y *= ky;
        cmd.ClipRect.w *= ky;
      }
    }
  }

  const std::uint32_t bg = IM_COL32(11, 18, 32, 255);
  int stride = 0;
  std::uint32_t *direct = g_screenshot.empty() ? backend::frame_begin(&stride) : nullptr;
  if(direct) {
    // Straight into the screen's own buffer: no full-frame copy at all.
    raster::render(data, raster::Target{direct, RENDER_W, RENDER_H, stride}, bg);
    backend::frame_end();
  } else {
    raster::render(data, raster::Target{g_frame.data(), RENDER_W, RENDER_H, RENDER_W}, bg);
    if(!g_screenshot.empty()) {
      write_bmp(g_screenshot, g_frame.data());
      g_screenshot.clear();
    }
    backend::present(g_frame.data());
  }
}

bool save_screenshot(const char *path) {
  g_screenshot = path;  // written by the next end_frame()
  return true;
}

void set_text_input(bool on) {
  g_text_input = on;
  g_typed.clear();
  g_backspace = g_enter = g_escape = false;
  g_held = ACT_NONE;
  backend::set_text_input(on);
}

bool take_typed(std::string &text, bool &backspace, bool &enter, bool &escape) {
  bool any = !g_typed.empty() || g_backspace || g_enter || g_escape;
  text = g_typed;
  backspace = g_backspace;
  enter = g_enter;
  escape = g_escape;
  g_typed.clear();
  g_backspace = g_enter = g_escape = false;
  return any;
}

bool has_touch() {
  if(!g_touch_known) poll_touch();
  return g_touch_present;
}

Touch take_touch() {
  Touch t = g_touch;
  g_touch.pressed = g_touch.released = false;
  g_touch.drag_y = 0;
  return t;
}

bool has_native_keyboard() {
  return backend::has_native_keyboard();
}

bool keyboard_failed() {
  return backend::keyboard_failed();
}

const char *about_file_manager() {
  return backend::about_file_manager();
}

int about_thanks(const char *const **lines) {
  return backend::about_thanks(lines);
}

bool native_text_prompt(const char *title, const std::string &initial, TextKind kind, std::string &out) {
  bool ok = backend::native_text_prompt(title, initial, kind, out);
  g_held = ACT_NONE;
  return ok;
}

bool can_quit() {
  return backend::can_quit();
}

void *alloc(std::size_t size) {
  return app_alloc(size);
}

void release(void *p) {
  app_free(p);
}

}  // namespace platform

namespace draw {

const ImU32 BG = IM_COL32(11, 18, 32, 255);
const ImU32 PANEL = IM_COL32(18, 26, 43, 255);
const ImU32 PANEL2 = IM_COL32(25, 35, 58, 255);
const ImU32 LINE = IM_COL32(38, 50, 77, 255);
const ImU32 TEXT = IM_COL32(232, 237, 245, 255);
const ImU32 MUTED = IM_COL32(142, 154, 179, 255);
const ImU32 ACCENT = IM_COL32(47, 140, 255, 255);
const ImU32 ACCENT2 = IM_COL32(255, 196, 64, 255);
const ImU32 OK = IM_COL32(60, 207, 145, 255);
const ImU32 ERR = IM_COL32(255, 107, 107, 255);
const ImU32 SEL = IM_COL32(36, 70, 122, 255);
const ImU32 CURSOR = IM_COL32(47, 140, 255, 70);

ImDrawList *dl() {
  return ImGui::GetBackgroundDrawList();
}

static ImFont *pick(bool bold) {
  return bold ? platform::font_bold : platform::font_regular;
}

/* The brand (assets/brand/BRAND.md). The mark: two stepped arrows, the down
   one being the up one turned 180 degrees, each split into a light and a dark
   facet. Polygons from chiral-ftp-mark.svg, whose viewBox is 20..180. */
void brand_mark(float x, float y, float s) {
  static const float arrow[][8] = {
      {138, 34, 104, 84, 138, 84, -1, -1}, {138, 34, 172, 84, 138, 84, -1, -1},
      {126, 84, 138, 84, 138, 110, 126, 110}, {138, 84, 150, 84, 150, 110, 138, 110},
      {126, 110, 138, 110, 122, 126, 110, 126}, {138, 110, 150, 110, 134, 126, 122, 126},
      {110, 126, 122, 126, 122, 164, 110, 152}, {122, 126, 134, 126, 134, 152, 122, 164},
  };
  const ImU32 light = IM_COL32(0xF2, 0xD9, 0x8A, 255), dark = IM_COL32(0xC7, 0x9F, 0x43, 255);
  float k = s / 160.0f;
  for(int turn = 0; turn < 2; turn++) {
    for(int i = 0; i < 8; i++) {
      ImVec2 pts[4];
      int n = arrow[i][6] < 0 ? 3 : 4;
      for(int j = 0; j < n; j++) {
        float px = arrow[i][j * 2], py = arrow[i][j * 2 + 1];
        if(turn) px = 200 - px, py = 200 - py;  // rotate(180 100 100)
        pts[j] = ImVec2(x + (px - 20) * k, y + (py - 20) * k);
      }
      dl()->AddConvexPolyFilled(pts, n, i % 2 ? dark : light);
    }
  }
}

/* "chiral" in white, "-ftp" in gold, IBM Plex Mono Medium, letter-spacing
   -0.02em. Returns the width. */
float brand_wordmark(float x, float y, float size) {
  const ImU32 white = IM_COL32(0xF5, 0xF3, 0xEC, 255), gold = IM_COL32(0xD9, 0xB6, 0x5E, 255);
  const char *word = "chiral-ftp";
  float cx = x;
  for(int i = 0; word[i]; i++) {
    char c[2] = {word[i], 0};
    dl()->AddText(platform::font_mono, size, ImVec2(cx, y), i < 6 ? white : gold, c);
    cx += platform::font_mono->CalcTextSizeA(size, FLT_MAX, 0, c).x - 0.02f * size;
  }
  return cx - x + 0.02f * size;
}

ImVec2 text_size(float size, const char *t, bool bold) {
  return pick(bold)->CalcTextSizeA(size, FLT_MAX, 0, t);
}

void text(float x, float y, float size, ImU32 col, const char *t, bool bold) {
  dl()->AddText(pick(bold), size, ImVec2(x, y), col, t);
}

void text_clip(float x, float y, float size, ImU32 col, const char *t, float max_w, bool bold) {
  ImFont *f = pick(bold);
  if(f->CalcTextSizeA(size, FLT_MAX, 0, t).x <= max_w) {
    dl()->AddText(f, size, ImVec2(x, y), col, t);
    return;
  }
  float ell = f->CalcTextSizeA(size, FLT_MAX, 0, "...").x;
  const char *end = t + strlen(t);
  // Shrink by UTF-8 characters until it fits with the ellipsis.
  while(end > t && f->CalcTextSizeA(size, FLT_MAX, 0, t, end).x + ell > max_w) {
    end--;
    while(end > t && ((unsigned char)*end & 0xC0) == 0x80) end--;
  }
  dl()->AddText(f, size, ImVec2(x, y), col, t, end);
  float w = f->CalcTextSizeA(size, FLT_MAX, 0, t, end).x;
  dl()->AddText(f, size, ImVec2(x + w, y), col, "...");
}

void rect(float x, float y, float w, float h, ImU32 col, float rounding) {
  dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), col, rounding);
}

void frame(float x, float y, float w, float h, ImU32 col, float rounding, float thick) {
  dl()->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), col, rounding, ImDrawFlags_None, thick);
}

float button(float x, float y, float s, Action a) {
  ImDrawList *d = dl();
  float r = s / 2;
  ImVec2 c(x + r, y + r);
  const char *label = nullptr;

  switch(a) {
#if CHIRAL_BUTTONS_PLAYSTATION
  // The four face buttons as their symbols, in their usual colours.
  case ACT_CROSS: case ACT_CIRCLE: case ACT_CIRCLE_LONG: case ACT_SQUARE: case ACT_TRIANGLE: {
    d->AddCircleFilled(c, r, IM_COL32(32, 44, 70, 255), 32);
    float k = r * 0.42f;
    if(a == ACT_CROSS) {
      ImU32 col = IM_COL32(124, 168, 255, 255);
      d->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col, 3);
      d->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), col, 3);
    } else if(a == ACT_CIRCLE || a == ACT_CIRCLE_LONG) {
      d->AddCircle(c, k * 1.05f, IM_COL32(255, 107, 129, 255), 24, 3);
    } else if(a == ACT_SQUARE) {
      d->AddRect(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), IM_COL32(241, 154, 222, 255), 0.0f,
                 ImDrawFlags_None, 3.0f);
    } else {
      d->AddTriangle(ImVec2(c.x, c.y - k * 1.1f), ImVec2(c.x + k * 1.1f, c.y + k * 0.8f),
                     ImVec2(c.x - k * 1.1f, c.y + k * 0.8f), IM_COL32(63, 224, 181, 255), 3);
    }
    return s;
  }
  case ACT_L1: label = "L1"; break;
  case ACT_R1: label = "R1"; break;
  case ACT_L2: label = "L2"; break;
  case ACT_R2: label = "R2"; break;
  case ACT_OPTIONS: label = "OPTIONS"; break;
  case ACT_TOUCHPAD: label = "TOUCHPAD"; break;
#else
  // The four face buttons, drawn where they sit on a Joy-Con: A right, B
  // bottom, X top, Y left. The actions keep their PS5 names through the
  // whole app (ACT_CROSS is confirm, ACT_CIRCLE is back); only the glyph and
  // the physical button change. backend_switch.cpp has the other half.
  case ACT_CROSS: case ACT_CIRCLE: case ACT_CIRCLE_LONG: case ACT_SQUARE: case ACT_TRIANGLE: {
    const char *letter = a == ACT_CROSS ? "A"
                         : (a == ACT_CIRCLE || a == ACT_CIRCLE_LONG) ? "B"
                         : a == ACT_SQUARE ? "Y" : "X";
    /* Held Back is still Back: same glyph, same red, so it reads as the
       same button rather than a different one. */
    ImU32 col = a == ACT_CROSS      ? IM_COL32(124, 168, 255, 255)   // confirm
                : (a == ACT_CIRCLE || a == ACT_CIRCLE_LONG)
                                    ? IM_COL32(255, 107, 129, 255)   // back
                : a == ACT_SQUARE   ? IM_COL32(241, 154, 222, 255)   // mark
                                    : IM_COL32(63, 224, 181, 255);   // transfer
    d->AddCircleFilled(c, r, IM_COL32(32, 44, 70, 255), 32);
    d->AddCircle(c, r - 1.5f, col, 32, 2.0f);
    float fs = s * 0.58f;
    ImVec2 ts = text_size(fs, letter, true);
    d->AddText(platform::font_bold, fs, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), col, letter);
    return s;
  }
  // The shoulder buttons keep their shapes; a Switch has no second pair of
  // bumpers, so L2/R2 (page up and down) are ZL and ZR.
  case ACT_L1: label = "L"; break;
  case ACT_R1: label = "R"; break;
  case ACT_L2: label = "ZL"; break;
  case ACT_R2: label = "ZR"; break;
  /* Plus opens the menus the PS5's OPTIONS did; Minus shows the transfer
     queue, which was on the touchpad. Both are drawn, not written: as
     glyphs in a pill they came out as a thin stroke in a small blob, and a
     "+" that cannot be read as a plus is worse than useless. These are
     round with thick bars, like the buttons themselves. */
  case ACT_OPTIONS: case ACT_TOUCHPAD: {
    const float r = s / 2;
    const ImVec2 mid(x + r, y + r);
    const float bar = s * 0.17f, arm = s * 0.27f;
    const ImU32 face = IM_COL32(206, 216, 232, 255), ink = IM_COL32(16, 24, 40, 255);
    d->AddCircleFilled(mid, r, face, 32);
    // horizontal bar: both buttons have one
    d->AddRectFilled(ImVec2(mid.x - arm, mid.y - bar / 2), ImVec2(mid.x + arm, mid.y + bar / 2),
                     ink, bar * 0.35f);
    // vertical bar: only Plus
    if(a == ACT_OPTIONS) {
      d->AddRectFilled(ImVec2(mid.x - bar / 2, mid.y - arm), ImVec2(mid.x + bar / 2, mid.y + arm),
                       ink, bar * 0.35f);
    }
    return s;
  }
#endif
  case ACT_UP: case ACT_DOWN: case ACT_LEFT: case ACT_RIGHT: {
    // D-pad: a small plus with the pressed arm highlighted.
    float t = s * 0.28f;
    ImU32 dim = IM_COL32(60, 74, 104, 255);
    d->AddRectFilled(ImVec2(c.x - t / 2, y + 2), ImVec2(c.x + t / 2, y + s - 2), dim, 3);
    d->AddRectFilled(ImVec2(x + 2, c.y - t / 2), ImVec2(x + s - 2, c.y + t / 2), dim, 3);
    ImU32 on = TEXT;
    if(a == ACT_UP || a == ACT_DOWN) {
      d->AddRectFilled(ImVec2(c.x - t / 2, y + 2), ImVec2(c.x + t / 2, y + s - 2), on, 3);
    } else {
      d->AddRectFilled(ImVec2(x + 2, c.y - t / 2), ImVec2(x + s - 2, c.y + t / 2), on, 3);
    }
    return s;
  }
  default: return 0;
  }
  /* Light fill, dark letter. The old pill was a dark fill on a dark panel
     with white text, which left the glyph barely visible. */
  float fs = s * 0.52f;
  ImVec2 ts = text_size(fs, label, true);
  float w = ts.x + s * 0.7f;
  const ImU32 face = IM_COL32(206, 216, 232, 255);
  d->AddRectFilled(ImVec2(x, y + s * 0.10f), ImVec2(x + w, y + s * 0.90f), face, s * 0.32f);
  d->AddText(platform::font_bold, fs, ImVec2(x + (w - ts.x) / 2, y + (s - ts.y) / 2),
             IM_COL32(16, 24, 40, 255), label);
  return w;
}

/* A left/right pair, for "switch side" -- which is what the d-pad and both
   sticks actually do. The shoulder buttons do it too, but showing R alone
   said the wrong thing. */
float hint_lr(float x, float y, const char *caption) {
  ImDrawList *d = dl();
  const float s = 38, h = s * 0.80f, top = y + (s - h) / 2;
  const float w = s * 1.30f;
  const ImU32 face = IM_COL32(206, 216, 232, 255), ink = IM_COL32(16, 24, 40, 255);

  d->AddRectFilled(ImVec2(x, top), ImVec2(x + w, top + h), face, h * 0.34f);
  const float cy = top + h / 2, k = h * 0.22f;
  // left arrow
  d->AddTriangleFilled(ImVec2(x + w * 0.34f, cy - k), ImVec2(x + w * 0.34f, cy + k),
                       ImVec2(x + w * 0.20f, cy), ink);
  // right arrow
  d->AddTriangleFilled(ImVec2(x + w * 0.66f, cy - k), ImVec2(x + w * 0.66f, cy + k),
                       ImVec2(x + w * 0.80f, cy), ink);
  text(x + w + 10, y + (s - 26) / 2 - 1, 26, MUTED, caption);
  return w + 10 + text_size(26, caption).x + 34;
}

float hint(float x, float y, Action a, const char *caption) {
  float s = 38;
  float w = button(x, y, s, a);
  text(x + w + 10, y + (s - 26) / 2 - 1, 26, MUTED, caption);
  return w + 10 + text_size(26, caption).x + 34;
}

void folder_icon(float x, float y, float s, ImU32 col) {
  ImDrawList *d = dl();
  d->AddRectFilled(ImVec2(x, y + s * 0.18f), ImVec2(x + s * 0.45f, y + s * 0.34f), col, 3);
  d->AddRectFilled(ImVec2(x, y + s * 0.28f), ImVec2(x + s, y + s * 0.86f), col, 4);
}

void file_icon(float x, float y, float s, ImU32 col) {
  ImDrawList *d = dl();
  float w = s * 0.72f;
  float fold = s * 0.24f;
  float x0 = x + (s - w) / 2;
  d->AddRectFilled(ImVec2(x0, y + s * 0.08f), ImVec2(x0 + w - fold, y + s * 0.92f), col, 2);
  d->AddRectFilled(ImVec2(x0, y + s * 0.08f + fold), ImVec2(x0 + w, y + s * 0.92f), col, 2);
  d->AddTriangleFilled(ImVec2(x0 + w - fold, y + s * 0.08f), ImVec2(x0 + w, y + s * 0.08f + fold),
                       ImVec2(x0 + w - fold, y + s * 0.08f + fold), IM_COL32(200, 210, 228, 255));
}

void spinner(float cx, float cy, float r, ImU32 col) {
  float t = (float)ImGui::GetTime() * 6.0f;
  ImDrawList *d = dl();
  d->PathClear();
  d->PathArcTo(ImVec2(cx, cy), r, t, t + 4.2f, 24);
  d->PathStroke(col, ImDrawFlags_None, 4.0f);
}

}  // namespace draw

extern "C" void chiral_imgui_check_failed(const char *expr, const char *file, int line) {
  static int reported;
  if(reported++ < 20) TRACE("ImGui check failed: %s (%s:%d)", expr, file, line);
}

/* C++ allocations: an out-of-memory stop names its size in the startup log
   (the C++ runtime's own version just calls abort). */
void *operator new(std::size_t size) {
  void *p = platform::alloc(size ? size : 1);
  if(!p) {
    TRACE("Out of memory: %zu bytes", size);
    abort();
  }
  return p;
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept { return platform::alloc(size ? size : 1); }
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept { return platform::alloc(size ? size : 1); }
void operator delete(void *p) noexcept { platform::release(p); }
void operator delete[](void *p) noexcept { platform::release(p); }
void operator delete(void *p, std::size_t) noexcept { platform::release(p); }
void operator delete[](void *p, std::size_t) noexcept { platform::release(p); }
