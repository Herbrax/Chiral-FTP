/* The mobile interface (CHIRAL_UI_MOBILE: Android phones, tablets and
   foldables), part of ui.cpp's private half -- included there after
   ui_desktop.inc, whose controls and palette it is drawn with. Its screens
   are its own, laid out for a narrow window: the computer's pages assume
   one at least 700 points wide.

   A phone's window can be anything: a flip phone's cover, a tall phone, the
   near-square inside of a book foldable, a trifold opened flat, half of a
   split screen. Nothing here asks what device it is on; every frame the
   window's size, in dp, decides:

     glance     under 240dp either way: a status card
     cover      a small cover screen: slim chrome, panes as below
     portrait   a top bar, the screen, a bottom bar of tabs
     landscape  the screen alone; the tabs come in from an edge on a swipe,
                and show by themselves for a few seconds after turning

   Files is one pane, with cut, copy and paste, unless two fit side by side.
   The client always shows this phone and the server: side by side when
   they fit, one above the other when not. The panes, folders, marks and
   scroll positions are the app's own state (g.fm, g.panes), so folding,
   unfolding or turning the phone only changes how they are laid out.
   Fingers arrive as the computer interface's mouse
   (platform/android/backend.cpp): a tap clicks, a drag is the wheel, a long
   press is the right click, which marks. */

namespace mob {

using namespace desk;
using platform::OpenHow;
using platform::OPEN_VIEW;
using platform::OPEN_CHOOSE;
using platform::OPEN_SHARE;

enum Tab { TAB_FILES, TAB_CLIENT, TAB_SHARE, TAB_NEARBY, TAB_SETTINGS, TAB_TRANSFERS, TAB_LOG, TAB_COUNT };
Tab g_tab = TAB_FILES;
Tab g_back_tab = TAB_FILES;  // where Back goes from Transfers or the log
int g_share_view = 0;        // the server, or the web app
int g_queue_tab = 0;         // queued, failed, done
float g_page_scroll[TAB_COUNT];
std::map<const void *, float> g_scroll_of;  // dp, per pane or list

/* This frame's shape. */
bool g_glance = false, g_cover = false, g_landscape = false, g_short = false;

/* Landscape: the tabs, shown for a while after turning or an edge swipe. */
double g_nav_until = 0;
bool g_nav_right = true;
bool g_was_landscape = false;
bool g_was_connected = false;

const float PANE_GOOD = 360, PANE_MIN = 300;

float row_h() { return P(g_short ? 48 : 56); }

/* ---------- small drawings ---------- */

float stroke() { return std::max(P(1.8f), 2 * px()); }

void line(float x0, float y0, float x1, float y1, ImU32 col) {
  draw::dl()->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), col, stroke());
}

void chevron_left(float x, float y, float s, ImU32 col) {
  line(x + s * 0.62f, y + s * 0.2f, x + s * 0.32f, y + s * 0.5f, col);
  line(x + s * 0.32f, y + s * 0.5f, x + s * 0.62f, y + s * 0.8f, col);
}

void check_mark(float x, float y, float s, ImU32 col) {
  line(x + s * 0.2f, y + s * 0.52f, x + s * 0.42f, y + s * 0.72f, col);
  line(x + s * 0.42f, y + s * 0.72f, x + s * 0.8f, y + s * 0.3f, col);
}

void cross(float x, float y, float s, ImU32 col) {
  line(x + s * 0.25f, y + s * 0.25f, x + s * 0.75f, y + s * 0.75f, col);
  line(x + s * 0.75f, y + s * 0.25f, x + s * 0.25f, y + s * 0.75f, col);
}

void dots_v(float x, float y, float s, ImU32 col) {
  for(int i = 0; i < 3; i++) dot(x + s / 2, y + s * (0.22f + 0.28f * (float)i), s * 0.08f, col);
}

void scissors(float x, float y, float s, ImU32 col) {
  draw::dl()->AddCircle(ImVec2(x + s * 0.28f, y + s * 0.72f), s * 0.14f, col, 16, stroke());
  draw::dl()->AddCircle(ImVec2(x + s * 0.72f, y + s * 0.72f), s * 0.14f, col, 16, stroke());
  line(x + s * 0.36f, y + s * 0.6f, x + s * 0.7f, y + s * 0.12f, col);
  line(x + s * 0.64f, y + s * 0.6f, x + s * 0.3f, y + s * 0.12f, col);
}

void trash(float x, float y, float s, ImU32 col) {
  line(x + s * 0.18f, y + s * 0.26f, x + s * 0.82f, y + s * 0.26f, col);
  line(x + s * 0.4f, y + s * 0.26f, x + s * 0.42f, y + s * 0.14f, col);
  line(x + s * 0.42f, y + s * 0.14f, x + s * 0.58f, y + s * 0.14f, col);
  line(x + s * 0.58f, y + s * 0.14f, x + s * 0.6f, y + s * 0.26f, col);
  line(x + s * 0.26f, y + s * 0.26f, x + s * 0.32f, y + s * 0.88f, col);
  line(x + s * 0.32f, y + s * 0.88f, x + s * 0.68f, y + s * 0.88f, col);
  line(x + s * 0.68f, y + s * 0.88f, x + s * 0.74f, y + s * 0.26f, col);
}

void pencil(float x, float y, float s, ImU32 col) {
  line(x + s * 0.2f, y + s * 0.8f, x + s * 0.22f, y + s * 0.62f, col);
  line(x + s * 0.22f, y + s * 0.62f, x + s * 0.66f, y + s * 0.18f, col);
  line(x + s * 0.66f, y + s * 0.18f, x + s * 0.82f, y + s * 0.34f, col);
  line(x + s * 0.82f, y + s * 0.34f, x + s * 0.38f, y + s * 0.78f, col);
  line(x + s * 0.38f, y + s * 0.78f, x + s * 0.2f, y + s * 0.8f, col);
}

void file_glyph(float x, float y, float s, ImU32 col) {
  const float l = x + s * 0.24f, r = x + s * 0.76f, top = y + s * 0.14f, b = y + s * 0.86f, f = s * 0.18f;
  line(l, top, r - f, top, col);
  line(r - f, top, r, top + f, col);
  line(r, top + f, r, b, col);
  line(r, b, l, b, col);
  line(l, b, l, top, col);
}

enum Kind { K_FILE, K_IMAGE, K_VIDEO, K_AUDIO, K_ARCHIVE };

Kind kind_of(const std::string &name) {
  const size_t d = name.rfind('.');
  if(d == std::string::npos) return K_FILE;
  std::string ext = name.substr(d + 1);
  for(char &c : ext) c = (char)tolower((unsigned char)c);
  static const char *const images[] = {"jpg", "jpeg", "png", "gif", "webp", "heic", "heif", "bmp", "dng", "avif"};
  static const char *const videos[] = {"mp4", "mkv", "mov", "avi", "webm", "m4v", "3gp", "ts"};
  static const char *const audio[] = {"mp3", "flac", "ogg", "opus", "m4a", "wav", "aac"};
  static const char *const archives[] = {"zip", "7z", "rar", "tar", "gz", "xz", "bz2", "zst", "apk", "nsp", "xci", "pkg"};
  for(const char *e : images) if(ext == e) return K_IMAGE;
  for(const char *e : videos) if(ext == e) return K_VIDEO;
  for(const char *e : audio) if(ext == e) return K_AUDIO;
  for(const char *e : archives) if(ext == e) return K_ARCHIVE;
  return K_FILE;
}

void entry_icon(const Entry &e, float x, float y, float s) {
  ImU32 tint = IM_COL32(0x20, 0x23, 0x28, 255), ink = SOFT;
  if(e.dir || e.drive) {
    tint = GOLD_TINT;
    ink = GOLD;
  } else {
    switch(kind_of(e.name)) {
    case K_IMAGE: tint = IM_COL32(0x18, 0x23, 0x3A, 255); ink = IM_COL32(0x7D, 0xB2, 0xFF, 255); break;
    case K_VIDEO: tint = IM_COL32(0x26, 0x1C, 0x38, 255); ink = IM_COL32(0xC4, 0xA5, 0xFA, 255); break;
    case K_AUDIO: tint = IM_COL32(0x15, 0x2A, 0x24, 255); ink = IM_COL32(0x6F, 0xD6, 0xB3, 255); break;
    case K_ARCHIVE: tint = IM_COL32(0x2E, 0x21, 0x16, 255); ink = IM_COL32(0xF3, 0xAE, 0x62, 255); break;
    default: break;
    }
  }
  rect(x, y, s, s, tint, s * 0.25f);
  if(e.dir || e.drive) icon(IC_FOLDER, x + s * 0.2f, y + s * 0.2f, s * 0.6f, ink);
  else file_glyph(x + s * 0.18f, y + s * 0.18f, s * 0.64f, ink);
}

/* Text that wraps to w; returns the height it took. */
float para(float x, float y, float w, float size, ImU32 col, const char *t, bool bold = false) {
  float yy = y;
  const float lh = size * 0.72f + P(2);
  for(const std::string &l : wrap_text(t, size, w)) {
    text(x, yy, size, col, l.c_str(), bold);
    yy += lh;
  }
  return yy - y;
}

/* The biggest monospaced size, up to max, at which t fits in w. */
float mono_fit(const char *t, float w, float max) {
  float s = max;
  while(s > F(11) && mono_w(s, t) > w) s *= 0.92f;
  return s;
}

bool over_rect(float x, float y, float w, float h) {
  return g_mouse.inside && g_mouse.x >= x && g_mouse.x < x + w && g_mouse.y >= y && g_mouse.y < y + h;
}

/* A round icon button; true when tapped. */
bool icon_button(float x, float y, float s, const std::function<void(float, float, float)> &glyph, bool on = false) {
  const Ctl c = control(x, y, s, s);
  if(c.hot || on) rect(x, y, s, s, on ? GOLD_TINT : HOVER, s / 2);
  glyph(x + s * 0.25f, y + s * 0.25f, s * 0.5f);
  if(c.focus) focus_ring(x, y, s, s, s / 2);
  return c.hit;
}

/* ---------- motion ---------- */

double g_frame_t = 0;  // when the frame being drawn began (now())
float g_frame_dt = 0;  // seconds since the last one, capped

/* Moves v towards target over `seconds`, at this frame's pace. */
void approach(float &v, float target, float seconds) {
  const float step = g_frame_dt / seconds;
  v = v < target ? std::min(target, v + step) : std::max(target, v - step);
}

float ease_out(float t) {
  const float u = 1 - t;
  return 1 - u * u * u;
}

/* ---------- the sheet ---------- */

/* A long press on a file, or a pane's ⋮: what can be done, in a sheet that
   rises from the bottom over a dimmed screen. */
struct SheetItem {
  std::string label;
  std::function<void()> run;
  bool danger = false;
};
struct Sheet {
  bool open = false;
  std::string title, subtitle;
  std::vector<SheetItem> items;
};
Sheet g_sheet;
float g_sheet_anim = 0;  // 0 down and gone, 1 all the way up

