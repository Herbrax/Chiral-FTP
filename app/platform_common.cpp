// Shared platform code: controller actions with key repeat, Dear ImGui with
// the embedded fonts, software-rendered frames and the drawing helpers.

#include "platform.h"

#include <cerrno>
#include <cstdlib>
#include <new>
#include <fcntl.h>
#include <sys/mman.h>
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
#include "raster.h"

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

/* Japanese, Korean, Chinese and Arabic come from the console's own fonts
   (as in ProsperoEden: /preinst/common/font), merged into the app's Noto
   Sans; the language in use goes first, so characters Chinese and Japanese
   share get its forms. On a computer one font covers them all. Files are
   read once and stay in memory for the font atlas. */
struct ScriptFont {
  const char *lang;
  const char *files[3];
};
// The console's font files, best first (/preinst/common/font).
const ScriptFont kConsoleFonts[] = {
    {"ja", {"SSTJpPro-Regular.otf", "SIE-RDC-Pr5N-M-JPN.otf", "SCE-RDC-R-JPN.otf"}},
    {"ko", {"YoonGothicProSIE760.otf", "SCEPS4Yoongd-Medium.otf", nullptr}},
    {"zh", {"DFHEI5-SONY.ttf", nullptr, nullptr}},
    {"ar", {"SSTArabic-Medium.otf", "SSTArabic-Roman.otf", nullptr}},
};
// On a computer: one font with every script, unless CHIRAL_FONT_DIR points at
// a copy of the console's fonts (to test exactly what the PS5 loads).
#ifndef __PROSPERO__
const ScriptFont kComputerFonts[] = {
    {"", {"/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", nullptr}},
};
#endif



/* The app's memory. The console's C library heap has a cap of its own, far
   below the memory the app may use: with the Japanese, Korean, Chinese or
   Arabic fonts the glyph atlas grows past it (a 130 KB request failed with
   448 MB free). So blocks of 32 KB and more come straight from the system
   (mmap); small ones stay on the heap. A 16-byte header in front of each
   block says which, and its size. */
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


struct MappedFont {
  const void *data;
  size_t size;
};
std::vector<MappedFont> g_script_fonts;

/* The file mapped read-only: a 3-8 MB font stays out of the app's heap.
   Checked with the font parser first, so a file it can't read is skipped
   (its script then falls back) instead of failing inside the font atlas. */
bool map_font(const std::string &path, MappedFont &out) {
  struct stat st;
  int fd = open(path.c_str(), O_RDONLY);
  if(fd < 0) return false;
  if(fstat(fd, &st) || st.st_size < 1024 || st.st_size > 64L * 1024 * 1024) {
    close(fd);
    return false;
  }
  void *p = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if(p == MAP_FAILED) {
    TRACE("Font %s: could not map it (%s)", path.c_str(), strerror(errno));
    return false;
  }
  stbtt_fontinfo info;
  int offset = stbtt_GetFontOffsetForIndex((const unsigned char *)p, 0);
  if(offset < 0 || !stbtt_InitFont(&info, (const unsigned char *)p, offset)) {
    TRACE("Font %s: not a font the renderer can read; skipped", path.c_str());
    munmap(p, (size_t)st.st_size);
    return false;
  }
  out = {p, (size_t)st.st_size};
  TRACE("Font %s (%lld bytes)", path.c_str(), (long long)st.st_size);
  return true;
}

void load_script_fonts(const char *current_lang) {

#ifdef __PROSPERO__
  const char *dir = "/preinst/common/font";
  const ScriptFont *fonts = kConsoleFonts;
  size_t count = sizeof(kConsoleFonts) / sizeof(kConsoleFonts[0]);
#else
  const char *dir = getenv("CHIRAL_FONT_DIR");
  const ScriptFont *fonts = dir ? kConsoleFonts : kComputerFonts;
  size_t count = dir ? sizeof(kConsoleFonts) / sizeof(kConsoleFonts[0]) : sizeof(kComputerFonts) / sizeof(kComputerFonts[0]);
#endif
  std::vector<const ScriptFont *> order;
  for(size_t i = 0; i < count; i++) if(!strcmp(fonts[i].lang, current_lang)) order.push_back(&fonts[i]);
  for(size_t i = 0; i < count; i++) if(strcmp(fonts[i].lang, current_lang)) order.push_back(&fonts[i]);
  for(const ScriptFont *s : order) {
    for(const char *file : s->files) {
      if(!file) break;
      std::string path = file[0] == '/' ? std::string(file) : std::string(dir ? dir : "") + "/" + file;
      MappedFont font;
      if(map_font(path, font)) {
        g_script_fonts.push_back(font);
        break;
      }
    }
  }
}

