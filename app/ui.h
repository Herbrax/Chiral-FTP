#pragma once

#include <string>

#include "platform.h"

namespace ui {

void init();
// How chiral-ftp got access to the PS5's storage at startup (PS5 only):
// full=true when it sees the real filesystem; detail says what happened.
void set_storage_access(bool full, const std::string &reason, int status);
// Background work and engine state (jobs, log, queue counters).
void update();
void handle(Action a);
void render();
bool quit_requested();
// True while something animates (spinners, transfers), so the loop keeps drawing.
bool busy();

}  // namespace ui