void sheet_open(const std::string &title, const std::string &subtitle, std::vector<SheetItem> items) {
  end_edit();
  g_sheet.open = true;
  g_sheet.title = title;
  g_sheet.subtitle = subtitle;
  g_sheet.items = std::move(items);
}

void sheet_draw() {
  approach(g_sheet_anim, g_sheet.open ? 1.0f : 0.0f, 0.18f);
  if(g_sheet_anim <= 0) return;
  const float t = ease_out(g_sheet_anim);
  rect(0, 0, W, H, IM_COL32(0, 0, 0, (int)(150 * t)));
  const float rh = P(52), head = g_sheet.subtitle.empty() ? P(58) : P(76);
  const float w = std::min(W, P(640)), x = (W - w) / 2;
  const float h = std::min(H * 0.85f, head + rh * (float)g_sheet.items.size() + P(16));
  const float y = H - h * t;
  card(x, y, w, h + P(24), SIDE, BORDER, P(22));
  rect(x + (w - P(36)) / 2, y + P(8), P(36), P(4), IM_COL32(0x4A, 0x4D, 0x55, 255), P(2));
  text(x + P(20), y + P(22), F(15), TEXT, fit_end(g_sheet.title, F(15), w - P(40)).c_str(), true);
  if(!g_sheet.subtitle.empty()) text(x + P(20), y + P(46), F(12), MUTED, fit_end(g_sheet.subtitle, F(12), w - P(40)).c_str());
  float ry = y + head;
  int chosen = -1;
  for(size_t i = 0; i < g_sheet.items.size(); i++, ry += rh) {
    if(ry > H) break;
    const Ctl c = control(x, ry, w, rh, g_sheet.open);
    if(c.hot) rect(x + P(8), ry + P(2), w - P(16), rh - P(4), HOVER, P(10));
    text(x + P(24), ry + (rh - F(15)) / 2, F(15), g_sheet.items[i].danger ? RED : TEXT, g_sheet.items[i].label.c_str());
    if(c.hit) chosen = (int)i;
  }
  if(!g_sheet.open) return;
  if(chosen >= 0) {
    auto run = g_sheet.items[chosen].run;
    g_sheet.open = false;
    run();
  } else if(g_mouse.click && !g_click_taken && !over_rect(x, y, w, h)) {
    g_sheet.open = false;  // a tap on the dimmed screen puts it away
    g_click_taken = true;
  }
}

/* ---------- the panes ---------- */

Pane &files_pane() { return g.fm[g.fm_active]; }

/* The drive a local path is on, as its label ("Internal storage") and the
   rest of the path after it. */
void drive_of(const std::string &path, std::string &label, std::string &rest) {
  localfs_root_t roots[16];
  const int n = localfs_roots(roots, 16);
  size_t best = 0;
  label = tr("Drives");
  rest = path;
  for(int i = 0; i < n; i++) {
    const std::string r = roots[i].path;
    if((path == r || (path.size() > r.size() && path.compare(0, r.size(), r) == 0 && path[r.size()] == '/')) && r.size() > best) {
      best = r.size();
      label = roots[i].label;
      rest = path.substr(r.size());
    }
  }
}

std::string pane_title(Pane &p, std::string *sub) {
  if(p.remote) {
    if(sub) *sub = p.path;
    if(!g.connected) return tr("Connect to a server");
    return g.server_label.empty() ? std::string(g.server.host) : g.server_label;
  }
  if(p.path.empty()) {
    if(sub) sub->clear();
    return tr("Drives");
  }
  std::string label, rest;
  drive_of(p.path, label, rest);
  if(sub) *sub = rest.empty() ? "/" : rest;
  return label;
}

/* The pane the clipboard bar and Back act on. */
Pane &active_pane() {
  if(g_tab == TAB_CLIENT) return g.panes[g.active];
  return files_pane();
}

/* ---------- cut, copy and paste ---------- */

struct Clip {
  bool cut = false, remote = false;
  std::string dir;
  std::vector<Entry> items;
};
Clip g_clip;

std::vector<Entry> marked_of(Pane &p) {
  std::vector<Entry> out;
  for(auto &e : p.entries) if(p.marked.count(e.name) && !e.drive) out.push_back(e);
  return out;
}

void clip_take(Pane &p, bool cut) {
  g_clip = Clip();
  g_clip.cut = cut;
  g_clip.remote = p.remote;
  g_clip.dir = p.path;
  g_clip.items = marked_of(p);
  p.marked.clear();
}

void paste_into(Pane &dst) {
  if(g_clip.items.empty()) return;
  if(dst.path.empty() || (dst.remote && !g.connected)) {
    show_toast(tr("Open a folder to paste into"), true);
    return;
  }
  if(dst.path == g_clip.dir && dst.remote == g_clip.remote) {
    show_toast(tr("They're already in this folder"), true);
    return;
  }
  const int n = (int)g_clip.items.size();
  if(!dst.remote && !g_clip.remote) {
    int added = 0;
    for(auto &e : g_clip.items) if(!fsjob_add(g_clip.cut, join(g_clip.dir, e.name).c_str(), dst.path.c_str())) added++;
    if(added) show_toast(trf(g_clip.cut ? N_("Moving {0} items to {1}") : N_("Copying {0} items to {1}"), {std::to_string(added), dst.path}));
  } else if(dst.remote != g_clip.remote) {
    ftp_server_t s;
    if(!g.connected || !session_server(&s)) return;
    int added = 0;
    for(auto &e : g_clip.items) {
      const std::string from = join(g_clip.dir, e.name), to = join(dst.path, e.name);
      const bool up = !g_clip.remote;
      if(!queue_add(up ? QUEUE_UPLOAD : QUEUE_DOWNLOAD, &s, up ? from.c_str() : to.c_str(), up ? to.c_str() : from.c_str(),
                    e.dir, e.dir ? -1 : e.size, e.mtime))
        added++;
    }
    if(g_clip.cut) show_toast(tr("Moving between this phone and a server copies the files; the originals stay"));
    else show_toast(trf(g_clip.remote ? N_("Downloading {0} items") : N_("Uploading {0} items"), {std::to_string(added)}));
  } else {
    if(!g_clip.cut) {
      show_toast(tr("Copying inside a server isn't possible: download it first"), true);
      return;
    }
    for(auto &e : g_clip.items) {
      const std::string from = join(g_clip.dir, e.name), to = join(dst.path, e.name);
      remote_op([from, to](char *err, size_t len) {
        PathCtx x{from, to, 0, nullptr};
        return session_run(run_rename, &x, err, len);
      }, tr("Done"));
    }
  }
  (void)n;
  if(g_clip.cut) g_clip = Clip();
}

/* Files shows both of its panes this frame (files_view decides). */
bool g_two_files = false;

/* The pane on the other side of p, when it is on screen with a folder
   open that can take what p has: a server connected, not the drive list,
   not the very same folder. Null when there is no such side. */
Pane *other_side(Pane &p) {
  Pane *o = nullptr;
  if(&p == &g.panes[0]) o = &g.panes[1];
  else if(&p == &g.panes[1]) o = &g.panes[0];
  else if(g_two_files) o = &p == &g.fm[0] ? &g.fm[1] : &g.fm[0];
  if(!o || o->path.empty() || (o->remote && !g.connected)) return nullptr;
  if(o->remote == p.remote && o->path == p.path) return nullptr;
  if(o->remote && p.remote) return nullptr;  // nothing copies inside a server
  return o;
}

/* Copies items from one side straight into the other's open folder: a
   paste, without going through the clipboard (which keeps what it had). */
void copy_across(Pane &from, const std::vector<Entry> &items, Pane &to) {
  const Clip kept = g_clip;
  g_clip = Clip();
  g_clip.remote = from.remote;
  g_clip.dir = from.path;
  g_clip.items = items;
  paste_into(to);
  g_clip = kept;
}

/* ---------- a pane ---------- */

struct ListResult {
  int tapped = -1, held = -1;
};

ListResult scroll_list(const void *key, float x, float y, float w, float h, int rows, float rh,
                       const std::function<void(int, float, float, float, float)> &draw_row) {
  ListResult res;
  float &scroll = g_scroll_of[key];
  const bool here = over_rect(x, y, w, h);
  if(here && g_wheel != 0) {
    scroll -= g_wheel * P(40);
    g_wheel = 0;
  }
  scroll = std::max(0.0f, std::min(scroll, std::max(0.0f, rh * (float)rows - h)));
  draw::dl()->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);
  for(int i = std::max(0, (int)(scroll / rh)); i < rows; i++) {
    const float ry = y + rh * (float)i - scroll;
    if(ry > y + h) break;
    const bool hot = here && g_mouse.y >= std::max(ry, y) && g_mouse.y < std::min(ry + rh, y + h);
    draw_row(i, x, ry, w, rh);
    if(hot && g_mouse.click && !g_click_taken) {
      res.tapped = i;
      g_click_taken = true;
    }
    if(hot && g_mouse.right) res.held = i;
  }
  draw::dl()->PopClipRect();
  return res;
}

void toggle_mark(Pane &p, int i) {
  if(i < 0 || i >= (int)p.entries.size() || p.entries[i].drive) return;
  const std::string &n = p.entries[i].name;
  if(p.marked.count(n)) p.marked.erase(n);
  else p.marked.insert(n);
}

void up(Pane &p) {
  g_scroll_of[&p] = 0;
  go_up(p);
}

/* The orders Sort by offers, each a field and a direction. */
struct SortChoice {
  const char *label;
  int by;
  bool desc;
};
const SortChoice kSortChoices[] = {
    {N_("Name (A to Z)"), SORT_NAME, false}, {N_("Name (Z to A)"), SORT_NAME, true},
    {N_("Newest first"), SORT_DATE, true},   {N_("Oldest first"), SORT_DATE, false},
    {N_("Largest first"), SORT_SIZE, true},  {N_("Smallest first"), SORT_SIZE, false},
    {N_("Type"), SORT_TYPE, false},
};

const char *sort_label() {
  for(const SortChoice &c : kSortChoices) {
    if(c.by == g_sort_by && c.desc == g_sort_desc) return tr(c.label);
  }
  return tr("Name (A to Z)");
}

/* Every open folder, put in the new order at once; what was marked stays
   marked, and the cursor stays on the same file. */
void resort_all() {
  for(Pane *p : {&g.fm[0], &g.fm[1], &g.panes[0], &g.panes[1]}) {
    if(p->path.empty()) continue;  // drives, or servers: in their own order
    const std::string at = p->cursor < (int)p->entries.size() ? p->entries[p->cursor].name : "";
    sort_entries(p->entries);
    for(int i = 0; i < (int)p->entries.size(); i++) {
      if(p->entries[i].name == at) p->cursor = i;
    }
  }
}

void sort_menu() {
  std::vector<SheetItem> items;
  for(const SortChoice &c : kSortChoices) {
    const int by = c.by;
    const bool desc = c.desc;
    items.push_back({tr(c.label), [by, desc] {
      g_sort_by = by;
      g_sort_desc = desc;
      save_prefs();
      resort_all();
    }});
  }
  sheet_open(tr("Sort by"), trf("Now: {0}", {sort_label()}), std::move(items));
}

