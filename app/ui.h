#pragma once

#include <string>

#include "platform.h"

namespace ui {

void init();
/* What the platform had to say about how the app was launched, for the
   banner on the start screen (backend::startup_notice): full storage access
   on the PS5, an application slot rather than a library applet on the
   Switch, nothing at all on a computer. line and detail are English and
   translated when drawn -- line is the platform's own wording, with {0} for
   detail and {1} for status. */
void set_startup_notice(bool ok, const std::string &line, const std::string &detail, int status);
// Background work and engine state (jobs, log, queue counters).
void update();
void handle(Action a);
void render();
bool quit_requested();
/* Ends the background worker and waits for it. Call before returning from
   main, ahead of the static destructors. */
void stop_workers();
// True while something animates (spinners, transfers), so the loop keeps drawing.
bool busy();

}  // namespace ui
