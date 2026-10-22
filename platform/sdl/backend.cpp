// Computer backend: an SDL window showing the software-rendered frame, with
// keyboard shortcuts, the mouse (and an SDL game controller when one is
// connected), and on macOS the menu bar and folder chooser (macos.mm), on
// Windows the folder chooser (win32/windows.cpp).

#include "backend.h"
#include "i18n.h"

#include <SDL.h>
#include <cmath>
#include <cstdio>

#ifdef __APPLE__
void macos_menu_install();  // macos.mm
bool macos_choose_folder(const char *start, std::string &out);
#endif
#ifdef _WIN32
bool windows_choose_folder(const char *start, std::string &out);  // win32/windows.cpp
#endif

namespace backend {
namespace {

SDL_Window *g_window = nullptr;
SDL_Renderer *g_renderer = nullptr;
SDL_Texture *g_texture = nullptr;
bool g_text_input = false;
bool g_headless = false;

/* The frame's size in pixels, as window_size() last handed it out: what
   present() is given. */
int g_frame_w = 1920, g_frame_h = 1080;
int g_tex_w = 0, g_tex_h = 0;
SDL_SystemCursor g_cursor_shape = SDL_SYSTEM_CURSOR_ARROW;
SDL_Cursor *g_cursors[2] = {nullptr, nullptr};

/* Cmd on a Mac, Ctrl elsewhere: the key that adds to a selection, and that
   the menu bar's shortcuts go through rather than the app's own keys. */
bool command_key(Uint16 mod) {
#ifdef __APPLE__
  return mod & KMOD_GUI;
#else
  return mod & KMOD_CTRL;
#endif
}

Action key_action(SDL_Keycode k) {
  switch(k) {
  case SDLK_UP: return ACT_UP;
  case SDLK_DOWN: return ACT_DOWN;
  case SDLK_LEFT: return ACT_LEFT;
  case SDLK_RIGHT: return ACT_RIGHT;
  case SDLK_RETURN: case SDLK_KP_ENTER: return ACT_CROSS;
  case SDLK_ESCAPE: case SDLK_BACKSPACE: return ACT_CIRCLE;
  case SDLK_SPACE: return ACT_SQUARE;
  case SDLK_t: return ACT_TRIANGLE;
  case SDLK_q: return ACT_L1;
  case SDLK_e: case SDLK_TAB: return ACT_R1;
  case SDLK_PAGEUP: return ACT_L2;
  case SDLK_PAGEDOWN: return ACT_R2;
  case SDLK_m: case SDLK_F10: return ACT_OPTIONS;
  case SDLK_g: return ACT_TOUCHPAD;
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
  case SDL_CONTROLLER_BUTTON_TOUCHPAD: case SDL_CONTROLLER_BUTTON_BACK: return ACT_TOUCHPAD;
  default: return ACT_NONE;
  }
}

void handle(const SDL_Event &e) {
  using namespace platform::detail;
  switch(e.type) {
  case SDL_QUIT: quit(); break;
  case SDL_TEXTINPUT: typed(e.text.text); break;
  case SDL_KEYDOWN:
    /* Cmd-T is the menu's Transfers, not T; Cmd-Q quits rather than
       switching tabs. On macOS the menu bar takes these (macos.mm). */
    if(command_key(e.key.keysym.mod)) {
#ifndef __APPLE__
      if(e.key.keysym.sym == SDLK_v && g_text_input) {
        if(char *clip = SDL_GetClipboardText()) {
          typed(clip);
          SDL_free(clip);
        }
      }
#endif
      break;
    }
    if(g_text_input) {
      if(e.key.keysym.sym == SDLK_BACKSPACE) key_backspace();
      if(e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) key_enter();
      if(e.key.keysym.sym == SDLK_ESCAPE) key_escape();
    } else if(!e.key.repeat) {
      press(key_action(e.key.keysym.sym));
    }
    break;
  case SDL_KEYUP: release(key_action(e.key.keysym.sym)); break;
  case SDL_MOUSEMOTION: pointer_move((float)e.motion.x, (float)e.motion.y); break;
  case SDL_MOUSEBUTTONDOWN:
  case SDL_MOUSEBUTTONUP: {
    if(e.button.button != SDL_BUTTON_LEFT && e.button.button != SDL_BUTTON_RIGHT) break;
    pointer_move((float)e.button.x, (float)e.button.y);
    /* A Ctrl-click on a Mac is the right click of a one-button mouse. */
    bool right = e.button.button == SDL_BUTTON_RIGHT;
#ifdef __APPLE__
    right = right || (SDL_GetModState() & KMOD_CTRL);
#endif
    pointer_button(right, e.type == SDL_MOUSEBUTTONDOWN, command_key(SDL_GetModState()));
    break;
  }
  case SDL_MOUSEWHEEL: {
    // Already the way the user scrolls (natural or not): SDL's FLIPPED only says so.
    pointer_wheel(e.wheel.preciseY);
    break;
  }
  case SDL_WINDOWEVENT:
    if(e.window.event == SDL_WINDOWEVENT_LEAVE) pointer_leave();
    break;
  case SDL_CONTROLLERDEVICEADDED: SDL_GameControllerOpen(e.cdevice.which); break;
  case SDL_CONTROLLERBUTTONDOWN: press(pad_button(e.cbutton.button)); break;
  case SDL_CONTROLLERBUTTONUP: release(pad_button(e.cbutton.button)); break;
  default: break;
  }
}

}  // namespace

bool init(bool headless) {
  g_headless = headless;
  if(headless) SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
#ifdef _WIN32
  /* Sharp on a high-DPI screen, as on a Mac: the window measured in points
     and drawn in pixels, rather than drawn small and stretched by Windows. */
  SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
  SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
  /* main() is the program's own, not SDL2main's (SDL_MAIN_HANDLED in
     platform.mk); SDL_Init refuses to start until it is told so. */
  SDL_SetMainReady();
#endif
  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)) {
    SDL_Log("SDL_Init: %s", SDL_GetError());
    return false;
  }
  if(headless) return true;  // frames go to screenshots only
  g_window = SDL_CreateWindow("chiral-ftp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              1280, 800, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if(!g_window) return false;
  /* Below this the layout runs out of room and shrinks to fit
     (platform_common.cpp); smaller than that is no use to anyone. */
  SDL_SetWindowMinimumSize(g_window, 720, 450);
  g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if(!g_renderer) return false;
  g_cursors[0] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
  g_cursors[1] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
#ifdef __APPLE__
  macos_menu_install();
#endif
  return true;
}