void pane_menu(Pane &p) {
  Pane *pp = &p;
  std::vector<SheetItem> items;
  if(!p.path.empty()) {
    items.push_back({trf("Sort: {0}", {sort_label()}), [] { sort_menu(); }});
    items.push_back({tr("New folder"), [pp] { make_folder(*pp); }});
    items.push_back({tr("Mark all"), [pp] { for(auto &e : pp->entries) if(!e.drive) pp->marked.insert(e.name); }});
    items.push_back({tr("Refresh"), [pp] { refresh(*pp); }});
  }
  if(p.remote) {
    if(g.connected) items.push_back({tr("Disconnect"), [] { disconnect(); }, true});
  } else {
    items.push_back({tr("Drives"), [pp] {
      g_scroll_of[pp] = 0;
      load_drives(*pp);
    }});
  }
  std::string sub;
  const std::string title = pane_title(p, &sub);
  sheet_open(title, sub, std::move(items));
}

void open_local_file(Pane &p, const Entry &e, OpenHow how) {
  if(!platform::open_file(join(p.path, e.name).c_str(), how)) show_toast(tr("No app on this phone opens this kind of file"), true);
}

/* A long press on one item: everything that can be done with it. */
void entry_sheet(Pane &p, int i) {
  if(i < 0 || i >= (int)p.entries.size()) return;
  const Entry e = p.entries[i];
  if(e.drive) return;
  Pane *pp = &p;
  std::vector<SheetItem> items;
  const bool file = !e.dir;
  if(file && !p.remote) {
    items.push_back({tr("Open"), [pp, e] { open_local_file(*pp, e, OPEN_VIEW); }});
    items.push_back({tr("Open with…"), [pp, e] { open_local_file(*pp, e, OPEN_CHOOSE); }});
    items.push_back({tr("Share"), [pp, e] { open_local_file(*pp, e, OPEN_SHARE); }});
  } else if(!file) {
    items.push_back({tr("Open"), [pp, i] {
      pp->cursor = i;
      g_scroll_of[pp] = 0;
      open_entry(*pp);
    }});
  }
  if(Pane *o = other_side(p)) items.push_back({tr("Copy to the other side"), [pp, o, e] { copy_across(*pp, {e}, *o); }});
  items.push_back({tr("Select"), [pp, e] { pp->marked.insert(e.name); }});
  auto take = [pp, e](bool cut) {
    pp->marked.clear();
    pp->marked.insert(e.name);
    clip_take(*pp, cut);
  };
  items.push_back({tr("Cut"), [take] { take(true); }});
  items.push_back({tr("Copy"), [take] { take(false); }});
  items.push_back({tr("Rename"), [pp, e] { rename_item(*pp, e); }});
  items.push_back({tr("Delete"), [pp, e] {
    pp->marked.clear();
    pp->marked.insert(e.name);
    delete_items(*pp);
  }, true});
  std::string meta = e.dir ? (e.mtime >= 0 ? fmt_date(e.mtime) : "")
                           : fmt_size(e.size) + (e.mtime >= 0 ? "  \xc2\xb7  " + fmt_date(e.mtime) : "");
  sheet_open(e.name, meta, std::move(items));
}

json::Object g_new_site;
bool g_new_site_init = false;

/* The server side while nothing is connected: saved and recent servers,
   and a form for a new one. */
void draw_connect(float x, float y, float w, float h) {
  if(!g_new_site_init) {
    json::set_str(g_new_site, "protocol", "sftp");
    json::set_num(g_new_site, "port", 22);
    g_new_site_init = true;
  }
  float &scroll = g_scroll_of[&g_new_site];
  static float content_h = 0;  // last frame's: the scroll is held inside it before drawing (page_view says why)
  if(over_rect(x, y, w, h) && g_wheel != 0) {
    scroll -= g_wheel * P(40);
    g_wheel = 0;
  }
  scroll = std::max(0.0f, std::min(scroll, std::max(0.0f, content_h - h)));
  draw::dl()->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);
  const float pad = P(14), iw = w - 2 * pad;
  float yy = y + P(14) - scroll;
  text(x + pad, yy, F(16), TEXT, tr("Connect to a server"), true);
  yy += P(34);

  auto site_row = [&](const json::Object &s, bool saved) {
    const float rh = P(60);
    card(x + pad, yy, iw, rh, saved ? CARD : BG, LINE, P(12));
    const std::string name = json::get_str(s, "name");
    const char *c = tr("Connect");
    const float bw = button_w(c, false), tw = iw - bw - P(36);
    text(x + pad + P(12), yy + P(10), F(14), TEXT, fit_end(name.empty() ? json::get_str(s, "host") : name, F(14), tw).c_str(), true);
    text(x + pad + P(12), yy + P(33), F(11), MUTED, fit_end(site_label(s), F(11), tw).c_str());
    const bool hit = button(x + pad + iw - P(10) - bw, yy + (rh - P(38)) / 2, c, saved ? PRIMARY : SECONDARY, -1, true, P(38));
    yy += rh + P(8);
    return hit;
  };
  if(!g.sites.empty()) {
    text(x + pad, yy, F(11), MUTED, tr("SAVED"), true);
    yy += P(22);
    for(auto &s : g.sites) if(site_row(s, true)) connect(s);
  }
  if(!g.history.empty()) {
    text(x + pad, yy, F(11), MUTED, tr("RECENT"), true);
    yy += P(22);
    for(auto &s : g.history) if(site_row(s, false)) connect(s);
  }
  text(x + pad, yy, F(11), MUTED, tr("NEW CONNECTION"), true);
  yy += P(22);
  const char *protos[2] = {"SFTP", "FTP"};
  const bool sftp = json::get_str(g_new_site, "protocol", "sftp") == "sftp";
  const int pick = segmented(x + pad, yy, protos, 2, sftp ? 0 : 1);
  if(pick != (sftp ? 0 : 1)) {
    json::set_str(g_new_site, "protocol", pick == 0 ? "sftp" : "ftp");
    json::set_num(g_new_site, "port", pick == 0 ? 22 : 21);
  }
  yy += P(50);
  auto labelled = [&](const char *key, const char *label, float lx, float lw, const char *jkey, bool secret, bool num) {
    text(lx, yy, F(11), MUTED, label);
    std::string v = num ? std::to_string((int)json::get_num(g_new_site, jkey, 0)) : json::get_str(g_new_site, jkey);
    if(field(key, lx, yy + P(18), lw, v, true, secret)) {
      if(num) json::set_num(g_new_site, jkey, atoi(v.c_str()));
      else json::set_str(g_new_site, jkey, v);
    }
  };
  const float pw = P(80), hw = iw - pw - P(8);
  labelled("m-host", tr("Host"), x + pad, hw, "host", false, false);
  labelled("m-port", tr("Port"), x + pad + hw + P(8), pw, "port", false, true);
  yy += P(64);
  if(iw >= P(320)) {
    const float half = (iw - P(8)) / 2;
    labelled("m-user", tr("Username"), x + pad, half, "user", false, false);
    labelled("m-pass", tr("Password"), x + pad + half + P(8), half, "pass", true, false);
    yy += P(64);
  } else {
    labelled("m-user", tr("Username"), x + pad, iw, "user", false, false);
    yy += P(64);
    labelled("m-pass", tr("Password"), x + pad, iw, "pass", true, false);
    yy += P(64);
  }
  if(button(x + pad, yy, tr("Connect"), PRIMARY, -1, true, P(46), iw)) {
    end_edit();
    connect(g_new_site);
  }
  yy += P(60);
  content_h = yy + scroll - y;
  draw::dl()->PopClipRect();
}

/* One pane: Up, the drive or server and the path, a menu, then the list.
   on_select is where the pane's own activation goes (the client's side). */
void draw_pane_m(Pane &p, float x, float y, float w, float h, bool active, const std::function<void()> &on_select) {
  card(x, y, w, h, IM_COL32(0x12, 0x14, 0x17, 255), active ? GOLD : LINE, P(14));
  if(g_mouse.click && over_rect(x, y, w, h)) on_select();
  if(p.remote && !g.connected) {
    draw_connect(x + px(), y + px(), w - 2 * px(), h - 2 * px());
    return;
  }
  if(!p.remote && p.path.empty() && p.entries.empty()) load_drives(p);
  refresh_free_space(p);

  // Header: Up, title and path, menu.
  const float hh = P(g_short ? 46 : 54), bs = P(40);
  float hx = x + P(6);
  const bool can_up = !p.path.empty() && !(p.remote && p.path == "/");
  if(can_up) {
    if(icon_button(hx, y + (hh - bs) / 2, bs, [](float gx, float gy, float s) { chevron_left(gx, gy, s, TEXT); })) up(p);
    hx += bs + P(2);
  } else {
    hx += P(8);
  }
  const float mx = x + w - P(6) - bs;
  if(icon_button(mx, y + (hh - bs) / 2, bs, [](float gx, float gy, float s) { dots_v(gx, gy, s, SOFT); })) pane_menu(p);
  std::string sub;
  const std::string title = pane_title(p, &sub);
  const float tw = mx - P(4) - hx;
  if(sub.empty()) {
    text(hx, y + (hh - F(14)) / 2, F(14), TEXT, fit_end(title, F(14), tw).c_str(), true);
  } else {
    text(hx, y + P(g_short ? 5 : 8), F(14), TEXT, fit_end(title, F(14), tw).c_str(), true);
    mono(hx, y + P(g_short ? 25 : 30), F(11), MUTED, fit(sub, F(11), tw, true).c_str());
  }
  hline(x + px(), y + hh, w - 2 * px());

  // Footer.
  const float fh = P(26);
  std::string foot;
  if(p.loading) foot = tr("Loading...");
  else if(!p.path.empty()) {
    foot = trf("{0} items", {std::to_string(p.entries.size())});
    if(!p.marked.empty()) foot += "  \xc2\xb7  " + trf("{0} marked", {std::to_string(p.marked.size())});
    if(!p.remote && p.free_bytes >= 0) foot += "  \xc2\xb7  " + trf("{0} free", {fmt_size(p.free_bytes)});
  }
  text(x + P(14), y + h - fh + (fh - F(11)) / 2, F(11), FAINT, fit_end(foot, F(11), w - P(28)).c_str());

  // The list.
  const float ly = y + hh + px(), lh = h - hh - fh - px();
  if(p.loading && p.entries.empty()) {
    draw::spinner(x + w / 2, ly + P(40), P(9), GOLD);
    return;
  }
  if(p.entries.empty()) {
    para(x + P(16), ly + P(16), w - P(32), F(13), MUTED, p.error.empty() ? tr("Empty folder") : p.error.c_str());
    return;
  }
  const float rh = row_h(), is = P(g_short ? 32 : 38);
  const bool marking = !p.marked.empty();
  ListResult r = scroll_list(&p, x + px(), ly, w - 2 * px(), lh, (int)p.entries.size(), rh,
                             [&](int i, float rx, float ry, float rw, float rhh) {
    const Entry &e = p.entries[i];
    const bool on = p.marked.count(e.name) > 0;
    if(on) rect(rx + P(6), ry + P(2), rw - P(12), rhh - P(4), IM_COL32(0x1F, 0x1C, 0x12, 255), P(10));
    float cx = rx + P(12);
    if(marking && !e.drive) {
      const float b = P(22);
      if(on) {
        rect(cx, ry + (rhh - b) / 2, b, b, GOLD, P(6));
        check_mark(cx, ry + (rhh - b) / 2, b, INK);
      } else {
        draw::frame(cx, ry + (rhh - b) / 2, b, b, IM_COL32(0x4A, 0x4D, 0x55, 255), P(6), std::max(P(2), 2 * px()));
      }
      cx += b + P(12);
    }
    entry_icon(e, cx, ry + (rhh - is) / 2, is);
    const float nx = cx + is + P(12), nw = rx + rw - P(12) - nx;
    const std::string name = e.drive ? (e.detail.empty() ? e.name : e.detail) : e.name;
    std::string meta;
    if(e.drive) meta = e.size >= 0 ? trf("{0} free", {fmt_size(e.size)}) : e.name;
    else if(e.dir) meta = e.mtime >= 0 ? fmt_date(e.mtime) : "";
    else meta = fmt_size(e.size) + (e.mtime >= 0 ? "  \xc2\xb7  " + fmt_date(e.mtime) : "");
    text(nx, ry + rhh / 2 - F(14) + P(1), F(14), on ? GOLD_LIGHT : TEXT, fit_end(name, F(14), nw).c_str());
    text(nx, ry + rhh / 2 + P(3), F(11), MUTED, fit_end(meta, F(11), nw).c_str());
  });
  /* A tap opens: a folder here, a file in the app that opens its kind. A
     long press offers everything else. While marking, both mark. */
  if(r.held >= 0) {
    on_select();
    if(marking) toggle_mark(p, r.held);
    else entry_sheet(p, r.held);
  } else if(r.tapped >= 0) {
    on_select();
    const Entry e = p.entries[r.tapped];
    if(marking && !e.drive) {
      toggle_mark(p, r.tapped);
    } else if(e.dir || e.drive) {
      p.cursor = r.tapped;
      g_scroll_of[&p] = 0;
      open_entry(p);
    } else if(!p.remote) {
      open_local_file(p, e, OPEN_VIEW);
    } else {
      entry_sheet(p, r.tapped);  // a server's file: what can be done with it
    }
  }
}

