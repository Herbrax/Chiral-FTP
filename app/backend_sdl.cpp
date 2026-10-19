// Computer backend: an SDL window showing the software-rendered frame, with
// keyboard shortcuts (and an SDL game controller when one is connected).

#include "backend.h"

#include <SDL.h>

namespace backend {
namespace {

SDL_Window *g_window = nullptr;
SDL_Renderer *g_renderer = nullptr;
SDL_Texture *g_texture = nullptr;
bool g_text_input = false;

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
    if(g_text_input) {
      if(e.key.keysym.sym == SDLK_BACKSPACE) key_backspace();
      if(e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) key_enter();
      if(e.key.keysym.sym == SDLK_ESCAPE) key_escape();
    } else if(!e.key.repeat) {
      press(key_action(e.key.keysym.sym));
    }
    break;
  case SDL_KEYUP: release(key_action(e.key.keysym.sym)); break;
  case SDL_CONTROLLERDEVICEADDED: SDL_GameControllerOpen(e.cdevice.which); break;
  case SDL_CONTROLLERBUTTONDOWN: press(pad_button(e.cbutton.button)); break;
  case SDL_CONTROLLERBUTTONUP: release(pad_button(e.cbutton.button)); break;
  default: break;
  }
}

}  // namespace

bool init(bool headless) {
  if(headless) SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)) {
    SDL_Log("SDL_Init: %s", SDL_GetError());
    return false;
  }
  if(headless) return true;  // frames go to screenshots only
  g_window = SDL_CreateWindow("chiral-ftp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              1280, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if(!g_window) return false;
  g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if(!g_renderer) return false;
  SDL_RenderSetLogicalSize(g_renderer, 1920, 1080);
  g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 1920, 1080);
  return g_texture != nullptr;
}

void pump(int timeout_ms) {
  SDL_Event e;
  if(timeout_ms > 0 ? SDL_WaitEventTimeout(&e, timeout_ms) : SDL_PollEvent(&e)) {
    handle(e);
    while(SDL_PollEvent(&e)) handle(e);
  }
}

void present(const std::uint32_t *pixels) {
  if(!g_texture) return;
  SDL_UpdateTexture(g_texture, nullptr, pixels, 1920 * 4);
  SDL_RenderClear(g_renderer);
  SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
  SDL_RenderPresent(g_renderer);
}

std::uint64_t ticks_ms() {
  return SDL_GetTicks64();
}

bool has_native_keyboard() {
  return false;
}

void prepare_keyboard() {}

bool keyboard_failed() {
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

bool can_quit() {
  return true;
}

}  // namespace backend