// Merges the script fonts into the font just added.
void merge_script_fonts(ImGuiIO &io) {
  for(const MappedFont &font : g_script_fonts) {
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.FontDataOwnedByAtlas = false;  // mapped, not ImGui's to free
    // The console's fonts draw larger than Noto Sans at the same size; 26 matches them.
    io.Fonts->AddFontFromMemoryTTF(const_cast<void *>(font.data), (int)font.size, 26.0f, &cfg);
  }
}

}  // namespace

namespace {

const int W = 1920, H = 1080;
const std::uint64_t REPEAT_DELAY = 380;
const std::uint64_t REPEAT_RATE = 70;

std::deque<Action> g_queue;
Action g_held = ACT_NONE;
std::uint64_t g_next_repeat = 0;
bool g_text_input = false;
std::string g_typed;
bool g_backspace = false, g_enter = false, g_escape = false;
std::vector<std::uint32_t> g_frame;
std::string g_screenshot;
std::uint64_t g_last_frame = 0;

bool repeats(Action a) {
  return a == ACT_UP || a == ACT_DOWN || a == ACT_LEFT || a == ACT_RIGHT || a == ACT_L2 ||
         a == ACT_R2;
}

// 24-bit BMP, bottom-up rows, so tests need no image library.
bool write_bmp(const std::string &path, const std::uint32_t *px) {
  FILE *f = fopen(path.c_str(), "wb");
  if(!f) return false;
  const int row = W * 3;
  const unsigned size = 54 + row * H;
  unsigned char h[54] = {'B', 'M'};
  auto put32 = [&](int o, unsigned v) { for(int i = 0; i < 4; i++) h[o + i] = (unsigned char)(v >> (8 * i)); };
  put32(2, size);
  put32(10, 54);
  put32(14, 40);
  put32(18, W);
  put32(22, H);
  h[26] = 1;
  h[28] = 24;
  put32(34, row * H);
  fwrite(h, 1, 54, f);
  std::vector<unsigned char> line(row);
  for(int y = H - 1; y >= 0; y--) {
    for(int x = 0; x < W; x++) {
      std::uint32_t c = px[(size_t)y * W + x];
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
  g_queue.push_back(a);
  if(repeats(a)) {
    g_held = a;
    g_next_repeat = backend::ticks_ms() + REPEAT_DELAY;
  }
}

void release(Action a) {
  if(g_held == a) g_held = ACT_NONE;
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
  g_frame.assign((size_t)W * H, 0);
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
  ImGui::DestroyContext();
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
    backend::pump(wait);
    if(g_held != ACT_NONE && backend::ticks_ms() >= g_next_repeat) {
      g_queue.push_back(g_held);
      g_next_repeat = backend::ticks_ms() + REPEAT_RATE;
    }
  }
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
  raster::render(data, raster::Target{g_frame.data(), W, H}, IM_COL32(11, 18, 32, 255));
  if(!g_screenshot.empty()) {
    write_bmp(g_screenshot, g_frame.data());
    g_screenshot.clear();
  }
  backend::present(g_frame.data());
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

bool has_native_keyboard() {
  return backend::has_native_keyboard();
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
  case ACT_CROSS: case ACT_CIRCLE: case ACT_SQUARE: case ACT_TRIANGLE: {
    d->AddCircleFilled(c, r, IM_COL32(32, 44, 70, 255), 32);
    float k = r * 0.42f;
    if(a == ACT_CROSS) {
      ImU32 col = IM_COL32(124, 168, 255, 255);
      d->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col, 3);
      d->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), col, 3);
    } else if(a == ACT_CIRCLE) {
      d->AddCircle(c, k * 1.05f, IM_COL32(255, 107, 129, 255), 24, 3);
    } else if(a == ACT_SQUARE) {
      d->AddRect(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), IM_COL32(241, 154, 222, 255), 0.0f, ImDrawFlags_None, 3.0f);
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
  float fs = s * 0.5f;
  ImVec2 ts = text_size(fs, label, true);
  float w = ts.x + s * 0.6f;
  d->AddRectFilled(ImVec2(x, y + s * 0.12f), ImVec2(x + w, y + s * 0.88f), IM_COL32(32, 44, 70, 255), s * 0.3f);
  d->AddText(platform::font_bold, fs, ImVec2(x + (w - ts.x) / 2, y + (s - ts.y) / 2), TEXT, label);
  return w;
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
