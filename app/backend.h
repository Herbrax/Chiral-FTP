// What platform_common.cpp needs from whatever the app is running on. One
// implementation per platform, in platform/<name>/backend.cpp:
//   ps5     VideoOut, ScePad, the system keyboard (sceImeDialog)
//   switch  a libnx framebuffer, the pad, swkbd
//   sdl     a window on a computer, with its keyboard and a game controller
// engine/system.h is the same idea for storage, the network and the clock.
#pragma once

#include <cstdint>
#include <string>

#include "platform.h"

namespace backend {

bool init(bool headless);
/* Gives back whatever init() took. Everything the graphics driver handed
   over has to be released before the process exits: a .nro that returns
   while it still owns framebuffers and a window does not get a clean exit,
   and on the Switch that can take the compositor with it. */
void shutdown();
// Waits up to timeout_ms for input and reports it through platform::detail.
void pump(int timeout_ms);
// Shows a finished frame (RGBA, IM_COL32 order, linear rows), at the pixel
// size window_size() last gave, or CHIRAL_RENDER_W x CHIRAL_RENDER_H on a
// console. On the Switch the compositor scales it to whatever the display is.
void present(const std::uint32_t *pixels);
/* A window on a computer: its size in points, and the pixels that covers
   (more than the points on a Retina display). The UI then lays itself out
   in the window as it is -- a bigger window shows more, not a bigger
   picture -- and draws every frame at exactly those pixels
   (platform_common.cpp). Just the facts: the layout's scale and zoom are
   decided in shared code. False on a console, whose UI is a fixed 1920x1080
   drawn at CHIRAL_RENDER_W x CHIRAL_RENDER_H. */
bool window_size(int *pt_w, int *pt_h, int *px_w, int *px_h);
/* The pointing hand while the mouse is over something clickable, the arrow
   otherwise. Nothing to do where there is no mouse. */
void pointer_hand(bool on);
/* Opens url in the system's web browser -- the web app, on a computer.
   False where there is no browser to hand it to (a console). */
bool open_url(const char *url);
/* Hands a file on this device to the system's other apps: its default app
   for that kind of file (OPEN_VIEW), the system's "open with" chooser
   (OPEN_CHOOSE), or its share sheet (OPEN_SHARE). False when nothing here
   can take it, or where there is no such thing (everywhere but Android). */
bool open_file(const char *path, int how);
/* Puts the app behind the home screen without ending it, as Back does on
   an Android app's first screen; its servers keep running. False where
   there is no such thing (everywhere but Android). */
bool to_background();
/* True while the app is what the person is looking at: in focus on a
   console, a focused and visible window on a computer. Quick connect only
   runs then. Where the platform can't tell, true. */
bool in_foreground();
/* Puts text on the system clipboard; false where there is none. */
bool copy_text(const char *text);
/* The system's own "choose a folder" panel, starting in start. True with
   the folder in out; false when cancelled or when the platform has no such
   panel (then the UI asks for a path instead). */
bool choose_folder(const char *start, std::string &out);

// The backend's own frame storage, for the renderer to draw straight into,
// saving a full-frame copy on the way to the screen. Returns null when the
// backend hasn't got any -- then draw into your own buffer and call
// present(). stride_pixels is the distance between rows.
std::uint32_t *frame_begin(int *stride_pixels);
void frame_end();
std::uint64_t ticks_ms();

// One finger, in the backend's own pixel space. False when the device has
// no touchscreen (then down is meaningless). Only the first touch point is
// reported: the UI has nothing that wants two.
bool touch(int *x, int *y, bool *down);

/* One line for the start screen about how the app was launched, where that
   is worth saying: on the PS5 whether the jailbreak granted it full storage
   access, on the Switch whether hbmenu gave it a full application slot or a
   library applet. Returns false when there is nothing to report (a computer).

   line is the banner itself, in the platform's own words: English, marked
   with N_(), with {0} standing for detail and {1} for status. detail is the
   reason (also N_(), translated on its own) and status a raw platform code,
   both for the log too. ok=false draws the line as a warning rather than a
   confirmation. */
bool startup_notice(bool *ok, const char **line, const char **detail, int *status);

/* Services the platform brought up before main() ran, reported now that
   there is somewhere to write them. It had no log that early, so whatever
   it recorded comes out here. trace takes a printf-style line. */
void report_startup_services(void (*trace)(const char *fmt, ...));

bool has_native_keyboard();
/* Whatever has to happen before the app touches storage, called once from
   main() on a single thread.

   On the PS5 that is two things in a fixed order: load the system
   keyboard's module (after the access request the title may no longer load
   it, and every text field would silently do nothing), then ask the
   jailbreak for access to the console's real filesystem -- until that
   succeeds /data looks empty. The result is what startup_notice() then
   reports. Nothing to do on the Switch, where the services come up in
   userAppInit, or on a computer. */
void early_init();
/* True when the last native_text_prompt couldn't open at all, as opposed
   to the user cancelling it -- worth telling them apart. */
bool keyboard_failed();
bool native_text_prompt(const char *title, const std::string &initial, TextKind kind,
                        std::string &out);
void set_text_input(bool on);
/* What the About screen says differently on each platform: what the file
   manager reaches (English, marked with N_() where it is written, so the UI
   translates it), and the projects to thank by name, one screen line each
   and not translated. */
const char *about_file_manager();
int about_thanks(const char *const **lines);
// True on the Switch: returning from main() goes back to hbmenu.
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
/* The mouse, in window points (platform_common turns them into the UI's
   units). Only a computer has one; the consoles never call these. */
void pointer_move(float x, float y);
void pointer_button(bool right, bool down, bool modifier);
void pointer_wheel(float notches);  // positive scrolls up
void pointer_leave();
// A menu-bar command (ACT_CMD_*): queued even while a text entry is open.
void command(Action a);
/* A TV (CHIRAL_BUTTONS_REMOTE): whether the last button came from a
   gamepad or the remote, for which glyphs the bottom bar draws. */
void input_from_pad(bool pad);

}  // namespace detail
}  // namespace platform