/* ---------- the bar under the panes ---------- */

/* What can be done with the marked items, as a row of icon buttons; or,
   with something cut or copied, the paste bar. Returns its height. */
float action_bar(float x, float bottom, float w) {
  Pane &p = active_pane();
  const int marked = (int)p.marked.size();
  if(marked > 0) {
    struct Act {
      const char *label;
      std::function<void(float, float, float)> glyph;
      std::function<void()> run;
    };
    Pane *pp = &p;
    std::vector<Act> acts;
    /* In the client, the marked items straight to the other side. */
    if(g_tab == TAB_CLIENT) {
      if(Pane *o = other_side(p)) {
        const bool down = p.remote;
        acts.push_back({down ? tr("Download") : tr("Upload"), [down](float gx, float gy, float s) {
          const ImU32 c = GOLD_LIGHT;
          line(gx + s * 0.5f, gy + s * 0.12f, gx + s * 0.5f, gy + s * 0.78f, c);
          if(down) {
            line(gx + s * 0.24f, gy + s * 0.52f, gx + s * 0.5f, gy + s * 0.78f, c);
            line(gx + s * 0.76f, gy + s * 0.52f, gx + s * 0.5f, gy + s * 0.78f, c);
          } else {
            line(gx + s * 0.24f, gy + s * 0.38f, gx + s * 0.5f, gy + s * 0.12f, c);
            line(gx + s * 0.76f, gy + s * 0.38f, gx + s * 0.5f, gy + s * 0.12f, c);
          }
          line(gx + s * 0.18f, gy + s * 0.92f, gx + s * 0.82f, gy + s * 0.92f, c);
        }, [pp, o] {
          copy_across(*pp, marked_of(*pp), *o);
          pp->marked.clear();
        }});
      }
    }
    acts.push_back({tr("Cut"), [](float gx, float gy, float s) { scissors(gx, gy, s, TEXT); }, [pp] { clip_take(*pp, true); }});
    acts.push_back({tr("Copy"), [](float gx, float gy, float s) { icon(IC_COPY, gx, gy, s, TEXT); }, [pp] { clip_take(*pp, false); }});
    if(marked == 1) {
      acts.push_back({tr("Rename"), [](float gx, float gy, float s) { pencil(gx, gy, s, TEXT); }, [pp] {
        for(auto &e : pp->entries) if(pp->marked.count(e.name)) {
          Entry copy = e;
          pp->marked.clear();
          rename_item(*pp, copy);
          break;
        }
      }});
    }
    acts.push_back({tr("Delete"), [](float gx, float gy, float s) { trash(gx, gy, s, RED); }, [pp] { delete_items(*pp); }});
    acts.push_back({tr("Mark all"), [](float gx, float gy, float s) { check_mark(gx, gy, s, TEXT); },
                    [pp] { for(auto &e : pp->entries) if(!e.drive) pp->marked.insert(e.name); }});
    acts.push_back({tr("Close"), [](float gx, float gy, float s) { cross(gx, gy, s, MUTED); }, [pp] { pp->marked.clear(); }});
    const float h = P(64), y = bottom - h;
    card(x, y, w, h, RAISED, BORDER, P(14));
    const float cw = w / (float)acts.size();
    for(size_t i = 0; i < acts.size(); i++) {
      const float cx = x + cw * (float)i;
      const Ctl c = control(cx, y, cw, h);
      if(c.hot) rect(cx + P(4), y + P(4), cw - P(8), h - P(8), HOVER, P(10));
      acts[i].glyph(cx + (cw - P(22)) / 2, y + P(10), P(22));
      const std::string l = fit_end(acts[i].label, F(11), cw - P(6));
      text(cx + (cw - text_w(F(11), l.c_str())) / 2, y + P(38), F(11), SOFT, l.c_str());
      if(c.hit) acts[i].run();
    }
    return h + P(8);
  }
  if(!g_clip.items.empty()) {
    const float h = P(60), y = bottom - h;
    card(x, y, w, h, RAISED, BORDER, P(14));
    const char *paste = tr("Paste"), *cancel = tr("Cancel");
    const float pw = button_w(paste, false), cw = button_w(cancel, false);
    const float tx = x + P(14), tw = w - pw - cw - P(48);
    const std::string what = trf("{0} items ready to paste", {std::to_string(g_clip.items.size())});
    text(tx, y + P(11), F(13), TEXT, fit_end(what, F(13), tw).c_str(), true);
    mono(tx, y + P(33), F(10), MUTED, fit(g_clip.dir, F(10), tw, true).c_str());
    if(button(x + w - P(12) - pw - P(8) - cw, y + (h - P(40)) / 2, cancel, QUIET, -1, true, P(40))) g_clip = Clip();
    if(button(x + w - P(12) - pw, y + (h - P(40)) / 2, paste, PRIMARY, -1, !p.path.empty(), P(40))) paste_into(p);
    return h + P(8);
  }
  return 0;
}

/* ---------- Files and Client ---------- */

void files_view(float x, float y, float w, float h) {
  const float gap = P(10);
  const float bar = action_bar(x, y + h, w);
  const float ah = h - bar;
  g_two_files = w - gap >= 2 * P(PANE_GOOD);
  if(g_two_files) {
    const float half = (w - gap) / 2;
    draw_pane_m(g.fm[0], x, y, half, ah, g.fm_active == 0, [] { g.fm_active = 0; });
    draw_pane_m(g.fm[1], x + half + gap, y, half, ah, g.fm_active == 1, [] { g.fm_active = 1; });
  } else {
    g.fm_active = 0;  // one pane: the first
    draw_pane_m(g.fm[0], x, y, w, ah, false, [] {});
  }
}

void client_view(float x, float y, float w, float h) {
  const float gap = P(10);
  const float bar = action_bar(x, y + h, w);
  const float ah = h - bar;
  if(g.panes[0].path.empty() && g.panes[0].entries.empty()) load_drives(g.panes[0]);
  if((w - gap) / 2 >= P(PANE_MIN) && (g_landscape || w >= P(600))) {
    const float half = (w - gap) / 2;
    draw_pane_m(g.panes[0], x, y, half, ah, g.active == 0, [] { g.active = 0; });
    draw_pane_m(g.panes[1], x + half + gap, y, half, ah, g.active == 1, [] { g.active = 1; });
  } else {
    // This phone above, the server below.
    const float half = (ah - gap) / 2;
    draw_pane_m(g.panes[0], x, y, w, half, g.active == 0, [] { g.active = 0; });
    draw_pane_m(g.panes[1], x, y + half + gap, w, half, g.active == 1, [] { g.active = 1; });
  }
}

/* ---------- pages ---------- */

/* A page's title, with a switch on the right when it has one. */
bool page_title(float x, float &y, float w, const char *title, int state /* -1 none, 0 off, 1 on */) {
  bool hit = false;
  const float sw = state >= 0 ? P(52) + P(12) : 0;
  text(x, y, F(22), TEXT, fit_end(title, F(22), w - sw).c_str(), true);
  if(state >= 0) {
    hit = toggle(x + w - P(52), y + P(2), state == 1);
    const char *st = state ? tr("Running") : tr("Stopped");
    dot(x + P(4), y + P(46), P(4), state ? GREEN : FAINT);
    text(x + P(14), y + P(38), F(12), state ? GREEN : MUTED, st, true);
    y += P(64);
  } else {
    y += P(44);
  }
  return hit;
}

/* An address in a card: as big as fits, with its buttons under it. */
void address_card(float x, float &y, float w, const std::string &addr, bool live, const char *note, bool open_button) {
  const float pad = P(16), iw = w - 2 * pad;
  const float size = mono_fit(addr.c_str(), iw, F(24));
  const float h = P(18) + size + (note ? P(30) : P(8)) + P(56);
  card(x, y, w, h);
  float yy = y + P(18);
  mono(x + pad, yy, size, live ? TEXT : MUTED, addr.c_str());
  yy += size + P(6);
  if(note) {
    text(x + pad, yy, F(13), GOLD, fit_end(note, F(13), iw).c_str());
    yy += P(24);
  }
  const char *cp = tr("Copy"), *open = tr("Open in browser");
  if(open_button) {
    const float bw = (iw - P(8)) / 2;
    if(button(x + pad, yy, open, PRIMARY, IC_OPEN, live, P(40), bw)) platform::open_url(addr.c_str());
    if(button(x + pad + bw + P(8), yy, cp, SECONDARY, IC_COPY, true, P(40), bw)) copy_address(addr);
  } else {
    if(button(x + pad, yy, cp, SECONDARY, IC_COPY, true, P(40), iw)) copy_address(addr);
  }
  y += h + P(16);
}

