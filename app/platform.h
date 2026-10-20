// Input, frames and drawing helpers. On the Switch the backend is a libnx
// framebuffer, the pad and swkbd; on a computer it is an SDL window. Both
// draw with the same software renderer.
//
// The actions keep the PS5 names this UI was designed around. On the Switch
// they land on the buttons in the same four positions: ACT_CROSS is A,
// ACT_CIRCLE is B, ACT_SQUARE is Y, ACT_TRIANGLE is X, ACT_OPTIONS is Plus
// and ACT_TOUCHPAD is Minus (app/backend_switch.cpp).
#pragma once

#include <cstddef>

#include <functional>
#include <string>

#include "imgui.h"

enum Action {
  ACT_NONE,
  ACT_UP,
  ACT_DOWN,
  ACT_LEFT,
  ACT_RIGHT,
  ACT_CROSS,     // confirm / open   (A)
  ACT_CIRCLE,    // back             (B)
  ACT_SQUARE,    // mark             (Y)
  ACT_TRIANGLE,  // transfer         (X)
  ACT_L1,        //                  (L)
  ACT_R1,        //                  (R)
  ACT_L2,        // page up          (ZL)
  ACT_R2,        // page down        (ZR)
  ACT_OPTIONS,   // menu             (Plus)
  ACT_TOUCHPAD,  // queue            (Minus)
  ACT_CIRCLE_LONG,  // back held down: out to the start screen from anywhere
  ACT_QUIT,      // window closed (computer only)
};

enum TextKind { TEXT_DEFAULT, TEXT_URL, TEXT_NUMBER, TEXT_PASSWORD };

namespace platform {

// Memory for the app's C++ objects: big blocks from the system, not the heap.
void *alloc(std::size_t size);
void release(void *p);

bool init(bool headless);
void shutdown();

// Next queued action, ACT_NONE when there are none.
Action poll(int timeout_ms);

/* The touchscreen, in the UI's coordinate space, described as what happened
   rather than where the finger is. has_touch() is false on a device without
   one. Updated by poll(). */
struct Touch {
  bool pressed;   // went down since the last look
  bool held;      // still down
  bool released;  // came up since the last look
  float x, y;     // where it is, or was on release
  float drag_y;   // vertical travel since the last look, for scrolling
  bool moved;     // travelled far enough to be a drag rather than a tap
};
bool has_touch();
// The touch state; clears pressed/released/drag_y so each is seen once.
Touch take_touch();

void begin_frame();
void end_frame();
// Writes the next rendered frame to a BMP (used by the automated UI tests).
bool save_screenshot(const char *path);
void push_action(Action a);  // scripted input for tests
void inject_text(const std::string &text, bool enter);  // scripted typing for tests

// Asks for text with the system keyboard (swkbd; blocks until it closes).
// On a computer the app shows its own entry box instead.
bool native_text_prompt(const char *title, const std::string &initial, TextKind kind,
                        std::string &out);
bool has_native_keyboard();
// False when the last prompt couldn't open, rather than being cancelled.
bool keyboard_failed();
// The About screen's per-platform lines (see backend.h).
const char *about_file_manager();
int about_thanks(const char *const **lines);
// Text typed on a computer keyboard while a prompt is open.
bool take_typed(std::string &text, bool &backspace, bool &enter, bool &escape);
void set_text_input(bool on);
// True on the Switch: the app can close itself and return to hbmenu.
bool can_quit();

extern ImFont *font_regular;
extern ImFont *font_bold;
extern ImFont *font_mono;

}  // namespace platform

// Drawing helpers on the full-screen draw list.
namespace draw {

extern const ImU32 BG, PANEL, PANEL2, LINE, TEXT, MUTED, ACCENT, ACCENT2, OK, ERR, SEL, CURSOR;

ImDrawList *dl();
ImVec2 text_size(float size, const char *text, bool bold = false);
void text(float x, float y, float size, ImU32 col, const char *text, bool bold = false);
// Draws text clipped to max_w, ending with an ellipsis when cut.
void text_clip(float x, float y, float size, ImU32 col, const char *text, float max_w,
               bool bold = false);
void rect(float x, float y, float w, float h, ImU32 col, float rounding = 0);
void frame(float x, float y, float w, float h, ImU32 col, float rounding = 0, float thick = 2);
// A controller button glyph (A/B/X/Y or a label like "ZL").
float button(float x, float y, float size, Action a);
// Glyph followed by a caption; returns the width used.
float hint(float x, float y, Action a, const char *caption);
// The same, with a left/right arrow pair instead of a single button.
float hint_lr(float x, float y, const char *caption);
void folder_icon(float x, float y, float s, ImU32 col);
// The chiral-ftp mark in an s x s square, and the wordmark (returns its width).
void brand_mark(float x, float y, float s);
float brand_wordmark(float x, float y, float size);
void file_icon(float x, float y, float s, ImU32 col);
void spinner(float cx, float cy, float r, ImU32 col);

}  // namespace draw
