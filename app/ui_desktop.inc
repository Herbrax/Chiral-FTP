/* The computer interface (CHIRAL_UI_DESKTOP: macOS, Windows and Linux), part of
   ui.cpp's private half -- included there, after everything it uses.

   A computer is not a TV across the room. Instead of a start screen of
   cards and screens of "<  value  >" rows read with a d-pad, a window has a
   sidebar of pages and real controls: switches, segmented choices, text
   fields typed into where they are, buttons. It is the same app underneath:
   these pages drive the same servers, settings and stores the console
   layout does, and the file manager and the client, when Settings puts
   them in the window, are the console layout's own screens drawn beside
   the sidebar.

   Immediate mode, like Dear ImGui underneath: each control is drawn and
   asked "were you clicked?" in the same call, from the mouse state
   handle_pointer() leaves in g_mouse. The keyboard still works -- Up and
   Down move a focus ring through the controls, Enter presses, Left and
   Right change a choice, Q and E change page -- but the mouse comes first.

   Sizes are written in the mockup's CSS pixels: P() turns one into layout
   units (1.5 a point, platform_common.cpp), F() a CSS font size into
   ImGui's line-height size. */

namespace desk {

inline float P(float css_px) { return css_px * 1.5f; }
inline float F(float css_px) { return css_px * 2.0f; }

/* The brand's dark palette (assets/brand/BRAND.md), and greys between. */
const ImU32 BG = IM_COL32(0x0E, 0x0F, 0x11, 255);
const ImU32 SIDE = IM_COL32(0x13, 0x15, 0x18, 255);
const ImU32 CARD = IM_COL32(0x18, 0x1B, 0x20, 255);
const ImU32 LINE = IM_COL32(0x23, 0x26, 0x2C, 255);
const ImU32 RAISED = IM_COL32(0x1E, 0x22, 0x28, 255);
const ImU32 HOVER = IM_COL32(0x1C, 0x1F, 0x24, 255);
const ImU32 BORDER = IM_COL32(0x34, 0x39, 0x43, 255);
const ImU32 SEG_ON = IM_COL32(0x2C, 0x30, 0x38, 255);
const ImU32 TEXT = IM_COL32(0xF5, 0xF3, 0xEC, 255);
const ImU32 SOFT = IM_COL32(0xC9, 0xCE, 0xD6, 255);
const ImU32 MUTED = IM_COL32(0xA3, 0xA9, 0xB4, 255);
const ImU32 FAINT = IM_COL32(0x7E, 0x85, 0x91, 255);
const ImU32 GOLD = IM_COL32(0xD9, 0xB6, 0x5E, 255);
const ImU32 GOLD_LIGHT = IM_COL32(0xF2, 0xD9, 0x8A, 255);
const ImU32 GOLD_TINT = IM_COL32(0x2A, 0x24, 0x16, 255);
const ImU32 GREEN = IM_COL32(0x3C, 0xCF, 0x91, 255);
const ImU32 RED = IM_COL32(0xFF, 0x7A, 0x7A, 255);
const ImU32 INK = IM_COL32(0x11, 0x11, 0x11, 255);

const float SIDE_W = 232 * 1.5f;

enum Page { PAGE_SERVER, PAGE_WEB, PAGE_QUICK, PAGE_LOG, PAGE_SETTINGS, PAGE_COUNT };
Page g_page = PAGE_SERVER;

/* The mouse as the controls see it this frame (handle_pointer fills it). */
struct Mouse {
  float x = -1, y = -1;
  bool inside = false;
  bool click = false, right = false;
};
Mouse g_mouse;
bool g_click_taken = false;
float g_wheel = 0;
bool g_hand = false;  // something under the pointer is clickable

/* The keyboard: a focus ring over the controls in drawing order. */
bool g_keys = false;    // the keyboard was used last: show the ring
int g_focus = -1;
int g_focus_count = 0;  // controls this frame
int g_focus_last = 0;   // and last frame, for wrapping
Action g_key = ACT_NONE;

/* The text field being typed into, if any. It starts with all its text
   selected, as a field you tab or click into does: typing replaces it,
   Backspace clears it. */
std::string g_edit_key, g_edit_text;
bool g_edit_fresh = false;
bool g_edit_seen = false;

float g_scroll[PAGE_COUNT];
float g_content_h = 0;

/* ---------- drawing ---------- */

float px() { return platform::pixel(); }

void rect(float x, float y, float w, float h, ImU32 col, float r = 0) {
  draw::rect(x, y, w, h, col, r);
}

/* A panel with a one-pixel edge: two fills, so the edge is exactly a pixel
   at any window size rather than an anti-aliased line that fades. */
void card(float x, float y, float w, float h, ImU32 fill = CARD, ImU32 edge = LINE, float r = P(14)) {
  const float p = px();
  rect(x, y, w, h, edge, r);
  rect(x + p, y + p, w - 2 * p, h - 2 * p, fill, r - p);
}

void hline(float x, float y, float w, ImU32 col = LINE) {
  rect(x, y, w, px(), col);
}

void text(float x, float y, float size, ImU32 col, const char *t, bool bold = false) {
  draw::text(x, y, size, col, t, bold);
}

float text_w(float size, const char *t, bool bold = false) {
  return draw::text_size(size, t, bold).x;
}

void mono(float x, float y, float size, ImU32 col, const char *t) {
  draw::dl()->AddText(platform::font_mono, size, ImVec2(x, y), col, t);
}

float mono_w(float size, const char *t) {
  return platform::font_mono->CalcTextSizeA(size, FLT_MAX, 0, t).x;
}

/* Words that must fit in w: the start, with an ellipsis at the end. */
std::string fit_end(const std::string &s, float size, float w) {
  if(text_w(size, s.c_str()) <= w) return s;
  std::string t = s;
  while(!t.empty() && text_w(size, (t + "…").c_str()) > w) {
    t.pop_back();
    while(!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();
  }
  return t + "…";
}

/* Text that must fit in w: the tail, for a path, with an ellipsis in front. */
std::string fit(const std::string &s, float size, float w, bool is_mono) {
  auto width = [&](const std::string &t) { return is_mono ? mono_w(size, t.c_str()) : text_w(size, t.c_str()); };
  if(width(s) <= w) return s;
  std::string t = s;
  while(!t.empty() && width("…" + t) > w) {
    t.erase(0, 1);
    while(!t.empty() && ((unsigned char)t[0] & 0xC0) == 0x80) t.erase(0, 1);
  }
  return "…" + t;
}

void dot(float cx, float cy, float r, ImU32 col) {
  draw::dl()->AddCircleFilled(ImVec2(cx, cy), r, col, 16);
}

/* Line icons, in a box of side s at x, y. */
enum Icon { IC_SERVER, IC_WINDOW, IC_FOLDER, IC_GEAR, IC_LINES, IC_COPY, IC_OPEN, IC_KEY, IC_WARN, IC_TRANSFER, IC_DOWN, IC_QUICK };

void icon(Icon which, float x, float y, float s, ImU32 col) {
  ImDrawList *d = draw::dl();
  const float t = std::max(P(1.8f), 2 * px());
  auto X = [&](float u) { return x + u * s / 24; };
  auto Y = [&](float u) { return y + u * s / 24; };
  auto line = [&](float a, float b, float c, float e) { d->AddLine(ImVec2(X(a), Y(b)), ImVec2(X(c), Y(e)), col, t); };
  auto box = [&](float a, float b, float c, float e, float r) {
    d->AddRect(ImVec2(X(a), Y(b)), ImVec2(X(c), Y(e)), col, r * s / 24, ImDrawFlags_None, t);
  };
  switch(which) {
  case IC_SERVER:
    box(3, 4, 21, 10, 1.5f);
    box(3, 14, 21, 20, 1.5f);
    dot(X(7), Y(7), t * 0.7f, col);
    dot(X(7), Y(17), t * 0.7f, col);
    break;
  case IC_WINDOW:
    box(3, 4, 21, 20, 2);
    line(3, 9, 21, 9);
    break;
  case IC_FOLDER:
    draw::folder_icon(x + s * 0.08f, y + s * 0.05f, s * 0.84f, col);
    break;
  case IC_GEAR:
    d->AddCircle(ImVec2(X(12), Y(12)), s * 3.2f / 24, col, 20, t);
    for(int k = 0; k < 8; k++) {
      const float a = (float)k * 3.14159265f / 4;
      line(12 + 6 * cosf(a), 12 + 6 * sinf(a), 12 + 9 * cosf(a), 12 + 9 * sinf(a));
    }
    break;
  case IC_LINES:
    line(4, 6, 20, 6);
    line(4, 12, 20, 12);
    line(4, 18, 14, 18);
    break;
  case IC_COPY:
    box(9, 9, 20, 20, 2);
    line(5, 15, 5, 5);
    line(5, 5, 15, 5);
    break;
  case IC_OPEN:
    line(14, 4, 20, 4);
    line(20, 4, 20, 10);
    line(20, 4, 11, 13);
    line(18, 14, 18, 20);
    line(18, 20, 4, 20);
    line(4, 20, 4, 6);
    line(4, 6, 10, 6);
    break;
  case IC_KEY:
    d->AddCircle(ImVec2(X(8), Y(15)), s * 4 / 24, col, 20, t);
    line(11, 12, 20, 3);
    line(16, 7, 19, 10);
    break;
  case IC_WARN:
    line(12, 3, 21, 19);
    line(21, 19, 3, 19);
    line(3, 19, 12, 3);
    line(12, 9, 12, 13);
    dot(X(12), Y(16), t * 0.7f, col);
    break;
  case IC_TRANSFER:
    draw::brand_mark(x, y, s);
    break;
  case IC_DOWN:  // a chevron, for a menu
    line(6, 9, 12, 15);
    line(12, 15, 18, 9);
    break;
  case IC_QUICK:  // a dot sending out two arcs
    dot(X(6), Y(18), t, col);
    for(int k = 1; k <= 2; k++) {
      d->PathArcTo(ImVec2(X(6), Y(18)), s * (5.5f * k) / 24, -1.5708f, 0, 10);
      d->PathStroke(col, ImDrawFlags_None, t);
    }
    break;
  }
}

/* ---------- controls ---------- */

bool over(float x, float y, float w, float h) {
  return g_mouse.inside && g_mouse.x >= x && g_mouse.x < x + w && g_mouse.y >= y && g_mouse.y < y + h;
}

struct Ctl {
  bool hot, focus, hit;
};

/* One focusable, clickable control: the click (or Enter, when it has the
   keyboard's focus) is taken once. */
Ctl control(float x, float y, float w, float h, bool enabled = true) {
  const int idx = g_focus_count++;
  Ctl c{false, false, false};
  c.hot = enabled && over(x, y, w, h);
  c.focus = g_keys && idx == g_focus;
  if(c.hot) g_hand = true;
  if(c.hot && g_mouse.click && !g_click_taken) {
    c.hit = true;
    g_click_taken = true;
    g_focus = idx;
  }
  if(enabled && idx == g_focus && g_key == ACT_CROSS) {
    c.hit = true;
    g_key = ACT_NONE;
  }
  return c;
}

void focus_ring(float x, float y, float w, float h, float r) {
  draw::frame(x - P(3), y - P(3), w + P(6), h + P(6), GOLD, r + P(3), P(2));
}

enum Kind { PRIMARY, SECONDARY, QUIET, DANGER };

float button_w(const char *label, bool with_icon) {
  return text_w(F(14), label, true) + P(with_icon ? 52 : 34);
}

bool button(float x, float y, const char *label, Kind kind = SECONDARY, int ic = -1, bool enabled = true,
            float h = P(40), float w = 0) {
  if(w <= 0) w = button_w(label, ic >= 0);
  const Ctl c = control(x, y, w, h, enabled);
  const float r = P(10);
  ImU32 fill = kind == PRIMARY ? GOLD : kind == QUIET ? (c.hot ? HOVER : SIDE) : RAISED;
  ImU32 ink = kind == PRIMARY ? INK : kind == DANGER ? RED : TEXT;
  if(kind == PRIMARY && c.hot) fill = GOLD_LIGHT;
  if(!enabled) {
    fill = kind == PRIMARY ? IM_COL32(0x6B, 0x5C, 0x34, 255) : RAISED;
    ink = kind == PRIMARY ? IM_COL32(0x2A, 0x26, 0x1A, 255) : FAINT;
  }
  if(kind == SECONDARY || kind == DANGER) card(x, y, w, h, c.hot ? SEG_ON : RAISED, BORDER, r);
  else rect(x, y, w, h, fill, r);
  float tx = x + (w - text_w(F(14), label, true) - (ic >= 0 ? P(24) : 0)) / 2;
  if(ic >= 0) {
    icon((Icon)ic, tx, y + (h - P(16)) / 2, P(16), ink);
    tx += P(24);
  }
  text(tx, y + (h - F(14)) / 2, F(14), ink, label, true);
  if(c.focus) focus_ring(x, y, w, h, r);
  return c.hit;
}

/* An on/off switch; true when clicked (the caller flips what it shows). */
bool toggle(float x, float y, bool on, bool enabled = true) {
  const float w = P(52), h = P(30), k = P(24);
  const Ctl c = control(x, y, w, h, enabled);
  rect(x, y, w, h, on ? (c.hot ? GOLD_LIGHT : GOLD) : (c.hot ? IM_COL32(0x46, 0x4C, 0x57, 255) : IM_COL32(0x3A, 0x3F, 0x48, 255)), h / 2);
  dot(on ? x + w - P(3) - k / 2 : x + P(3) + k / 2, y + h / 2, k / 2, on ? INK : SOFT);
  if(c.focus) focus_ring(x, y, w, h, h / 2);
  return c.hit;
}

float segmented_w(const char *const *opts, int n) {
  float w = P(6) + P(3) * (n - 1);
  for(int i = 0; i < n; i++) w += text_w(F(14), opts[i], true) + P(32);
  return w;
}

/* Two or three choices side by side, one picked. Returns the one picked now
   (the same one when nothing changed). Left and Right move it from the
   keyboard. */
int segmented(float x, float y, const char *const *opts, int n, int sel, bool enabled = true) {
  const float h = P(38), w = segmented_w(opts, n);
  card(x, y, w, h, BG, LINE, P(10));
  float ox = x + P(3);
  int pick = sel;
  for(int i = 0; i < n; i++) {
    const float ow = text_w(F(14), opts[i], true) + P(32);
    const bool on = i == sel;
    const bool hot = enabled && over(ox, y + P(3), ow, h - P(6));
    if(hot && !on) g_hand = true;
    if(on) rect(ox, y + P(3), ow, h - P(6), SEG_ON, P(8));
    else if(hot) rect(ox, y + P(3), ow, h - P(6), HOVER, P(8));
    text(ox + P(16), y + (h - F(14)) / 2, F(14), !enabled ? FAINT : on ? TEXT : MUTED, opts[i], on);
    if(hot && g_mouse.click && !g_click_taken && !on) {
      g_click_taken = true;
      pick = i;
    }
    ox += ow + P(3);
  }
  /* For the keyboard's focus, after the options have had the click. */
  const Ctl c = control(x, y, w, h, enabled);
  if(c.focus && enabled && (g_key == ACT_LEFT || g_key == ACT_RIGHT)) {
    pick = (sel + (g_key == ACT_RIGHT ? 1 : n - 1)) % n;
    g_key = ACT_NONE;
  }
  if(c.focus) focus_ring(x, y, w, h, P(10));
  return pick;
}

void end_edit() {
  if(g_edit_key.empty()) return;
  g_edit_key.clear();
  g_edit_text.clear();
  platform::set_text_input(false);
}

/* A text field typed into where it is. Returns true once, when an edit is
   finished (Enter, or a click somewhere else); value then holds what was
   typed. Esc puts it back. */
bool field(const char *key, float x, float y, float w, std::string &value, bool enabled = true,
           bool secret = false, bool is_mono = false, bool right = false) {
  const float h = P(36);
  const Ctl c = control(x, y, w, h, enabled);
  bool editing = g_edit_key == key, done = false;
  if(c.hit && enabled && !editing) {
    end_edit();
    g_edit_key = key;
    g_edit_text = value;
    g_edit_fresh = !value.empty();
    platform::set_text_input(true);
    editing = true;
  }
  if(editing) {
    g_edit_seen = true;
    std::string typed;
    bool bs, enter, esc;
    if(platform::take_typed(typed, bs, enter, esc)) {
      if(g_edit_fresh && (bs || !typed.empty())) {
        g_edit_text.clear();  // the selection goes
        bs = false;
      }
      if(bs || !typed.empty()) g_edit_fresh = false;
      if(bs && !g_edit_text.empty()) {
        g_edit_text.pop_back();
        while(!g_edit_text.empty() && ((unsigned char)g_edit_text.back() & 0xC0) == 0x80) g_edit_text.pop_back();
      }
      g_edit_text += typed;
      if(enter) {
        value = g_edit_text;
        done = true;
      }
      if(enter || esc) {
        end_edit();
        editing = false;
      }
    }
    if(editing && g_mouse.click && !over(x, y, w, h)) {  // the click goes on to whatever it is on
      value = g_edit_text;
      done = true;
      end_edit();
      editing = false;
    }
  }
  card(x, y, w, h, BG, editing ? GOLD : enabled ? BORDER : LINE, P(8));
  /* A password shows as dots, as many whatever its length. */
  std::string shown = editing ? g_edit_text : secret ? (value.empty() ? "" : "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2") : value;
  const float size = F(14), pad = P(12);
  shown = fit(shown, size, w - 2 * pad - P(6), is_mono);
  const float tw = is_mono ? mono_w(size, shown.c_str()) : text_w(size, shown.c_str());
  const float tx = right && !editing ? x + w - pad - tw : x + pad;
  const ImU32 col = enabled ? TEXT : MUTED;
  if(editing && g_edit_fresh) rect(tx - P(2), y + P(7), tw + P(4), h - P(14), IM_COL32(0x5A, 0x4A, 0x22, 255), P(3));
  if(is_mono) mono(tx, y + (h - size) / 2, size, col, shown.c_str());
  else text(tx, y + (h - size) / 2, size, col, shown.c_str());
  if(editing && !g_edit_fresh && fmod(now(), 1.0) < 0.55) rect(tx + tw + P(1), y + P(8), std::max(px(), P(1.5f)), h - P(16), GOLD);
  if(c.focus) focus_ring(x, y, w, h, P(8));
  return done;
}

/* A number with − and + either side. Returns the new value. */
int stepper(float x, float y, int value, int lo, int hi) {
  const float h = P(36), bw = P(36);
  char num[16];
  snprintf(num, sizeof(num), "%d", value);
  if(button(x, y, "\xe2\x88\x92", SECONDARY, -1, value > lo, h, bw)) value--;  // a real minus
  text(x + bw + (P(56) - text_w(F(14), num, true)) / 2, y + (h - F(14)) / 2, F(14), TEXT, num, true);
  if(button(x + bw + P(56), y, "+", SECONDARY, -1, value < hi, h, bw)) value++;
  return std::max(lo, std::min(hi, value));
}

/* A value that opens a menu of choices; true when clicked. */
bool chooser(float x, float y, float w, const std::string &value) {
  const float h = P(36);
  const Ctl c = control(x, y, w, h);
  card(x, y, w, h, c.hot ? SEG_ON : RAISED, BORDER, P(8));
  text(x + P(12), y + (h - F(14)) / 2, F(14), TEXT, fit(value, F(14), w - P(44), false).c_str());
  icon(IC_DOWN, x + w - P(28), y + (h - P(16)) / 2, P(16), MUTED);
  if(c.focus) focus_ring(x, y, w, h, P(8));
  return c.hit;
}

/* ---------- pieces of a page ---------- */

/* The title, its line under it, and the switch on the right that starts and
   stops what the page is about. Returns true when the switch was clicked. */
bool header(float x, float y, float w, const char *title, const char *sub, bool running, bool has_switch = true) {
  text(x, y, F(26), TEXT, title, true);
  text(x, y + P(44), F(15), MUTED, sub);
  if(!has_switch) return false;
  const float sx = x + w - P(52);
  const char *state = running ? tr("Running") : tr("Stopped");
  const float stw = text_w(F(14), state, true);
  dot(sx - P(20) - stw - P(10), y + P(15), P(4), running ? GREEN : FAINT);
  text(sx - P(16) - stw, y + P(15) - F(14) / 2, F(14), running ? GREEN : MUTED, state, true);
  return toggle(sx, y, running);
}

/* A row of a grouped card: label (and a line under it) on the left; the
   control is the caller's, right-aligned at the returned x. */
void row_label(float x, float y, float h, const char *label, const char *note) {
  if(note && *note) {
    text(x + P(24), y + h / 2 - F(14) - P(1), F(14), TEXT, label);
    text(x + P(24), y + h / 2 + P(2), F(13), MUTED, note);
  } else {
    text(x + P(24), y + (h - F(14)) / 2, F(14), TEXT, label);
  }
}

void feature(float x, float y, float w, float h, Icon ic, const char *title, const char *body) {
  card(x, y, w, h);
  rect(x + P(22), y + P(20), P(40), P(40), GOLD_TINT, P(10));
  icon(ic, x + P(32), y + P(30), P(20), GOLD);
  const float tx = x + P(78);
  text(tx, y + P(20), F(14), TEXT, title, true);
  float ty = y + P(44);
  for(const std::string &l : wrap_text(body, F(13), x + w - P(22) - tx)) {
    text(tx, ty, F(13), MUTED, l.c_str());
    ty += P(20);
  }
}

void copy_address(const std::string &what) {
  show_toast(platform::copy_text(what.c_str()) ? trf("Copied {0}", {what}) : std::string(tr("Could not copy")),
             false);
}

/* ---------- the sidebar ---------- */

void go(Page p) {
  end_edit();
  g_page = p;
  g_focus = -1;
  if(p == PAGE_QUICK) discovery_probe();  // the list fills in now, not in a few seconds
  g.mode = MODE_HOME;  // the console layout's screens aren't running
}

void nav(float x, float y, float w, Page p, Icon ic, const char *title, const char *status, ImU32 status_col) {
  const float h = status ? P(56) : P(42);
  const bool on = g_page == p;
  const Ctl c = control(x, y, w, h);
  if(on) rect(x, y, w, h, LINE, P(10));
  else if(c.hot) rect(x, y, w, h, HOVER, P(10));
  icon(ic, x + P(12), y + (h - P(20)) / 2, P(20), on ? GOLD : MUTED);
  const float tx = x + P(44);
  /* Inside the highlight, whatever the language: a status such as "Running
     · this computer only" ran past its right edge. (Bold is a little wider
     than fit_end measures, hence the extra margin on the title.) */
  const float right = x + w - P(10);
  const std::string t = fit_end(title, F(14), right - tx - (on ? P(6) : 0));
  if(status) {
    text(tx, y + P(9), F(14), on ? TEXT : SOFT, t.c_str(), on);
    dot(tx + P(3), y + P(39), P(3.5f), status_col);
    text(tx + P(12), y + P(31), F(12), MUTED, fit_end(status, F(12), right - tx - P(12)).c_str());
  } else {
    text(tx, y + (h - F(14)) / 2, F(14), on ? TEXT : SOFT, t.c_str(), on);
  }
  if(c.focus) focus_ring(x, y, w, h, P(10));
  if(c.hit) go(p);
}

void sidebar() {
  rect(0, 0, SIDE_W, H, SIDE);
  rect(SIDE_W - px(), 0, px(), H, LINE);
  draw::brand_mark(P(22), P(24), P(26));
  draw::brand_wordmark(P(56), P(26), F(18));

  const float x = P(12), w = SIDE_W - P(24);
  float y = P(80);
  const bool srv = server_running();
  const std::string srv_status = srv ? trf("{0} · Running", {server_is_sftp() ? "SFTP" : "FTP"}) : std::string(tr("Stopped"));
  nav(x, y, w, PAGE_SERVER, IC_SERVER, tr("FTP / SFTP server"), srv_status.c_str(), srv ? GREEN : FAINT);
  y += P(60);
  const bool web = websrv_running();
  // Running is the green dot; the words say who can reach it (they fit the sidebar, unlike "Running · this computer only").
  const char *web_status = !web ? tr("Stopped") : web_local() ? tr("This computer only") : tr("Home network");
  nav(x, y, w, PAGE_WEB, IC_WINDOW, tr("Web app"), web_status, web ? GREEN : FAINT);
  y += P(60);
  float by = H - P(20) - P(42) * 2 - P(4);
  {
    /* Quick connect, above the log and Settings: the other chiral-ftp
       devices on this network. */
    discovery_peer_t peers[DISCOVERY_MAX_PEERS];
    const int n = discovery_peers(peers, DISCOVERY_MAX_PEERS);
    const std::string st = n ? trf("{0} nearby", {std::to_string(n)}) : std::string(tr("Looking..."));
    nav(x, by - P(64), w, PAGE_QUICK, IC_QUICK, tr("Quick connect"), st.c_str(), n ? GREEN : FAINT);
  }
  nav(x, by, w, PAGE_LOG, IC_LINES, tr("Message log"), nullptr, 0);
  nav(x, by + P(46), w, PAGE_SETTINGS, IC_GEAR, tr("Settings"), nullptr, 0);
}

/* ---------- pages ---------- */

void page_web(float x, float y, float w) {
  const bool running = websrv_running(), local = web_local();
  if(header(x, y, w, tr("Web app"), tr("The file manager and the FTP / SFTP client, in your browser."), running)) {
    if(running) websrv_stop();
    else start_web();
  }
  y += P(100);

  // The address, and what to do with it.
  const float ah = P(100);
  card(x, y, w, ah);
  text(x + P(24), y + P(22), F(12), MUTED, tr("ADDRESS"), true);
  const std::string addr = web_address();
  mono(x + P(24), y + P(44), F(24), running ? TEXT : MUTED, addr.c_str());
  const char *open = tr("Open in browser"), *cp = tr("Copy");
  float bx = x + w - P(24) - button_w(cp, true);
  if(button(bx, y + (ah - P(40)) / 2, cp, SECONDARY, IC_COPY)) copy_address(addr);
  bx -= P(10) + button_w(open, true);
  if(button(bx, y + (ah - P(40)) / 2, open, PRIMARY, IC_OPEN, running)) platform::open_url(addr.c_str());
  y += ah + P(24);

  // Who may open it, and on which port.
  const float rh = P(72);
  card(x, y, w, rh * 2);
  const char *reach[2] = {tr("This computer"), tr("Home network")};
  row_label(x, y, rh, tr("Reachable from"),
            running ? tr("Turn the web app off to change it.")
                    : local ? tr("Only browsers on this computer can open it.")
                            : tr("Phones and other devices on your network can open it too."));
  const int pick = segmented(x + w - P(24) - segmented_w(reach, 2), y + (rh - P(38)) / 2, reach, 2, local ? 0 : 1, !running);
  if(pick != (local ? 0 : 1)) {
    g_srv.web_local = pick == 0;
    save_server_prefs();
  }
  hline(x + P(24), y + rh, w - P(48));
  row_label(x, y + rh, rh, tr("Port"), running ? tr("Turn the web app off to change it.") : "");
  std::string port = std::to_string(g_srv.web_port);
  if(field("web-port", x + w - P(24) - P(96), y + rh + (rh - P(36)) / 2, P(96), port, !running, false, true, true)) {
    const int p = atoi(port.c_str());
    if(p < 1 || p > 65535) show_toast(tr("A port is a number from 1 to 65535"), true);
    else {
      g_srv.web_port = p;
      save_server_prefs();
    }
  }
  y += rh * 2 + P(24);

  // What it has.
  const float fw = (w - P(16)) / 2, fh = P(104);
  feature(x, y, fw, fh, IC_FOLDER, tr("Files"),
          tr("This computer's files: browse, upload, download, copy, move, rename and delete. Drag and drop works."));
  feature(x + fw + P(16), y, fw, fh, IC_TRANSFER, tr("FTP / SFTP client"),
          tr("Connect to servers and transfer files, with the same connections and queue as this window."));
  g_content_h = y + fh + P(48);
}

void page_server(float x, float y, float w) {
  const bool running = server_running(), sftp = server_is_sftp();
  if(header(x, y, w, tr("FTP / SFTP server"),
            tr("Share a folder of this computer over SFTP or FTP, with a phone, a console or another computer."), running)) {
    if(running) stop_server();
    else start_server();
  }
  y += P(100);

  /* Two columns where the window is wide, one above the other where not. */
  const bool wide = w >= P(900);
  const float lw = wide ? (w - P(24)) * 0.6f : w, rx = wide ? x + lw + P(24) : x;
  float ly = y;

  // How to connect.
  {
    const float ch = sftp ? P(200) : P(140);
    card(x, ly, lw, ch);
    const char *protos[2] = {"SFTP", "FTP"};
    const int pick = segmented(x + P(24), ly + P(22), protos, 2, sftp ? 0 : 1, !running);
    if(pick != (sftp ? 0 : 1)) {
#if CHIRAL_HAS_SFTPD
      g_srv.sftp = pick == 0;
      save_server_prefs();
#endif
    }
    const char *note = running ? tr("Turn the server off to change it.")
                       : sftp  ? tr("Encrypted: for most apps.")
                               : tr("Not encrypted: for apps that only speak FTP.");
    text(x + P(24) + segmented_w(protos, 2) + P(16), ly + P(22) + (P(38) - F(13)) / 2, F(13), MUTED, note);
    const std::string addr = server_address();
    mono(x + P(24), ly + P(80), F(24), running ? TEXT : MUTED, fit(addr, F(24), lw - P(170), true).c_str());
    const char *cp = tr("Copy");
    if(button(x + lw - P(24) - button_w(cp, true), ly + P(76), cp, SECONDARY, IC_COPY)) copy_address(addr);
#if CHIRAL_HAS_SFTPD
    if(sftp) {
      char fp[128] = "";
      sftpd_fingerprint(fp, sizeof(fp));
      const float ky = ly + P(136);
      rect(x + P(24), ky, lw - P(48), P(44), BG, P(10));
      icon(IC_KEY, x + P(38), ky + P(14), P(16), GOLD);
      if(fp[0]) {
        const char *label = tr("Key");
        text(x + P(64), ky + (P(44) - F(13)) / 2, F(13), MUTED, label);
        const float kx = x + P(64) + text_w(F(13), label) + P(8);
        mono(kx, ky + (P(44) - F(13)) / 2, F(13), TEXT, fit(fp, F(13), x + lw - P(36) - kx, true).c_str());
      } else {
        text(x + P(64), ky + (P(44) - F(13)) / 2, F(13), MUTED, tr("The key shows here once the server has started."));
      }
    }
#endif
    ly += ch + P(20);
  }

  // Login, folder and port.
  {
    const float rh = P(64);
    const bool default_login = !strcmp(g_srv.ftp.user, FTPD_DEFAULT_USER) && !strcmp(g_srv.ftp.pass, FTPD_DEFAULT_PASS);
    const int rows = 4 + (sftp ? 0 : 1);
    /* The warning wraps rather than being cut, however narrow the column. */
    const std::vector<std::string> warning =
        default_login ? wrap_text(tr("Still the default password. Change it before sharing on a network you don't trust."), F(13), lw - P(74))
                      : std::vector<std::string>();
    const float wh = default_login ? std::max(P(44), P(16) + P(20) * (float)warning.size()) : 0;
    card(x, ly, lw, rh * rows + wh);
    float yy = ly;
    const float fw = std::min(P(240), lw * 0.45f), fx = x + lw - P(24) - fw;

    row_label(x, yy, rh, tr("Username"), "");
    std::string user = g_srv.ftp.user;
    if(field("srv-user", fx, yy + (rh - P(36)) / 2, fw, user)) {
      if(user.empty()) show_toast(tr("The username can't be empty"), true);
      else {
        snprintf(g_srv.ftp.user, sizeof(g_srv.ftp.user), "%s", user.c_str());
        save_server_prefs();
      }
    }
    yy += rh;
    hline(x + P(24), yy, lw - P(48));
    row_label(x, yy, rh, tr("Password"), "");
    std::string pass = g_srv.ftp.pass;
    if(field("srv-pass", fx, yy + (rh - P(36)) / 2, fw, pass, true, true)) {
      if(pass.empty()) show_toast(tr("The password can't be empty; turn on anonymous login instead"), true);
      else {
        snprintf(g_srv.ftp.pass, sizeof(g_srv.ftp.pass), "%s", pass.c_str());
        save_server_prefs();
      }
    }
    yy += rh;
    if(default_login) {
      rect(x + px(), yy, lw - 2 * px(), wh, GOLD_TINT);
      icon(IC_WARN, x + P(24), yy + (wh - P(16)) / 2, P(16), GOLD_LIGHT);
      float ty = yy + (wh - P(20) * (float)warning.size()) / 2 + (P(20) - F(13)) / 2;
      for(const std::string &l : warning) {
        text(x + P(50), ty, F(13), GOLD_LIGHT, l.c_str());
        ty += P(20);
      }
      yy += wh;
    } else {
      hline(x + P(24), yy, lw - P(48));
    }
    if(!sftp) {
      row_label(x, yy, rh, tr("Anonymous login"), "");
      if(toggle(x + lw - P(24) - P(52), yy + (rh - P(30)) / 2, g_srv.ftp.anonymous)) {
        g_srv.ftp.anonymous = !g_srv.ftp.anonymous;
        save_server_prefs();
      }
      yy += rh;
      hline(x + P(24), yy, lw - P(48));
    }
    row_label(x, yy, rh, tr("Shared folder"), "");
    {
      const char *choose = tr("Choose…"), *whole = tr("Whole computer");
      const float bw = button_w(choose, false), bx = x + lw - P(24) - bw;
      const float ww = button_w(whole, false), wx = bx - P(10) - ww;
      if(button(wx, yy + (rh - P(36)) / 2, whole, SECONDARY, -1, true, P(36))) {
        end_edit();
        share_whole_computer();
      }
      const std::string shown = fit(short_path(g_srv.ftp.root), F(14), wx - P(40) - (x + P(160)), true);
      const float sw = mono_w(F(14), shown.c_str());
      icon(IC_FOLDER, wx - P(14) - sw - P(24), yy + (rh - P(18)) / 2, P(18), GOLD);
      mono(wx - P(14) - sw, yy + (rh - F(14)) / 2, F(14), TEXT, shown.c_str());
      if(button(bx, yy + (rh - P(36)) / 2, choose, SECONDARY, -1, true, P(36))) {
        std::string out;
        auto set_root = [](const std::string &v) {
          struct stat st;
          if(v.empty() || stat(v.c_str(), &st) || !S_ISDIR(st.st_mode)) return show_toast(trf("Not a folder: {0}", {v}), true);
          set_shared_root(v);
          show_toast(trf("Sharing {0}", {short_path(v)}));
        };
        end_edit();
        if(platform::choose_folder(g_srv.ftp.root, out)) {
          set_root(out);
        } else {
          /* No folder panel on this system: type it. */
          prompt(tr("Shared folder"), short_path(g_srv.ftp.root), TEXT_URL, [set_root](std::string v) {
            const char *home = getenv("HOME");
            if(!v.empty() && v[0] == '~' && home) v = home + v.substr(1);
            set_root(v);
          });
        }
      }
    }
    yy += rh;
    hline(x + P(24), yy, lw - P(48));
    row_label(x, yy, rh, tr("Port"), running ? tr("Turn the server off to change it.") : "");
    std::string port = std::to_string(server_port());
    if(field(sftp ? "srv-sftp-port" : "srv-ftp-port", x + lw - P(24) - P(96), yy + (rh - P(36)) / 2, P(96), port, !running,
             false, true, true)) {
      const int p = atoi(port.c_str());
      if(p < 1 || p > 65535) show_toast(tr("A port is a number from 1 to 65535"), true);
      else {
        if(sftp) g_srv.sftp_port = p;
        else g_srv.ftp.port = (unsigned short)p;
        save_server_prefs();
      }
    }
    yy += rh;
    ly = yy + P(20);
  }

  // What is happening.
  float ay = wide ? y : ly;
  {
    const float aw = wide ? w - lw - P(24) : w;
    ftpd_stats_t st;
    server_stats(&st);
    char lines[16][160];
    const int n = server_recent(lines, 16);
    const float list_h = std::max(P(60), P(26) * (float)n);
    const float ah = P(150) + list_h;
    card(rx, ay, aw, ah);
    text(rx + P(24), ay + P(20), F(16), TEXT, tr("Activity"), true);
    const float tw = (aw - P(58)) / 2;
    const std::string clients = std::to_string(st.clients), sent = fmt_size(st.bytes_up);
    const std::string got = trf("sent · {0} received", {fmt_size(st.bytes_down)});
    rect(rx + P(24), ay + P(54), tw, P(60), BG, P(10));
    text(rx + P(38), ay + P(62), F(20), TEXT, clients.c_str(), true);
    text(rx + P(38), ay + P(92), F(12), MUTED, tr("connected"));
    rect(rx + P(34) + tw, ay + P(54), tw, P(60), BG, P(10));
    text(rx + P(48) + tw, ay + P(62), F(20), TEXT, sent.c_str(), true);
    text(rx + P(48) + tw, ay + P(92), F(12), MUTED, fit(got, F(12), tw - P(28), false).c_str());
    float yy = ay + P(132);
    if(!n) text(rx + P(24), yy, F(13), MUTED, tr("Logins and transfers show here."));
    for(int i = n - 1; i >= 0; i--, yy += P(26)) {  // newest first
      const char *l = lines[i];
      const char *gap = strstr(l, "  ");
      std::string when = gap ? std::string(l, gap) : "", what = gap ? gap + 2 : l;
      mono(rx + P(24), yy, F(13), FAINT, when.c_str());
      const float wx = rx + P(24) + mono_w(F(13), "00:00:00") + P(12);
      text(wx, yy, F(13), TEXT, fit_end(what, F(13), rx + aw - P(24) - wx).c_str());
    }
    ay += ah;
  }
  g_content_h = std::max(ly, ay) + P(28);
}

/* Where to get chiral-ftp for the other devices, under Quick connect's
   list: the download page as a QR code, and the two pages as links that
   open in the browser. qs is the code's size; returns the card's height.
   The phone's page has it too. */
float download_card(float x, float y, float w, float qs) {
  const float pad = P(16), tx = x + pad + qs + P(20), tw = x + w - pad - tx;
  const std::vector<std::string> lines =
      wrap_text(tr("Free for Windows, Mac, Linux, Android, Nintendo Switch and PS5. Scan the code, or go to:"), F(13), tw);
  const float lh = P(20), text_h = P(26) + lines.size() * lh + P(6) + 2 * P(24);
  const float ch = std::max(qs, text_h) + 2 * pad;
  card(x, y, w, ch);
  download_qr(x + pad, y + (ch - qs) / 2, qs);
  float ty = y + pad;
  text(tx, ty, F(15), TEXT, fit_end(tr("chiral-ftp on your other devices"), F(15), tw - P(8)).c_str(), true);
  ty += P(26);
  for(const std::string &l : lines) {
    text(tx, ty, F(13), MUTED, l.c_str());
    ty += lh;
  }
  ty += P(6);
  for(const char *page : {kDownloadPage, kReleasesPage}) {
    const std::string shown = fit_end(page, F(13), tw);
    const float lw = text_w(F(13), shown.c_str());
    const Ctl c = control(tx, ty, lw, P(20));
    text(tx, ty, F(13), c.hot ? GOLD_LIGHT : GOLD, shown.c_str());
    if(c.hot || c.focus) hline(tx, ty + F(13) + P(1), lw, GOLD);
    if(c.hit) platform::open_url((std::string("https://") + page).c_str());
    ty += P(24);
  }
  return ch;
}

/* Quick connect: the devices nearby, each with Connect; then the wait for
   its answer, with the code to compare. */
void page_quick(float x, float y, float w) {
  text(x, y, F(26), TEXT, tr("Quick connect"), true);
  text(x, y + P(44), F(15), MUTED, tr("chiral-ftp on other devices on this network. Ask one to share, then browse it here."));
  const char *refresh = tr("Refresh");
  if(button(x + w - button_w(refresh, false), y, refresh, SECONDARY)) discovery_probe();
  y += P(100);

  discovery_outcome_t o;
  discovery_get_outcome(&o);
  if(o.state == DISCOVERY_ASKING) {
    const float ch = P(260);
    card(x, y, w, ch);
    text(x + P(28), y + P(26), F(20), TEXT, trf("Asking {0}...", {o.name}).c_str(), true);
    text(x + P(28), y + P(62), F(14), MUTED, trf("Accept on {0} to connect. Check that it shows the same code:", {o.name}).c_str());
    const float bs = P(56), gap = P(12), total = 4 * bs + 3 * gap;
    for(int i = 0; i < 4 && o.code[i]; i++) {
      const float bx = x + P(28) + i * (bs + gap);
      rect(bx, y + P(100), bs, bs * 1.2f, BG, P(10));
      char d[2] = {o.code[i], 0};
      mono(bx + (bs - mono_w(F(30), d)) / 2, y + P(100) + (bs * 1.2f - F(30)) / 2, F(30), GOLD, d);
    }
    (void)total;
    draw::spinner(x + P(36), y + P(205), P(7), GOLD);
    text(x + P(52), y + P(196), F(13), MUTED, trf("Waiting · {0} · gives up in {1} s", {o.ip, std::to_string(o.seconds_left)}).c_str());
    const char *cancel = tr("Cancel");
    if(button(x + w - P(28) - button_w(cancel, false), y + ch - P(28) - P(40), cancel)) discovery_cancel();
    g_content_h = y + ch + P(48);
    return;
  }

  discovery_peer_t peers[DISCOVERY_MAX_PEERS];
  const int n = discovery_peers(peers, DISCOVERY_MAX_PEERS);
  if(!n) {
    const float ch = P(110);
    card(x, y, w, ch);
    if(!discovery_running() && discovery_problem()[0]) {
      text(x + P(28), y + P(30), F(14), RED, tr("Quick connect isn't available here: the network refused it (see the message log)."));
    } else {
      draw::spinner(x + P(38), y + P(42), P(8), GOLD);
      text(x + P(60), y + P(28), F(15), TEXT, tr("Looking for chiral-ftp on your network..."), true);
      text(x + P(60), y + P(56), F(13), MUTED, tr("Open chiral-ftp on the other device: it shows up here by itself."));
    }
    y += ch + P(16);
  }
  const float rh = P(76);
  for(int i = 0; i < n; i++) {
    const discovery_peer_t &p = peers[i];
    card(x, y, w, rh);
    kind_square(x + P(22), y + (rh - P(44)) / 2, P(44), p.kind);
    text(x + P(82), y + P(17), F(15), TEXT, p.name, true);
    const std::string sub = std::string(kind_label(p.kind)) + "  \xc2\xb7  " + p.ip + "  \xc2\xb7  chiral-ftp " + p.version;
    text(x + P(82), y + P(43), F(13), MUTED, sub.c_str());
    const char *connect_label = tr("Connect");
    if(button(x + w - P(22) - button_w(connect_label, false), y + (rh - P(40)) / 2, connect_label, PRIMARY)) discovery_ask(&p);
    y += rh + P(12);
  }
  char me[64];
  system_device_name(me, sizeof(me));
  text(x + P(4), y + P(8), F(13), MUTED, trf("This computer shows up on the others as \"{0}\".", {me}).c_str());
  y += P(44);
  g_content_h = y + download_card(x, y, w, P(132)) + P(48);
}

void page_log(float x, float y, float w) {
  text(x, y, F(26), TEXT, tr("Message log"), true);
  text(x, y + P(44), F(15), MUTED, tr("What the app, the servers and the client have been doing."));
  const char *clear = tr("Clear");
  if(button(x + w - button_w(clear, false), y, clear, SECONDARY, -1, !g.log.empty())) {
    g.log.clear();
    log_clear();
  }
  y += P(100);
  const float lh = P(24);
  const float h = std::max(P(200), lh * (float)g.log.size() + P(40));
  card(x, y, w, h);
  float yy = y + P(20);
  if(g.log.empty()) text(x + P(24), yy, F(13), MUTED, tr("Nothing yet."));
  for(auto &e : g.log) {
    const ImU32 col = e.first == LOG_COMMAND ? GOLD_LIGHT : e.first == LOG_RESPONSE ? GREEN : e.first == LOG_ERROR ? RED : SOFT;
    mono(x + P(24), yy, F(12), col, fit(e.second, F(12), w - P(48), true).c_str());
    yy += lh;
  }
  g_content_h = y + h + P(48);
}

/* Settings, as grouped rows: a label on the left, its control on the right. */
void page_settings(float x, float y, float w) {
  text(x, y, F(26), TEXT, tr("Settings"), true);
  text(x, y + P(44), F(15), MUTED, tr("Saved right away."));
  y += P(100);
  const float rh = P(64), cw = std::min(w, P(300));
  settings_t s = settings_get();
  bool changed = false;

  auto group = [&](const char *title, int rows) {
    text(x + P(4), y, F(12), MUTED, title, true);
    y += P(24);
    card(x, y, w, rh * rows);
  };
  auto sep = [&](float yy) { hline(x + P(24), yy, w - P(48)); };
  const float rx = x + w - P(24);

  // General
  system_net_t nets[16];
  const int nn = system_networks(nets, 16);
  group(tr("GENERAL"), nn > 0 ? 2 : 1);
  float yy = y;
  row_label(x, yy, rh, tr("Language"), "");
  if(chooser(rx - cw, yy + (rh - P(36)) / 2, cw, Settings::language_value())) {
    Menu *m = new Menu;
    m->title = tr("Language");
    std::string current = i18n_current();
    std::string auto_name;
    for(int i = 0; i < i18n_count(); i++) if(current == i18n_code(i)) auto_name = i18n_native_name(i);
    m->items.push_back({trf("Automatic ({0})", {auto_name}), [] { i18n_set("auto"); save_prefs(); }});
    for(int i = 0; i < i18n_count(); i++) {
      std::string code = i18n_code(i);
      m->items.push_back({i18n_native_name(i), [code] { i18n_set(code.c_str()); save_prefs(); }});
    }
    push(m);
  }
  yy += rh;
  if(nn > 0) {
    sep(yy);
    row_label(x, yy, rh, tr("Network"), tr("The address the server and web app show."));
    if(chooser(rx - cw, yy + (rh - P(36)) / 2, cw, Settings::network_value())) {
      Menu *m = new Menu;
      m->title = tr("Network");
      m->items.push_back({trf("Automatic ({0})", {pick_ip()}), [] { g.network.clear(); save_prefs(); ip_changed(); }});
      for(int i = 0; i < nn; i++) {
        std::string name = nets[i].name;
        m->items.push_back({Settings::network_name(nets[i]), [name] { g.network = name; save_prefs(); ip_changed(); }});
      }
      push(m);
    }
    yy += rh;
  }
  y = yy + P(28);

  // Transfers
  group(tr("TRANSFERS"), 6);
  yy = y;
  row_label(x, yy, rh, tr("Simultaneous transfers"), tr("Each logs in with its own connection, like FileZilla."));
  {
    const int v = stepper(rx - P(128), yy + (rh - P(36)) / 2, s.connections, 1, MAX_TRANSFER_CONNECTIONS);
    if(v != s.connections) s.connections = v, changed = true;
  }
  yy += rh;
  sep(yy);
  row_label(x, yy, rh, tr("When the file already exists"), "");
  if(chooser(rx - cw, yy + (rh - P(36)) / 2, cw, Settings::policy_name(s.policy))) {
    Menu *m = new Menu;
    m->title = tr("When the file already exists");
    const int order[4] = {POLICY_RESUME, POLICY_OVERWRITE, POLICY_SKIP, POLICY_RENAME};
    for(int p : order) {
      m->items.push_back({Settings::policy_name(p), [p] {
        settings_t t = settings_get();
        t.policy = p;
        settings_set(&t);
        queue_kick();
      }});
    }
    push(m);
  }
  yy += rh;
  sep(yy);
  row_label(x, yy, rh, tr("Retries after a dropped connection"), "");
  {
    const int v = stepper(rx - P(128), yy + (rh - P(36)) / 2, s.retries, 0, 10);
    if(v != s.retries) s.retries = v, changed = true;
  }
  yy += rh;
  sep(yy);
  row_label(x, yy, rh, tr("Keep file dates"), "");
  if(toggle(rx - P(52), yy + (rh - P(30)) / 2, s.preserve_mtime)) s.preserve_mtime = !s.preserve_mtime, changed = true;
  yy += rh;
  sep(yy);
  row_label(x, yy, rh, tr("Show hidden files (FTP LIST -a)"), "");
  if(toggle(rx - P(52), yy + (rh - P(30)) / 2, g.show_hidden)) {
    g.show_hidden = !g.show_hidden;
    save_prefs();
  }
  yy += rh;
  sep(yy);
  row_label(x, yy, rh, tr("Remember passwords in Recent"), "");
  if(toggle(rx - P(52), yy + (rh - P(30)) / 2, g.save_pass)) {
    g.save_pass = !g.save_pass;
    if(!g.save_pass) {
      for(auto &h : g.history) json::set_str(h, "pass", "");
      save_store("history", g.history);
    }
    save_prefs();
  }
  yy += rh;
  y = yy + P(28);

  // The rest
  group(tr("MORE"), 1);
  yy = y;
  row_label(x, yy, rh, tr("Forget saved SFTP host keys"), tr("The next connection trusts the server's current key."));
  {
    const char *forget = tr("Forget");
    if(button(rx - button_w(forget, false), yy + (rh - P(36)) / 2, forget, DANGER, -1, true, P(36))) {
      confirm(tr("Forget host keys"), tr("Forget every saved SFTP server key? The next connection trusts the server's current key."),
              tr("Forget"), [] { show_toast(ftp_forget_host_keys() ? tr("Could not remove the keys") : tr("Saved host keys forgotten")); });
    }
  }
  yy += rh;
  sep(yy);
  row_label(x, yy, rh, tr("About chiral-ftp"), trf("version {0}", {VERSION_TAG}).c_str());
  {
    const char *about = tr("About");
    if(button(rx - button_w(about, false), yy + (rh - P(36)) / 2, about, SECONDARY, -1, true, P(36))) push(new About);
  }
  yy += rh;
  g_content_h = yy + P(48);

  if(changed) {
    settings_set(&s);
    queue_kick();
  }
}

void toast() {
  if(now() >= g.toast_until || g.toast.empty() || g.toast_err) return;  // errors: draw_overlays shows them
  const float tw = std::min(W - SIDE_W - P(96), text_w(F(14), g.toast.c_str()) + P(48));
  const float tx = SIDE_W + (W - SIDE_W - tw) / 2, ty = H - P(72);
  card(tx, ty, tw, P(44), RAISED, BORDER, P(22));
  text(tx + P(24), ty + (P(44) - F(14)) / 2, F(14), TEXT, fit_end(g.toast, F(14), tw - P(48)).c_str());
}

void render() {
  g_focus_last = g_focus_count;
  g_focus_count = 0;
  g_hand = false;
  g_click_taken = false;
  g_edit_seen = false;
  if(!g.overlays.empty()) end_edit();  // a dialog has the keyboard now

  rect(0, 0, W, H, BG);
  {
    float &scroll = g_scroll[g_page];
    const float x = SIDE_W + P(48), w = std::min(W - x - P(48), P(1100));
    if(g_wheel != 0 && g.overlays.empty()) scroll -= g_wheel * P(40);
    scroll = std::max(0.0f, std::min(scroll, std::max(0.0f, g_content_h - H)));
    g_wheel = 0;
    draw::dl()->PushClipRect(ImVec2(SIDE_W, 0), ImVec2(W, H), true);
    const float y = P(36) - scroll;
    g_content_h = 0;
    switch(g_page) {
    case PAGE_WEB: page_web(x, y, w); break;
    case PAGE_QUICK: page_quick(x, y, w); break;
    case PAGE_LOG: page_log(x, y, w); break;
    case PAGE_SETTINGS: page_settings(x, y, w); break;
    default: page_server(x, y, w); break;
    }
    g_content_h += scroll;
    draw::dl()->PopClipRect();
  }
  sidebar();
  toast();

  if(!g_edit_seen) end_edit();  // its page went away
  if(g_focus >= g_focus_count) g_focus = g_focus_count - 1;
  g_mouse.click = g_mouse.right = false;
  if(g.overlays.empty()) platform::pointer_hand(g_hand);
}

/* A key for the desktop pages; false to let the console layout have it. */
bool key(Action a) {
  if(a == ACT_L1 || a == ACT_R1) {
    static const Page order[] = {PAGE_SERVER, PAGE_WEB, PAGE_QUICK, PAGE_LOG, PAGE_SETTINGS};
    const int n = (int)(sizeof(order) / sizeof(order[0]));
    int at = 0;
    for(int i = 0; i < n; i++) if(order[i] == g_page) at = i;
    go(order[(at + (a == ACT_R1 ? 1 : n - 1)) % n]);
    return true;
  }
  g_keys = true;
  const int n = std::max(1, g_focus_last);
  switch(a) {
  case ACT_UP: g_focus = g_focus <= 0 ? n - 1 : g_focus - 1; return true;
  case ACT_DOWN: g_focus = g_focus < 0 ? 0 : (g_focus + 1) % n; return true;
  case ACT_CROSS: case ACT_LEFT: case ACT_RIGHT: g_key = a; return true;
  case ACT_TOUCHPAD: push(new QueueView); return true;
  case ACT_OPTIONS: go(PAGE_SETTINGS); return true;
  case ACT_L2: g_scroll[g_page] -= P(400); return true;
  case ACT_R2: g_scroll[g_page] += P(400); return true;
  default: return true;  // nothing else means anything here
  }
}

/* The mouse; false to let the console layout's own handling have it (an
   open dialog). */
bool pointer(const platform::Pointer &p) {
  g_mouse.x = p.x;
  g_mouse.y = p.y;
  g_mouse.inside = p.inside;
  if(!g.overlays.empty()) return false;
  if(p.moved) g_keys = false;
  if(p.clicked) g_mouse.click = true;
  if(p.right_clicked) g_mouse.right = true;
  g_wheel += p.wheel;
  g_touch_dirty = true;
  return true;
}

/* The menu bar's commands, as pages here. False for the ones handled the
   console layout's way (dialogs over whatever is open). */
bool command(Action a) {
  switch(a) {
  case ACT_CMD_HOME: case ACT_CMD_FTPD: g.overlays.clear(); go(PAGE_SERVER); return true;
  case ACT_CMD_WEB: g.overlays.clear(); go(PAGE_WEB); return true;
  case ACT_CMD_SETTINGS: g.overlays.clear(); go(PAGE_SETTINGS); return true;
  case ACT_CMD_LOG: g.overlays.clear(); go(PAGE_LOG); return true;
  case ACT_CMD_QUICK: g.overlays.clear(); go(PAGE_QUICK); return true;
  default: end_edit(); return false;
  }
}

bool editing() {
  return !g_edit_key.empty();
}

}  // namespace desk

/* A request from another device, drawn the desktop's way: a sheet over the
   window. Its two buttons are the dialog's own (HIT_MENU_ITEM 0 Deny,
   1 Allow), clicked through the dialog handling every overlay shares. */
void draw_quick_prompt_desktop(const discovery_request_t &r) {
  using namespace desk;
  rect(0, 0, W, H, IM_COL32(5, 6, 8, 150));
  const float w = std::min(P(560), W - P(64)), x = (W - w) / 2, y = P(96);
  const std::vector<std::string> body =
      wrap_text(tr("It will be able to browse, download, upload and delete everything this server shares. Allow it only if it shows the same code."),
                F(14), w - P(56));
  const float h = P(260) + P(22) * (float)body.size();
  card(x, y, w, h, CARD, BORDER, P(16));
  hit_add(HIT_OVERLAY_BODY, x, y, w, h);
  kind_square(x + P(28), y + P(26), P(44), r.kind);
  text(x + P(86), y + P(28), F(18), TEXT, trf("{0} wants to connect", {r.name}).c_str(), true);
  text(x + P(86), y + P(54), F(13), MUTED, r.ip);
  float ty = y + P(92);
  for(const std::string &l : body) {
    text(x + P(28), ty, F(14), SOFT, l.c_str());
    ty += P(22);
  }
  const std::string spaced = std::string(1, r.code[0]) + " " + r.code[1] + " " + r.code[2] + " " + r.code[3];
  mono(x + (w - mono_w(F(34), spaced.c_str())) / 2, ty + P(10), F(34), GOLD, spaced.c_str());
  const float by = y + h - P(28) - P(40);
  text(x + P(28), by + (P(40) - F(13)) / 2, F(13), MUTED, trf("Refused on its own in {0} s", {std::to_string(r.seconds_left)}).c_str());
  const char *allow = tr("Allow"), *deny = tr("Deny");
  const float aw = button_w(allow, false), dw = button_w(deny, false);
  const float ax = x + w - P(28) - aw, dx = ax - P(10) - dw;
  card(dx, by, dw, P(40), over(dx, by, dw, P(40)) ? SEG_ON : RAISED, BORDER, P(10));
  text(dx + (dw - text_w(F(14), deny, true)) / 2, by + (P(40) - F(14)) / 2, F(14), TEXT, deny, true);
  hit_add(HIT_MENU_ITEM, dx, by, dw, P(40), 0);
  rect(ax, by, aw, P(40), over(ax, by, aw, P(40)) ? GOLD_LIGHT : GOLD, P(10));
  text(ax + (aw - text_w(F(14), allow, true)) / 2, by + (P(40) - F(14)) / 2, F(14), INK, allow, true);
  hit_add(HIT_MENU_ITEM, ax, by, aw, P(40), 1);
}