float page_server_m(float x, float y, float w) {
  const bool running = server_running(), sftp = server_is_sftp();
  if(page_title(x, y, w, CHIRAL_HAS_SFTPD ? tr("FTP / SFTP server") : tr("FTP server"), running ? 1 : 0)) {
    if(running) stop_server();
    else start_server();
  }
  y += para(x, y, w, F(13), MUTED,
            CHIRAL_HAS_SFTPD ? tr("Other devices connect to this phone with FileZilla, Cyberduck, a file manager or any SFTP or FTP app.")
                             : tr("Other devices connect to this phone with FileZilla, a file manager or any FTP app.")) + P(16);
  const float pad = P(16), iw = w - 2 * pad;
#if CHIRAL_HAS_SFTPD
  // Which server: SFTP (encrypted) or FTP, chosen while it's off.
  {
    const char *protos[2] = {"SFTP", "FTP"};
    const int pick = segmented(x, y, protos, 2, sftp ? 0 : 1, !running);
    if(pick != (sftp ? 0 : 1)) {
      g_srv.sftp = pick == 0;
      save_server_prefs();
    }
    const char *note = running ? tr("Turn the server off to change it.")
                       : sftp  ? tr("Encrypted: for most apps.")
                               : tr("Not encrypted: for apps that only speak FTP.");
    y += P(38) + P(8);
    y += para(x, y, w, F(12), MUTED, note) + P(12);
  }
#endif
  const std::string login = g_srv.ftp.anonymous && !sftp ? std::string() : trf("Login: {0} / {1}", {g_srv.ftp.user, g_srv.ftp.pass});
  address_card(x, y, w, server_address(), running, login.empty() ? nullptr : login.c_str(), false);
#if CHIRAL_HAS_SFTPD
  // The server's key, for checking the fingerprint a client shows the first time.
  if(sftp) {
    char fp[128] = "";
    sftpd_fingerprint(fp, sizeof(fp));
    const float kh = P(44);
    rect(x, y, w, kh, CARD, P(10));
    icon(IC_KEY, x + pad, y + (kh - P(16)) / 2, P(16), GOLD);
    if(fp[0]) {
      const char *label = tr("Key");
      text(x + pad + P(26), y + (kh - F(12)) / 2, F(12), MUTED, label);
      const float kx = x + pad + P(26) + text_w(F(12), label) + P(8);
      mono(kx, y + (kh - F(12)) / 2, F(12), TEXT, fit(fp, F(12), x + w - pad - kx, true).c_str());
    } else {
      text(x + pad + P(26), y + (kh - F(12)) / 2, F(12), MUTED,
           fit(tr("The key shows here once the server has started."), F(12), iw - P(26), false).c_str());
    }
    y += kh + P(16);
  }
#endif

  // Login, folder and port.
  float yy = y;
  const float fh = P(64);
  float h = fh * 2 + (sftp ? 0 : P(56)) + P(118) + P(64) + P(8);
  card(x, y, w, h);
  yy += P(10);
  text(x + pad, yy, F(12), MUTED, tr("Username"));
  std::string user = g_srv.ftp.user;
  if(field("srv-user", x + pad, yy + P(18), iw, user)) {
    if(user.empty()) show_toast(tr("The username can't be empty"), true);
    else {
      snprintf(g_srv.ftp.user, sizeof(g_srv.ftp.user), "%s", user.c_str());
      save_server_prefs();
    }
  }
  yy += fh;
  text(x + pad, yy, F(12), MUTED, tr("Password"));
  std::string pass = g_srv.ftp.pass;
  if(field("srv-pass", x + pad, yy + P(18), iw, pass, true, true)) {
    if(pass.empty()) show_toast(tr("The password can't be empty; turn on anonymous login instead"), true);
    else {
      snprintf(g_srv.ftp.pass, sizeof(g_srv.ftp.pass), "%s", pass.c_str());
      save_server_prefs();
    }
  }
  yy += fh;
  if(!sftp) {  // SFTP always asks for the login
    hline(x + pad, yy, iw);
    text(x + pad, yy + (P(56) - F(14)) / 2, F(14), TEXT, tr("Anonymous login"));
    if(toggle(x + w - pad - P(52), yy + (P(56) - P(30)) / 2, g_srv.ftp.anonymous)) {
      g_srv.ftp.anonymous = !g_srv.ftp.anonymous;
      save_server_prefs();
    }
    yy += P(56);
  }
  hline(x + pad, yy, iw);
  text(x + pad, yy + P(12), F(14), TEXT, tr("Shared folder"));
  icon(IC_FOLDER, x + pad, yy + P(40), P(16), GOLD);
  mono(x + pad + P(22), yy + P(40), F(12), SOFT, fit(g_srv.ftp.root, F(12), iw - P(22), true).c_str());
  {
    const float bw = (iw - P(8)) / 2, by = yy + P(66);
    if(button(x + pad, by, tr("Choose…"), SECONDARY, -1, true, P(38), bw)) {
      end_edit();
      prompt(tr("Shared folder"), g_srv.ftp.root, TEXT_URL, [](std::string v) {
        struct stat st;
        if(v.empty() || stat(v.c_str(), &st) || !S_ISDIR(st.st_mode)) return show_toast(trf("Not a folder: {0}", {v}), true);
        set_shared_root(v);
        show_toast(trf("Sharing {0}", {v}));
      });
    }
    if(button(x + pad + bw + P(8), by, tr("All storage"), SECONDARY, -1, true, P(38), bw)) {
      set_shared_root(system_ftpd_full_root());
      show_toast(trf("Sharing {0}", {system_ftpd_full_root()}));
    }
  }
  yy += P(118);
  hline(x + pad, yy, iw);
  text(x + pad, yy + (P(64) - F(14)) / 2, F(14), running ? MUTED : TEXT, tr("Port"));
  std::string port = std::to_string(server_port());
  if(field(sftp ? "srv-sftp-port" : "srv-ftp-port", x + w - pad - P(96), yy + (P(64) - P(36)) / 2, P(96), port, !running, false, true, true)) {
    const int p = atoi(port.c_str());
    if(p < 1 || p > 65535) show_toast(tr("A port is a number from 1 to 65535"), true);
    else {
#if CHIRAL_HAS_SFTPD
      if(sftp) g_srv.sftp_port = p;
      else
#endif
        g_srv.ftp.port = (unsigned short)p;
      save_server_prefs();
    }
  }
  y += h + P(16);

  // What is happening.
  ftpd_stats_t st;
  server_stats(&st);
  char lines[12][160];
  const int n = server_recent(lines, 12);
  const float lh = P(22);
  const float ah = P(118) + std::max(P(24), lh * (float)n);
  card(x, y, w, ah);
  text(x + pad, y + P(14), F(15), TEXT, tr("Activity"), true);
  const float tw = (iw - P(8)) / 2;
  rect(x + pad, y + P(44), tw, P(52), BG, P(10));
  text(x + pad + P(12), y + P(50), F(18), TEXT, std::to_string(st.clients).c_str(), true);
  text(x + pad + P(12), y + P(76), F(11), MUTED, tr("connected"));
  rect(x + pad + tw + P(8), y + P(44), tw, P(52), BG, P(10));
  text(x + pad + tw + P(20), y + P(50), F(18), TEXT, fmt_size(st.bytes_up).c_str(), true);
  text(x + pad + tw + P(20), y + P(76), F(11), MUTED, fit_end(trf("sent · {0} received", {fmt_size(st.bytes_down)}), F(11), tw - P(24)).c_str());
  float ry = y + P(108);
  if(!n) text(x + pad, ry, F(12), MUTED, tr("Logins and transfers show here."));
  for(int i = n - 1; i >= 0; i--, ry += lh) mono(x + pad, ry, F(11), SOFT, fit(lines[i], F(11), iw, true).c_str());
  y += ah + P(16);
  return y;
}

float page_web_m(float x, float y, float w) {
  const bool running = websrv_running(), local = web_local();
  if(page_title(x, y, w, tr("Web app"), running ? 1 : 0)) {
    if(running) websrv_stop();
    else start_web(false);
  }
  y += para(x, y, w, F(13), MUTED, tr("Use chiral-ftp from a browser on a phone, a tablet or another computer.")) + P(16);
  address_card(x, y, w, web_address(), running, nullptr, true);

  const float pad = P(16), iw = w - 2 * pad;
  const char *reach[2] = {tr("This phone"), tr("Home network")};
  const float sh = P(38);
  const float note_h = P(40);
  const float h = P(14) + P(24) + note_h + sh + P(16) + P(64);
  card(x, y, w, h);
  float yy = y + P(14);
  text(x + pad, yy, F(14), TEXT, tr("Reachable from"));
  yy += P(24);
  yy += std::max(note_h, para(x + pad, yy, iw, F(12), MUTED,
                              running ? tr("Turn the web app off to change it.")
                                      : local ? tr("Only browsers on this computer can open it.")
                                              : tr("Phones and other devices on your network can open it too.")));
  const int pick = segmented(x + pad, yy, reach, 2, local ? 0 : 1, !running);
  if(pick != (local ? 0 : 1)) {
    g_srv.web_local = pick == 0;
    save_server_prefs();
  }
  yy += sh + P(16);
  hline(x + pad, yy, iw);
  text(x + pad, yy + (P(64) - F(14)) / 2, F(14), running ? MUTED : TEXT, tr("Port"));
  std::string port = std::to_string(g_srv.web_port);
  if(field("web-port", x + w - pad - P(96), yy + (P(64) - P(36)) / 2, P(96), port, !running, false, true, true)) {
    const int p = atoi(port.c_str());
    if(p < 1 || p > 65535) show_toast(tr("A port is a number from 1 to 65535"), true);
    else {
      g_srv.web_port = p;
      save_server_prefs();
    }
  }
  y += h + P(16);
  return y;
}