void shutdown() {
  for(SDL_Cursor *&c : g_cursors) {
    if(c) SDL_FreeCursor(c);
    c = nullptr;
  }
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
  if(timeout_ms > 0 ? SDL_WaitEventTimeout(&e, timeout_ms) : SDL_PollEvent(&e)) {
    handle(e);
    while(SDL_PollEvent(&e)) handle(e);
  }
}

std::uint32_t *frame_begin(int *stride_pixels) {
  // SDL wants a finished frame handed to a texture; nothing to draw into.
  (void)stride_pixels;
  return nullptr;
}

void frame_end() {}

bool window_size(int *pt_w, int *pt_h, int *px_w, int *px_h) {
  if(g_headless) {
    /* Screenshots are the consoles' fixed 1920x1080, unless the automated
       UI tests ask for a window: CHIRAL_TEST_WINDOW=1280x800 or, for a
       Retina one, 1280x800@2. */
    const char *spec = SDL_getenv("CHIRAL_TEST_WINDOW");
    int w = 0, h = 0;
    float k = 1.0f;
    if(!spec || sscanf(spec, "%dx%d@%f", &w, &h, &k) < 2 || w <= 0 || h <= 0 || k <= 0) return false;
    *pt_w = w;
    *pt_h = h;
    *px_w = g_frame_w = (int)std::lround(w * k);
    *px_h = g_frame_h = (int)std::lround(h * k);
    return true;
  }
  if(!g_window || !g_renderer) return false;
  SDL_GetWindowSize(g_window, pt_w, pt_h);
  if(SDL_GetRendererOutputSize(g_renderer, px_w, px_h)) return false;
  g_frame_w = *px_w;
  g_frame_h = *px_h;
  return true;
}

