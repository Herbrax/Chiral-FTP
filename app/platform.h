// Input, frames and drawing helpers. On the PS5 the backend is VideoOut, ScePad
// and the system keyboard; on a computer it is an SDL window. Both draw with
// the same software renderer.
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
  ACT_CROSS,     // confirm / open
  ACT_CIRCLE,    // back
  ACT_SQUARE,    // mark
  ACT_TRIANGLE,  // transfer
  ACT_L1,
  ACT_R1,
  ACT_L2,        // page up
  ACT_R2,        // page down
  ACT_OPTIONS,   // menu
  ACT_TOUCHPAD,  // queue
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

void begin_frame();
void end_frame();
// Writes the next rendered frame to a BMP (used by the automated UI tests).
bool save_screenshot(const char *path);
void push_action(Action a);  // scripted input for tests
void inject_text(const std::string &text, bool enter);  // scripted typing for tests

// Asks for text with the system keyboard on the PS5 (blocks until it closes).
// On a computer the app shows its own entry box instead.
bool native_text_prompt(const char *title, const std::string &initial, TextKind kind,
                        std::string &out);
bool has_native_keyboard();
bool keyboard_failed();
// Text typed on a computer keyboard while a prompt is open.
bool take_typed(std::string &text, bool &backspace, bool &enter, bool &escape);
void set_text_input(bool on);
// False on the PS5: apps are closed with the PS button, not from inside.
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
// A PlayStation button glyph (cross/circle/square/triangle or a label like "L1").
float button(float x, float y, float size, Action a);
// Glyph followed by a caption; returns the width used.
float hint(float x, float y, Action a, const char *caption);
void folder_icon(float x, float y, float s, ImU32 col);
// The chiral-ftp mark in an s x s square, and the wordmark (returns its width).
void brand_mark(float x, float y, float s);
float brand_wordmark(float x, float y, float size);
void file_icon(float x, float y, float s, ImU32 col);
void spinner(float cx, float cy, float r, ImU32 col);

}  // namespace draw