float page_quick_m(float x, float y, float w) {
  const char *refresh_l = tr("Refresh");
  const float rw = button_w(refresh_l, false);
  text(x, y, F(22), TEXT, fit_end(tr("Quick connect"), F(22), w - rw - P(12)).c_str(), true);
  if(button(x + w - rw, y - P(4), refresh_l, SECONDARY, -1, true, P(38))) discovery_probe();
  y += P(44);
  y += para(x, y, w, F(13), MUTED, tr("chiral-ftp on other devices on this network. Ask one to share, then browse it here.")) + P(16);

  discovery_outcome_t o;
  discovery_get_outcome(&o);
  if(o.state == DISCOVERY_ASKING) {
    const float pad = P(16), iw = w - 2 * pad;
    const float ch = P(250);
    card(x, y, w, ch);
    text(x + pad, y + P(16), F(17), TEXT, fit_end(trf("Asking {0}...", {o.name}), F(17), iw).c_str(), true);
    para(x + pad, y + P(46), iw, F(12), MUTED, trf("Accept on {0} to connect. Check that it shows the same code:", {o.name}).c_str());
    const float bs = std::min(P(56), (iw - 3 * P(10)) / 4);
    for(int i = 0; i < 4 && o.code[i]; i++) {
      const float bx = x + pad + i * (bs + P(10));
      rect(bx, y + P(96), bs, bs * 1.2f, BG, P(10));
      char d[2] = {o.code[i], 0};
      mono(bx + (bs - mono_w(F(28), d)) / 2, y + P(96) + (bs * 1.2f - F(28)) / 2, F(28), GOLD, d);
    }
    draw::spinner(x + pad + P(8), y + P(186), P(7), GOLD);
    text(x + pad + P(24), y + P(178), F(12), MUTED,
         fit_end(trf("Waiting · {0} · gives up in {1} s", {o.ip, std::to_string(o.seconds_left)}), F(12), iw - P(24)).c_str());
    if(button(x + pad, y + ch - P(52), tr("Cancel"), SECONDARY, -1, true, P(40), iw)) discovery_cancel();
    return y + ch + P(16);
  }
  discovery_peer_t peers[DISCOVERY_MAX_PEERS];
  const int n = discovery_peers(peers, DISCOVERY_MAX_PEERS);
  if(!n) {
    const float ch = P(96);
    card(x, y, w, ch);
    if(!discovery_running() && discovery_problem()[0]) {
      para(x + P(16), y + P(18), w - P(32), F(13), RED, tr("Quick connect isn't available here: the network refused it (see the message log)."));
    } else {
      draw::spinner(x + P(26), y + P(30), P(8), GOLD);
      text(x + P(46), y + P(18), F(14), TEXT, fit_end(tr("Looking for chiral-ftp on your network..."), F(14), w - P(62)).c_str(), true);
      para(x + P(46), y + P(44), w - P(62), F(12), MUTED, tr("Open chiral-ftp on the other device: it shows up here by itself."));
    }
    y += ch + P(12);
  }
  const float rh = P(72);
  for(int i = 0; i < n; i++) {
    const discovery_peer_t &p = peers[i];
    card(x, y, w, rh);
    kind_square(x + P(14), y + (rh - P(44)) / 2, P(44), p.kind);
    const char *cl = tr("Connect");
    const float bw = button_w(cl, false), tw = w - P(72) - bw - P(24);
    text(x + P(70), y + P(14), F(15), TEXT, fit_end(p.name, F(15), tw).c_str(), true);
    const std::string sub = std::string(kind_label(p.kind)) + "  \xc2\xb7  " + p.ip;
    text(x + P(70), y + P(40), F(12), MUTED, fit_end(sub, F(12), tw).c_str());
    if(button(x + w - P(12) - bw, y + (rh - P(40)) / 2, cl, PRIMARY)) discovery_ask(&p);
    y += rh + P(10);
  }
  char me[64];
  system_device_name(me, sizeof(me));
  y += para(x + P(4), y + P(6), w - P(8), F(12), MUTED, trf("This phone shows up on the others as \"{0}\".", {me}).c_str()) + P(20);
  y += download_card(x, y, w, std::min(P(112), w * 0.32f)) + P(20);
  return y;
}

float page_settings_m(float x, float y, float w) {
  text(x, y, F(22), TEXT, tr("Settings"), true);
  y += P(44);
  settings_t s = settings_get();
  bool changed = false;
  const float pad = P(16), iw = w - 2 * pad;

  auto group = [&](const char *title) {
    text(x + P(4), y, F(11), MUTED, title, true);
    y += P(22);
  };
  // A row: label (and note) on the left, a small control on the right.
  auto row_small = [&](float &yy, const char *label, const char *note, float cw, const std::function<void(float, float)> &ctl) {
    const float lw = iw - cw - P(12);
    float th = para(x + pad, yy + P(12), lw, F(14), TEXT, label);
    if(note && *note) th += P(2) + para(x + pad, yy + P(14) + th, lw, F(12), MUTED, note);
    const float rh = std::max(P(56), th + P(24));
    ctl(x + w - pad - cw, yy + rh / 2);
    yy += rh;
  };
  // A row with the control on its own line under the label.
  auto row_wide = [&](float &yy, const char *label, const char *note, const std::function<void(float, float)> &ctl) {
    float th = para(x + pad, yy + P(12), iw, F(14), TEXT, label);
    if(note && *note) th += P(2) + para(x + pad, yy + P(14) + th, iw, F(12), MUTED, note);
    ctl(x + pad, yy + P(18) + th);
    yy += th + P(18) + P(36) + P(14);
  };
  auto sep = [&](float yy) { hline(x + pad, yy, iw); };

  /* Each group is drawn twice: once to measure, once over its card. The
     controls only act on the second pass. */
  auto boxed = [&](const std::function<void(float &)> &rows) {
    const bool prev_click = g_mouse.click;
    const bool prev_right = g_mouse.right;
    float yy = y;
    draw::dl()->PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
    g_mouse.click = false;
    g_mouse.right = false;
    const int focus_before = g_focus_count;
    rows(yy);
    g_focus_count = focus_before;
    draw::dl()->PopClipRect();
    g_mouse.click = prev_click;
    g_mouse.right = prev_right;
    card(x, y, w, yy - y);
    float y2 = y;
    rows(y2);
    y = y2 + P(24);
  };

  group(tr("GENERAL"));
  system_net_t nets[16];
  const int nn = system_networks(nets, 16);
  boxed([&](float &yy) {
    row_wide(yy, tr("Language"), nullptr, [&](float cx, float cy) {
      if(chooser(cx, cy, iw, Settings::language_value())) {
        Menu *m = new Menu;
        m->title = tr("Language");
        std::string current = i18n_current(), auto_name;
        for(int i = 0; i < i18n_count(); i++) if(current == i18n_code(i)) auto_name = i18n_native_name(i);
        m->items.push_back({trf("Automatic ({0})", {auto_name}), [] { i18n_set("auto"); save_prefs(); }});
        for(int i = 0; i < i18n_count(); i++) {
          std::string code = i18n_code(i);
          m->items.push_back({i18n_native_name(i), [code] { i18n_set(code.c_str()); save_prefs(); }});
        }
        push(m);
      }
    });
    if(nn > 0) {
      sep(yy);
      row_wide(yy, tr("Network"), tr("The address the server and web app show."), [&](float cx, float cy) {
        if(chooser(cx, cy, iw, Settings::network_value())) {
          Menu *m = new Menu;
          m->title = tr("Network");
          m->items.push_back({trf("Automatic ({0})", {pick_ip()}), [] { g.network.clear(); save_prefs(); ip_changed(); }});
          for(int i = 0; i < nn; i++) {
            std::string name = nets[i].name;
            m->items.push_back({Settings::network_name(nets[i]), [name] { g.network = name; save_prefs(); ip_changed(); }});
          }
          push(m);
        }
      });
    }
  });

  group(tr("TRANSFERS"));
  boxed([&](float &yy) {
    row_small(yy, tr("Simultaneous transfers"), tr("Each logs in with its own connection, like FileZilla."), P(128), [&](float cx, float cy) {
      const int v = stepper(cx, cy - P(18), s.connections, 1, MAX_TRANSFER_CONNECTIONS);
      if(v != s.connections) s.connections = v, changed = true;
    });
    sep(yy);
    row_wide(yy, tr("When the file already exists"), nullptr, [&](float cx, float cy) {
      if(chooser(cx, cy, iw, Settings::policy_name(s.policy))) {
        Menu *m = new Menu;
        m->title = tr("When the file already exists");
        const int order[4] = {POLICY_RESUME, POLICY_OVERWRITE, POLICY_SKIP, POLICY_RENAME};
        for(int pol : order) {
          m->items.push_back({Settings::policy_name(pol), [pol] {
            settings_t t = settings_get();
            t.policy = pol;
            settings_set(&t);
            queue_kick();
          }});
        }
        push(m);
      }
    });
    sep(yy);
    row_small(yy, tr("Retries after a dropped connection"), nullptr, P(128), [&](float cx, float cy) {
      const int v = stepper(cx, cy - P(18), s.retries, 0, 10);
      if(v != s.retries) s.retries = v, changed = true;
    });
    sep(yy);
    row_small(yy, tr("Keep file dates"), nullptr, P(52), [&](float cx, float cy) {
      if(toggle(cx, cy - P(15), s.preserve_mtime)) s.preserve_mtime = !s.preserve_mtime, changed = true;
    });
    sep(yy);
    row_small(yy, tr("Show hidden files (FTP LIST -a)"), nullptr, P(52), [&](float cx, float cy) {
      if(toggle(cx, cy - P(15), g.show_hidden)) {
        g.show_hidden = !g.show_hidden;
        save_prefs();
      }
    });
    sep(yy);
    row_small(yy, tr("Remember passwords in Recent"), nullptr, P(52), [&](float cx, float cy) {
      if(toggle(cx, cy - P(15), g.save_pass)) {
        g.save_pass = !g.save_pass;
        if(!g.save_pass) {
          for(auto &hh : g.history) json::set_str(hh, "pass", "");
          save_store("history", g.history);
        }
        save_prefs();
      }
    });
  });

  group(tr("MORE"));
  boxed([&](float &yy) {
    const char *forget = tr("Forget");
    row_small(yy, tr("Forget saved SFTP host keys"), tr("The next connection trusts the server's current key."), button_w(forget, false),
              [&](float cx, float cy) {
      if(button(cx, cy - P(18), forget, DANGER, -1, true, P(36))) {
        confirm(tr("Forget host keys"), tr("Forget every saved SFTP server key? The next connection trusts the server's current key."),
                tr("Forget"), [] { show_toast(ftp_forget_host_keys() ? tr("Could not remove the keys") : tr("Saved host keys forgotten")); });
      }
    });
    sep(yy);
    /* For testing: the Android TV interface on this phone, until the app is
       restarted (platform/android/backend.cpp reads CHIRAL_TV_LAYOUT).
       English only on purpose: not a feature, a test switch. */
    row_small(yy, "TV layout (testing)", "Shows the Android TV interface until the app restarts.", P(52), [&](float cx, float cy) {
      const char *tv = getenv("CHIRAL_TV_LAYOUT");
      const bool on = tv && *tv == '1';
      if(toggle(cx, cy - P(15), on)) setenv("CHIRAL_TV_LAYOUT", on ? "0" : "1", 1);
    });
    sep(yy);
    const char *open = tr("Open");
    row_small(yy, tr("Message log"), nullptr, button_w(open, false), [&](float cx, float cy) {
      if(button(cx, cy - P(18), open, SECONDARY, -1, true, P(36))) {
        g_back_tab = TAB_SETTINGS;
        g_tab = TAB_LOG;
      }
    });
  });
  if(changed) {
    settings_set(&s);
    queue_kick();
  }

  // About, in place: no dialog to close.
  group(tr("ABOUT"));
  boxed([&](float &yy) {
    float ay = yy + P(16);
    draw::brand_mark(x + pad, ay, P(30));
    const float ww = draw::brand_wordmark(x + pad + P(40), ay + P(4), F(20));
    text(x + pad + P(48) + ww, ay + P(9), F(12), MUTED, trf("version {0}", {VERSION_TAG}).c_str());
    ay += P(46);
    ay += para(x + pad, ay, iw, F(13), TEXT, tr("The ultimate file manager and FTP interface for this phone:")) + P(4);
    const char *features[] = {N_("  FTP server: other devices connect to this phone with any FTP app."),
                              N_("  Web app: manage this phone's files from another device's browser."),
                              N_("  File manager: copy, move and delete files on this phone."),
                              N_("  FTP / SFTP client: up to 30 transfers at once with your laptop, PC or NAS.")};
    for(const char *f : features) ay += para(x + pad, ay, iw, F(12), MUTED, tr(f)) + P(2);
    ay += para(x + pad, ay + P(6), iw, F(12), TEXT, tr("chiral-ftp runs on Windows, Mac, Linux, Android, Nintendo Switch and PS5. Get it at:")) + P(8);
    ay += para(x + pad, ay, iw, F(12), GOLD, "chiral-ftp.com") + P(16);
    ay += para(x + pad, ay, iw, F(14), GOLD, tr("Made by Herbrax (Simo Hakim)"), true) + P(4);
    ay += para(x + pad, ay, iw, F(12), MUTED, "simohakim.com  \xc2\xb7  linkedin.com/in/simo-hakim") + P(8);
    ay += para(x + pad, ay, iw, F(12), TEXT, tr("Found a bug? Please report it on GitHub. Pull requests are welcome.")) + P(2);
    ay += para(x + pad, ay, iw, F(12), GOLD, "github.com/Herbrax/Chiral-FTP") + P(14);
    ay += para(x + pad, ay, iw, F(12), TEXT, tr("Built with open-source work by others, thank you:"), true) + P(2);
    const char *const *thanks = nullptr;
    for(int i = 0, n = platform::about_thanks(&thanks); i < n; i++) ay += para(x + pad, ay, iw, F(12), MUTED, thanks[i]);
    ay += P(8) + para(x + pad, ay + P(8), iw, F(11), FAINT, tr("chiral-ftp is free software under the GPL-3.0; licenses are in the app's licenses folder."));
    yy = ay + P(18);
  });
  return y;
}