void present(const std::uint32_t *pixels) {
  if(!g_renderer) return;
  if(!g_texture || g_tex_w != g_frame_w || g_tex_h != g_frame_h) {
    if(g_texture) SDL_DestroyTexture(g_texture);
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                  g_frame_w, g_frame_h);
    if(!g_texture) return;
    g_tex_w = g_frame_w;
    g_tex_h = g_frame_h;
  }
  SDL_UpdateTexture(g_texture, nullptr, pixels, g_frame_w * 4);
  // The frame is the window's own pixels: copied one to one, never scaled.
  SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
  SDL_RenderPresent(g_renderer);
}

bool in_foreground() {
  if(g_headless || !g_window) return true;  // the automated UI tests
  const Uint32 flags = SDL_GetWindowFlags(g_window);
  return (flags & SDL_WINDOW_INPUT_FOCUS) && !(flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN));
}

bool copy_text(const char *text) {
  return SDL_SetClipboardText(text) == 0;
}

bool choose_folder(const char *start, std::string &out) {
#ifdef __APPLE__
  if(!g_headless) return macos_choose_folder(start, out);
#endif
#ifdef _WIN32
  if(!g_headless) return windows_choose_folder(start, out);
#endif
  (void)start;
  (void)out;
  return false;  // no folder panel here: the UI asks for a path
}

bool open_url(const char *url) {
  if(g_headless) return false;  // the automated UI tests open nothing on the desktop
  return SDL_OpenURL(url) == 0;
}

void pointer_hand(bool on) {
  if(!g_cursors[on ? 1 : 0]) return;
  const SDL_SystemCursor want = on ? SDL_SYSTEM_CURSOR_HAND : SDL_SYSTEM_CURSOR_ARROW;
  if(want == g_cursor_shape) return;
  g_cursor_shape = want;
  SDL_SetCursor(g_cursors[on ? 1 : 0]);
}

std::uint64_t ticks_ms() {
  return SDL_GetTicks64();
}

bool touch(int *x, int *y, bool *down) {
  // A window has a mouse, not a finger; not worth pretending otherwise.
  (void)x;
  (void)y;
  (void)down;
  return false;
}

bool startup_notice(bool *ok, const char **line, const char **detail, int *status) {
  /* A window on a computer has nothing to explain about how it started. */
  (void)ok;
  (void)line;
  (void)detail;
  (void)status;
  return false;
}

void report_startup_services(void (*trace)(const char *fmt, ...)) {
  (void)trace;  /* no services to bring up */
}

void early_init() {}

bool keyboard_failed() {
  return false;  // there is no native prompt to fail
}

bool has_native_keyboard() {
  return false;
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
  return N_("  File manager: copy, move and delete files on this computer.");
}

int about_thanks(const char *const **lines) {
  static const char *const kThanks[] = {
      "websrv and ftpsrv (John T\xc3\xb6rnblom), SDL2, libssh2, libssh, Mbed TLS, GNU libmicrohttpd, Dear ImGui,",
      "FreeType (portions \xc2\xa9 The FreeType Project, freetype.org), Noto Sans, IBM Plex Mono.",
  };
  *lines = kThanks;
  return (int)(sizeof(kThanks) / sizeof(kThanks[0]));
}

bool can_quit() {
  return true;
}

}  // namespace backend
