// What platform_common.cpp needs from a device backend: the PS5 (VideoOut,
// ScePad, system keyboard) or a computer (SDL window and keyboard).
#pragma once

#include <cstdint>
#include <string>

#include "platform.h"

namespace backend {

bool init(bool headless);
// Waits up to timeout_ms for input and reports it through platform::detail.
void pump(int timeout_ms);
// Shows a finished 1920x1080 frame (RGBA, IM_COL32 order, linear rows).
void present(const std::uint32_t *pixels);
std::uint64_t ticks_ms();

bool has_native_keyboard();
bool native_text_prompt(const char *title, const std::string &initial, TextKind kind,
                        std::string &out);
void set_text_input(bool on);
// The PS5 can't quit an app from inside (the PS button closes it).
bool can_quit();

}  // namespace backend

namespace platform {
namespace detail {

void press(Action a);
void release(Action a);
void typed(const char *text);
void key_backspace();
void key_enter();
void key_escape();
void quit();

}  // namespace detail
}  // namespace platform