float page_transfers_m(float x, float y, float w) {
  text(x, y, F(22), TEXT, tr("Transfers"), true);
  y += P(44);
  static queue_view_t items[100];
  queue_stats_t st;
  queue_snapshot("queued", items, 0, &st);
  const char *tabs_raw[3] = {"Queued", "Failed", "Successful"};
  const int counts[3] = {st.queued + st.running, st.failed, st.done};
  std::string labels[3];
  const char *tabs[3];
  for(int i = 0; i < 3; i++) {
    labels[i] = std::string(tr(tabs_raw[i])) + " " + std::to_string(counts[i]);
    tabs[i] = labels[i].c_str();
  }
  if(segmented_w(tabs, 3) <= w) g_queue_tab = segmented(x, y, tabs, 3, g_queue_tab);
  else g_queue_tab = segmented(x, y, tabs, 3, g_queue_tab);
  y += P(54);

  // A copy on this phone, from Files.
  fsjob_stats_t js;
  fsjob_get_stats(&js);
  if(js.running) {
    const double pct = js.total > 0 ? std::min(1.0, (double)js.done / (double)js.total) : 0;
    card(x, y, w, P(70));
    text(x + P(16), y + P(12), F(13), TEXT, fit_end(trf(js.move ? N_("Moving {0}") : N_("Copying {0}"), {js.current}), F(13), w - P(32)).c_str());
    rect(x + P(16), y + P(40), w - P(32), P(6), LINE, P(3));
    rect(x + P(16), y + P(40), (w - P(32)) * (float)pct, P(6), GOLD, P(3));
    y += P(82);
  }

  const char *tab = g_queue_tab == 0 ? "queued" : g_queue_tab == 1 ? "failed" : "done";
  const int n = queue_snapshot(tab, items, 100, &st);
  if(!n) {
    const char *empty = g_queue_tab == 0 ? tr("No transfers yet.") : g_queue_tab == 1 ? tr("No failed transfers.") : tr("Finished transfers appear here.");
    y += para(x + P(4), y, w, F(13), MUTED, empty) + P(16);
  }
  for(int i = 0; i < n; i++) {
    const queue_view_t &it = items[i];
    const bool running = !strcmp(it.state, "running");
    const float rh = running ? P(84) : P(64);
    const Ctl c = control(x, y, w, rh);
    card(x, y, w, rh, c.hot ? RAISED : CARD);
    const std::string name = base_of(it.upload ? it.local : it.remote);
    const std::string route = it.error[0] ? std::string(it.error) : it.upload ? trf("Upload to {0}", {it.remote}) : trf("Download to {0}", {it.local});
    text(x + P(16), y + P(12), F(14), TEXT, fit_end(name, F(14), w - P(32)).c_str(), true);
    text(x + P(16), y + P(36), F(11), it.error[0] ? RED : MUTED, fit_end(route, F(11), w - P(32)).c_str());
    if(running) {
      const double pct = it.size > 0 ? std::min(1.0, (double)it.done / (double)it.size) : 0;
      rect(x + P(16), y + P(62), w - P(32), P(6), LINE, P(3));
      rect(x + P(16), y + P(62), (w - P(32)) * (float)pct, P(6), GOLD, P(3));
    }
    if(c.hit) {
      Menu *m = new Menu;
      m->title = name;
      unsigned long id = it.id;
      std::string state = it.state;
      if(state == "queued" || state == "running") m->items.push_back({tr("Cancel transfer"), [id] { queue_cancel(id); }, true, true});
      if(state == "failed" || state == "canceled") m->items.push_back({tr("Retry"), [id] { queue_retry(id); }});
      if(state != "running") m->items.push_back({tr("Remove from list"), [id] { queue_remove(id); }});
      push(m);
    }
    y += rh + P(8);
  }
  y += P(8);
  const float bw = (w - P(8)) / 2;
  if(g_queue_tab == 0) {
    if(button(x, y, st.paused ? tr("Resume") : tr("Pause"), SECONDARY, -1, true, P(42), bw)) queue_set_paused(!st.paused);
    if(button(x + bw + P(8), y, tr("Cancel all"), DANGER, -1, counts[0] > 0, P(42), bw))
      confirm(tr("Cancel all"), tr("Cancel every queued and running transfer?"), tr("Cancel all"), [] { queue_cancel(0); });
  } else if(g_queue_tab == 1) {
    if(button(x, y, tr("Retry all"), PRIMARY, -1, n > 0, P(42), w)) queue_retry(0);
  } else {
    if(button(x, y, tr("Clear list"), SECONDARY, -1, n > 0, P(42), w)) queue_clear("done");
  }
  return y + P(60);
}

/* A page, scrolled with a finger. The scroll is held inside the page
   before it is drawn, from the height the page had last frame: clamping
   only afterwards drew the page past its end for a frame and snapped it
   back on the next, a stutter for as long as a finger kept pushing at the
   top or the bottom. */
float g_page_content[TAB_COUNT];

void page_view(float x, float y, float w, float h) {
  float &scroll = g_page_scroll[g_tab];
  if(over_rect(x - P(16), y, w + P(32), h) && g_wheel != 0 && g.overlays.empty()) scroll -= g_wheel * P(40);
  g_wheel = 0;
  scroll = std::max(0.0f, std::min(scroll, std::max(0.0f, g_page_content[g_tab] - h)));
  draw::dl()->PushClipRect(ImVec2(x - P(16), y), ImVec2(x + w + P(16), y + h), true);
  const float top = y + P(18) - scroll;
  float bottom = top;
  float py = top;
  if(g_tab == TAB_SHARE) {
    const char *views[2] = {CHIRAL_HAS_SFTPD ? tr("Server") : tr("FTP server"), tr("Web app")};
    const float sw = std::min(w, segmented_w(views, 2));
    (void)sw;
    g_share_view = segmented(x, py, views, 2, g_share_view);
    py += P(58);
  }
  switch(g_tab) {
  case TAB_SHARE: bottom = g_share_view == 0 ? page_server_m(x, py, w) : page_web_m(x, py, w); break;
  case TAB_NEARBY: bottom = page_quick_m(x, py, w); break;
  case TAB_SETTINGS: bottom = page_settings_m(x, py, w); break;
  case TAB_TRANSFERS: bottom = page_transfers_m(x, py, w); break;
  case TAB_LOG:
    g_content_h = 0;
    page_log(x, py, w);
    bottom = g_content_h;
    break;
  default: break;
  }
  g_page_content[g_tab] = bottom - top + P(18);
  draw::dl()->PopClipRect();
}

/* ---------- the tabs ---------- */

const Tab kNav[4] = {TAB_FILES, TAB_CLIENT, TAB_SHARE, TAB_NEARBY};

Icon tab_icon(Tab t) {
  switch(t) {
  case TAB_FILES: return IC_FOLDER;
  case TAB_CLIENT: return IC_TRANSFER;
  case TAB_SHARE: return IC_SERVER;
  case TAB_NEARBY: return IC_QUICK;
  case TAB_TRANSFERS: return IC_LINES;
  default: return IC_GEAR;
  }
}

const char *tab_label(Tab t) {
  switch(t) {
  case TAB_FILES: return tr("Files");
  case TAB_CLIENT: return tr("Client");
  case TAB_SHARE: return tr("Server");
  case TAB_NEARBY: return tr("Quick connect");
  case TAB_TRANSFERS: return tr("Transfers");
  default: return tr("Settings");
  }
}

void go_tab(Tab t) {
  end_edit();
  if(t == TAB_TRANSFERS && g_tab != TAB_TRANSFERS) g_back_tab = g_tab == TAB_LOG ? TAB_SETTINGS : g_tab;
  g_tab = t;
  g_focus = -1;
  if(t == TAB_NEARBY) discovery_probe();  // the list fills in now, not in a few seconds
}

bool tab_dot(Tab t, ImU32 *col) {
  if(t == TAB_SHARE && (server_running() || websrv_running())) return *col = GREEN, true;
  if(t == TAB_CLIENT && g.connected) return *col = GREEN, true;
  if(t == TAB_TRANSFERS && g.qs.running > 0) return *col = GOLD, true;
  if(t == TAB_NEARBY) {
    discovery_peer_t peers[DISCOVERY_MAX_PEERS];
    if(discovery_peers(peers, DISCOVERY_MAX_PEERS) > 0) return *col = GOLD, true;
  }
  return false;
}

bool nav_item(Tab t, float x, float y, float w, float h, bool label, bool enabled = true) {
  const bool on = g_tab == t || (t == TAB_SETTINGS && g_tab == TAB_LOG);
  const Ctl c = control(x, y, w, h, enabled);
  const float is = P(22), pill_w = P(52), pill_h = P(30);
  const float iy = label ? y + (h - pill_h - F(11) - P(4)) / 2 : y + (h - pill_h) / 2;
  if(on) rect(x + (w - pill_w) / 2, iy, pill_w, pill_h, IM_COL32(0x3A, 0x33, 0x20, 255), pill_h / 2);
  else if(c.hot) rect(x + (w - pill_w) / 2, iy, pill_w, pill_h, HOVER, pill_h / 2);
  icon(tab_icon(t), x + (w - is) / 2, iy + (pill_h - is) / 2, is, on ? GOLD_LIGHT : MUTED);
  ImU32 dc;
  if(tab_dot(t, &dc)) dot(x + w / 2 + is / 2 + P(2), iy + P(4), P(4), dc);
  if(label) {
    const std::string n = fit_end(tab_label(t), F(11), w - P(4));
    text(x + (w - text_w(F(11), n.c_str(), on)) / 2, iy + pill_h + P(4), F(11), on ? TEXT : MUTED, n.c_str(), on);
  }
  if(c.focus) focus_ring(x, y, w, h, P(10));
  if(c.hit) go_tab(t);
  return c.hit;
}

