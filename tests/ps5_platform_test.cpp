// Chiral Test 2: the PS5 platform layer (VideoOut + software renderer, ScePad,
// system keyboard) with Dear ImGui, without the FTP engine. Press buttons to see
// them; Cross opens the keyboard.

#include <cstdio>
#include <string>

#include "backend.h"
#include "platform.h"

namespace {

const char *name(Action a) {
  switch(a) {
  case ACT_UP: return "UP"; case ACT_DOWN: return "DOWN"; case ACT_LEFT: return "LEFT";
  case ACT_RIGHT: return "RIGHT"; case ACT_CROSS: return "CROSS"; case ACT_CIRCLE: return "CIRCLE";
  case ACT_SQUARE: return "SQUARE"; case ACT_TRIANGLE: return "TRIANGLE"; case ACT_L1: return "L1";
  case ACT_R1: return "R1"; case ACT_L2: return "L2"; case ACT_R2: return "R2";
  case ACT_OPTIONS: return "OPTIONS"; case ACT_TOUCHPAD: return "TOUCHPAD";
  default: return "?";
  }
}

}  // namespace

int main() {
  if(!platform::init(false)) {
    for(;;) backend::pump(1000);
  }
  std::string last = "none yet", typed = "(press Cross)";
  int presses = 0;
  for(;;) {
    Action a = platform::poll(100);
    while(a != ACT_NONE) {
      presses++;
      last = name(a);
      if(a == ACT_CROSS) {
        std::string out;
        typed = platform::native_text_prompt("Chiral test: type something", "hello", TEXT_DEFAULT, out)
                    ? "You typed: " + out : "Keyboard closed without text (or failed)";
      }
      a = platform::poll(0);
    }
    platform::begin_frame();
    draw::rect(0, 0, 1920, 1080, draw::BG);
    draw::text(120, 120, 64, draw::TEXT, "Chiral Test 2", true);
    draw::text(120, 220, 34, draw::MUTED, "Display, controller and keyboard layer (no FTP engine)");
    char buf[200];
    snprintf(buf, sizeof(buf), "Last button: %s   (%d presses)", last.c_str(), presses);
    draw::text(120, 360, 40, draw::ACCENT2, buf);
    draw::text(120, 440, 40, draw::TEXT, typed.c_str());
    float x = 120;
    x += draw::hint(x, 560, ACT_CROSS, "Open keyboard");
    draw::hint(x, 560, ACT_CIRCLE, "Any button shows here");
    draw::text(120, 700, 28, draw::MUTED, "Close with the PS button.");
    platform::end_frame();
  }
}