void bottom_nav(float h) {
  rect(0, H - h, W, h, SIDE);
  hline(0, H - h, W);
  const float iw = W / 4;
  for(int i = 0; i < 4; i++) nav_item(kNav[i], iw * (float)i, H - h, iw, h, !g_cover);
}

void top_bar(float h) {
  const float ms = P(24);
  draw::brand_mark(P(14), (h - ms) / 2, ms);
  if(!g_cover) draw::brand_wordmark(P(46), (h - F(17)) / 2, F(17));
  const float bs = P(44);
  float bx = W - P(6) - bs;
  if(icon_button(bx, (h - bs) / 2, bs, [](float gx, float gy, float s) { icon(IC_GEAR, gx, gy, s, SOFT); },
                 g_tab == TAB_SETTINGS || g_tab == TAB_LOG))
    go_tab(TAB_SETTINGS);
  bx -= bs + P(4);
  if(icon_button(bx, (h - bs) / 2, bs, [](float gx, float gy, float s) { icon(IC_LINES, gx, gy, s, SOFT); }, g_tab == TAB_TRANSFERS))
    go_tab(TAB_TRANSFERS);
  const int busy_n = g.qs.running + g.qs.queued;
  if(busy_n > 0) {
    const std::string n = std::to_string(busy_n);
    const float bw = std::max(P(16), text_w(F(10), n.c_str(), true) + P(8));
    rect(bx + bs - bw, (h - bs) / 2 + P(2), bw, P(16), GOLD, P(8));
    text(bx + bs - bw + (bw - text_w(F(10), n.c_str(), true)) / 2, (h - bs) / 2 + P(2) + (P(16) - F(10)) / 2, F(10), INK, n.c_str(), true);
  }
}

/* Landscape: the tabs, in a panel that slides in from an edge, over the
   screen. */
float g_nav_anim = 0;  // 0 tucked away, 1 all the way in

const float NAV_W = 84;

/* Where the panel is this frame, or the handle's touch zone when it's
   away: what the screen under it must not answer to. */
void nav_area(float &x, float &y, float &w, float &h) {
  if(g_nav_anim > 0) {
    const float pw = P(NAV_W), shown = pw * ease_out(g_nav_anim);
    x = g_nav_right ? W - shown : shown - pw;
    y = 0;
    w = pw;
    h = H;
  } else {
    x = g_nav_right ? W - P(28) : 0;
    y = (H - P(96)) / 2;
    w = P(28);
    h = P(96);
  }
}

void nav_drawer() {
  const double t = now();
  approach(g_nav_anim, t < g_nav_until ? 1.0f : 0.0f, 0.2f);
  float x, y, w, h;
  nav_area(x, y, w, h);
  if(g_nav_anim <= 0) {
    /* A handle at the edge says the tabs are there. */
    const float hx = g_nav_right ? W - P(6) : P(2), hh = P(44);
    rect(hx, (H - hh) / 2, P(4), hh, IM_COL32(0x5A, 0x5E, 0x66, 160), P(2));
    const Ctl c = control(x, y, w, h);
    if(c.hit) g_nav_until = t + 6;
    return;
  }
  /* A tap anywhere else puts it away. */
  if(g_mouse.click && !g_click_taken && !over_rect(x, y, w, h)) g_nav_until = 0;
  rect(x, 0, w, H, IM_COL32(0x13, 0x15, 0x18, 248));
  rect(g_nav_right ? x : x + w - px(), 0, px(), H, LINE);
  const Tab tabs[6] = {TAB_FILES, TAB_CLIENT, TAB_SHARE, TAB_NEARBY, TAB_TRANSFERS, TAB_SETTINGS};
  const float ih = std::min(P(64), (H - P(16)) / 6);
  float iy = (H - ih * 6) / 2;
  const bool in = g_nav_anim >= 1;  // only once it has arrived does it take taps
  for(Tab tab : tabs) {
    if(nav_item(tab, x, iy, w, ih, ih >= P(56), in)) g_nav_until = t + 1.2;
    iy += ih;
  }
  if(in && over_rect(x, 0, w, H) && g_mouse.click) g_nav_until = std::max(g_nav_until, t + 4);
}

void glance() {
  const bool running = server_running();
  draw::brand_mark(P(8), P(8), std::min(P(24), std::min(W, H) * 0.2f));
  float y = P(40);
  text(P(8), y, F(12), running ? GREEN : MUTED, running ? tr("Running") : tr("Stopped"), true);
  y += P(20);
  if(toggle(P(8), y, running)) {
    if(running) stop_server();
    else start_server();
  }
  y += P(40);
  if(running) mono(P(8), y, F(10), TEXT, fit(server_address(), F(10), W - P(16), true).c_str());
}

void toast_m(float bottom) {
  if(now() >= g.toast_until || g.toast.empty()) return;
  const float tw = std::min(W - P(32), text_w(F(13), g.toast.c_str()) + P(40));
  const float tx = (W - tw) / 2, ty = bottom - P(56);
  card(tx, ty, tw, P(40), g.toast_err ? IM_COL32(0x46, 0x18, 0x1E, 255) : RAISED, BORDER, P(20));
  text(tx + P(20), ty + (P(40) - F(13)) / 2, F(13), TEXT, fit_end(g.toast, F(13), tw - P(40)).c_str());
}

void show_nav(bool right, double seconds) {
  g_nav_right = right;
  g_nav_until = now() + seconds;
}

void render() {
  g_focus_last = g_focus_count;
  g_focus_count = 0;
  g_hand = false;
  g_click_taken = false;
  g_edit_seen = false;
  if(!g.overlays.empty()) end_edit();

  const float wdp = W / P(1), hdp = H / P(1);
  g_glance = wdp < 240 || hdp < 240;
  g_cover = !g_glance && hdp < 520 && wdp < 600;
  g_landscape = !g_glance && !g_cover && wdp > hdp;
  g_short = hdp < 480;
  if(g_landscape && !g_was_landscape) show_nav(true, 7);  // just turned: show where the tabs went
  g_was_landscape = g_landscape;
  if(g.connected && !g_was_connected) go_tab(TAB_CLIENT);  // a quick connect, or a server just connected
  g_was_connected = g.connected;

  const double t = now();
  g_frame_dt = g_frame_t > 0 ? (float)std::min(0.05, t - g_frame_t) : 0.0f;
  g_frame_t = t;

  /* The pointer, kept from whatever lies under the panel or the sheet: a
     tap on the panel answered the screen behind it too, since the screen
     is drawn (and asked) first. */
  const Mouse mouse = g_mouse;
  const float wheel = g_wheel;
  auto hide_pointer = [] {
    g_mouse.click = g_mouse.right = false;
    g_mouse.inside = false;
    g_wheel = 0;
  };
  auto give_pointer = [&] {
    g_mouse = mouse;
    g_wheel = wheel;
  };
  const bool modal = g_sheet.open || g_sheet_anim > 0;
  bool under_nav = false;
  if(g_landscape && !modal) {
    float nx, ny, nw, nh;
    nav_area(nx, ny, nw, nh);
    under_nav = over_rect(nx, ny, nw, nh) || (g_nav_anim > 0 && mouse.click);  // a tap outside it only closes it
  }
  if(modal || under_nav) hide_pointer();

  rect(0, 0, W, H, BG);
  if(g_glance) {
    glance();
  } else {
    const float bar_h = g_landscape ? 0 : P(g_cover ? 48 : 56);
    const float nav_h = g_landscape ? 0 : P(g_cover ? 52 : 68);
    const float pad = P(g_cover ? 6 : 10);
    const float x = pad, y = bar_h + (g_landscape ? pad : 0);
    const float w = W - 2 * pad, h = H - y - nav_h - pad;
    switch(g_tab) {
    case TAB_FILES: files_view(x, y, w, h); break;
    case TAB_CLIENT: client_view(x, y, w, h); break;
    default: {
      const float pw = std::min(w - P(8), P(640));
      page_view(x + (w - pw) / 2, y, pw, h);
      break;
    }
    }
    if(g_landscape) {
      if(!modal) give_pointer();
      nav_drawer();
    } else {
      top_bar(bar_h);
      bottom_nav(nav_h);
    }
    toast_m(H - nav_h);
  }
  if(modal) give_pointer();
  sheet_draw();

  if(!g_edit_seen) end_edit();
  if(g_focus >= g_focus_count) g_focus = g_focus_count - 1;
  g_wheel = 0;
  g_mouse.click = g_mouse.right = false;
}

/* Back steps out: marks, then a folder, then Transfers or the log, then
   to Files. */
bool key(Action a) {
  if(a == ACT_CIRCLE || a == ACT_CIRCLE_LONG) {
    end_edit();
    if(g_sheet.open) {
      g_sheet.open = false;
    } else if(g_nav_anim > 0) {
      g_nav_until = 0;
    } else if(g_tab == TAB_TRANSFERS || g_tab == TAB_LOG) {
      g_tab = g_tab == TAB_LOG ? TAB_SETTINGS : g_back_tab;
    } else if(g_tab == TAB_FILES || g_tab == TAB_CLIENT) {
      Pane &p = active_pane();
      if(!p.marked.empty()) p.marked.clear();
      else if(a == ACT_CIRCLE && !p.path.empty() && !(p.remote && p.path == "/")) up(p);
      else if(g_tab == TAB_CLIENT) go_tab(TAB_FILES);
      else if(a == ACT_CIRCLE && p.path.empty()) platform::to_background();  // the first screen: as any app, behind the home screen
    } else {
      go_tab(TAB_FILES);
    }
    return true;
  }
  switch(a) {
  case ACT_TOUCHPAD: go_tab(TAB_TRANSFERS); return true;
  case ACT_OPTIONS: go_tab(TAB_SETTINGS); return true;
  case ACT_UP: case ACT_DOWN: case ACT_LEFT: case ACT_RIGHT: case ACT_CROSS: return desk::key(a);
  default: return true;
  }
}

bool busy() {
  (void)desk::render;  // the computer's whole-window layout: this shell draws instead
  /* Anything still sliding, or the panel's time not yet up. */
  const bool sliding = (g_sheet_anim > 0 && g_sheet_anim < 1) || (g_sheet_anim > 0 && !g_sheet.open) ||
                       (g_nav_anim > 0 && g_nav_anim < 1) || (g_nav_anim > 0 && now() >= g_nav_until);
  return g.panes[1].loading > 0 || sliding || (g_landscape && now() < g_nav_until);
}

}  // namespace mob
