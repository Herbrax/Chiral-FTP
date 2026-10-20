# Porting chiral-ftp from the PS5 to the Switch

This file is for whoever has to keep the two trees in step. It lists every
place the Switch build differs from
[chiral-ftp for PS5](https://github.com/Herbrax/Chiral-FTP-PS5) 1.0 and why.

## What didn't change

Most of it. The PS5 app was already split so that only one layer knows what
console it is on:

- `engine/` — FTP and SFTP, the transfer queue, the FTP server, local
  filesystem jobs, settings. Plain C on POSIX calls.
- `web/` — the web app's HTTP server on libmicrohttpd.
- `app/` above `backend.h` — the UI, Dear ImGui, the software rasteriser,
  translations.
- `vendor/imgui`, `assets/`, `i18n/`.

The UI draws into a 1920×1080 CPU framebuffer in `IM_COL32` order and hands
it to `backend::present()`. That interface is what the port replaces; the
pixels above it are identical on both consoles, which is also why the host
(SDL) build still tests the real UI.

## The console layer

| PS5 | Switch |
|---|---|
| `app/backend_ps5.cpp` — VideoOut, ScePad, `sceImeDialog` | `app/backend_switch.cpp` — libnx framebuffer, `PadState`, swkbd |
| `app/ps5_runtime.c` — libc gaps, RDRAND | *(none needed; newlib and libnx cover it)* |
| `app/elevation/` — Lapy / elfldr sandbox escape | *(none needed; homebrew already reads the SD card)* |
| `engine/ps5_net.c` — `sceNetResolver` instead of `getaddrinfo` | `engine/nx_net.c` — a working `socketpair()` |
| *(services inherited from the system)* | `app/switch_init.c` — `userAppInit`/`userAppExit` |
| `tools/build-ps5.sh`, `ps5/` toolchain | devkitPro `Makefile` |

### Video

`framebufferCreate(..., 1920, 1080, PIXEL_FORMAT_RGBA_8888, 2)` plus
`framebufferMakeLinear()`. Two things fall out of that:

- `PIXEL_FORMAT_RGBA_8888` puts red in the low byte, which is exactly what
  `IM_COL32` and the rasteriser already produce, so `present()` is a row
  `memcpy` (the PS5 had to swizzle every pixel into a tiled layout).
- The framebuffer stays 1920×1080 in handheld mode and the compositor scales
  it to the panel, so there is no second layout to test and no UI coordinate
  anywhere had to change.

`framebufferMakeLinear` costs a block-linear swizzle inside libnx on every
`framebufferEnd`. That plus a software 1080p rasteriser is the port's main
performance question; the app only redraws on input, while busy, or once a
second (`app/main.cpp`), so it has not been a problem in the PS5 build's
design and shouldn't be here either — but it is the first thing to measure
if the UI feels slow. Dropping to a 1280×720 framebuffer would mean
rescaling the whole UI, so prefer `deko3d` over that if it ever matters.

### Input

The UI's actions keep their PS5 names (`ACT_CROSS` is confirm, `ACT_CIRCLE`
is back) and are mapped by **position**, not by letter, so the same thumb
movement does the same thing:

| Action | PS5 | Switch |
|---|---|---|
| `ACT_CROSS` (confirm) | Cross | **A** |
| `ACT_CIRCLE` (back) | Circle | **B** |
| `ACT_SQUARE` (mark) | Square | **Y** |
| `ACT_TRIANGLE` (transfer) | Triangle | **X** |
| `ACT_L1` / `ACT_R1` | L1 / R1 | **L** / **R** |
| `ACT_L2` / `ACT_R2` (page) | L2 / R2 | **ZL** / **ZR** |
| `ACT_OPTIONS` (menu) | OPTIONS | **Plus** |
| `ACT_TOUCHPAD` (queue) | touchpad | **Minus** |

The PS5 backend read the analog stick by hand and synthesised directions.
libnx already folds both sticks into `HidNpadButton_Any{Up,Down,Left,Right}`,
so the Switch backend uses those — but the D-pad and a stick then share one
mask, which would press the same action twice. `actions_held()` collapses the
raw button mask into one bit per *action* before edge-detecting, which keeps
`platform_common.cpp`'s key-repeat logic correct.

`draw::button()` in `app/platform_common.cpp` draws the glyphs: lettered
circles for A/B/X/Y, pills for ZL/ZR and +/−.

### Text entry

swkbd, modelled on NX-Shell's `source/keyboard.cpp`. `swkbdShow` returning a
failure covers both a real error and the user cancelling, which is what
`native_text_prompt` wants. Buttons pressed while the keyboard had the pad
are re-latched afterwards so they don't leak into the UI.

### Filesystem

The PS5 had `/data`, `/mnt/usb0-7`, `/mnt/ext0-1`, `/mnt/disc` and `/user`.
The Switch has the SD card.

**Paths stay unprefixed.** libnx makes `sdmc` the default device, so
`/switch/chiral-ftp` *is* `sdmc:/switch/chiral-ftp`. Writing it that way
means `path_join`, `path_parent`, `path_basename`, the FTP server's root and
every stored site path behave exactly as they do in the host build — no
device-prefix special cases anywhere. This is the single most important
decision in the port; breaking it means auditing all of `engine/`.

Changed:

- `engine/localfs.c` — `localfs_roots()` lists `/` (SD card), `/switch`
  (Homebrew) and `/Nintendo`. `root_info()` now reports free space through
  `statvfs`, which libnx's fsdev devices answer properly (the PS5 build
  couldn't call `statfs` at all and showed nothing).
- `engine/settings.c` — `data_dir_init()` uses `/switch/chiral-ftp`.
- `engine/config.h` — `PS5_FILE_MODE` renamed to `CHIRAL_FILE_MODE`. FAT32
  and exFAT have no permission bits, so the mode only matters for files
  uploaded *to* a server; the PS5's "every file must be 777 or the console
  won't start it" rule has no Switch equivalent.

### Network

libnx gives us real `getaddrinfo`, `getnameinfo` and `gethostid` through
sfdnsres and nifm, so the PS5's `sceNetResolver` shim is gone entirely and
`engine/ftpd.c`'s reverse lookup now takes the same path as the host build.

What *is* missing is `socketpair()`: Horizon has no `AF_UNIX`, and libnx
declares the function only to return `ENOSYS`. libmicrohttpd wants one to
wake its own polling thread, and its other two options (`pipe`, `eventfd`)
don't exist on Horizon either. `engine/nx_net.c` builds the pair out of a
loopback TCP listener, and `tools/build-switch-deps.sh` compiles
libmicrohttpd with `-Dsocketpair=chiral_socketpair` so it picks that up. The
rename is needed because libnx's own `socketpair` is in the same archive we
link, so simply defining ours would be a duplicate symbol.

**Loopback works, and the socketpair was never the problem.** Tested on
hardware once the harness had a socket driver: 127.0.0.1 binds, connects,
accepts and round-trips a byte, as does a pair made via the console's own
address. `chiral_socketpair` is sound.

`MHD_USE_ITC` turned out not to be a choice: libmicrohttpd builds an
inter-thread channel during `MHD_start_daemon` whether or not the flag is
passed, because its polling thread has no other way to be woken. So
`chiral_socketpair` is on the web app's critical path no matter what.

And it was rejecting MHD's request. MHD asks for
`SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK`; `chiral_socketpair` compared
the whole `type` argument against `SOCK_STREAM`, so it answered EPROTOTYPE
-- "protocol wrong type for socket", which is exactly what MHD's own error
log said once it was asked. It now masks the flag bits off before deciding
the socket type, and applies `O_NONBLOCK` when any flag was set, since a
caller expecting a non-blocking pair will otherwise hang on a read.

Worth noting how long that took to see: MHD's error log had the answer in
plain language the whole time, and the daemon was started without
`MHD_OPTION_EXTERNAL_LOGGER` first in the option list, which MHD warns
about and which hides its early messages. Put the logger first.

Dropping `MHD_USE_ITC` did not stop the crash either. What did remain
suspicious was the *shape* of the daemon: chiral-ftp ran it with
`MHD_USE_NO_LISTEN_SOCKET` and fed it connections from its own accept loop
through `MHD_add_connection`, because that is what the PS5's socket setup
needed. It is a corner of libmicrohttpd that very little code exercises.
On Switch the daemon is now started the ordinary way -- MHD binds, listens
and accepts, and `serve()` only waits for the stop flag -- with
`MHD_USE_POLL`, since Horizon has `poll()` and MHD's default `select()`
walks a fixed-size `fd_set`. The ITC stays off --
`web/websrv.c` omits `MHD_USE_ITC` on Switch. It was never needed: that
daemon runs with `MHD_USE_NO_LISTEN_SOCKET`, the accept loop belongs to us,
and shutdown is driven by `g_stop` rather than by interrupting MHD's poll.

`chiral_socketpair` stays, hardened (it tries the console's own address as
well as loopback, and always sets a meaningful errno -- Horizon's socket
calls will happily fail without setting one, which is how a failure came to
report itself as "errno 0, Success"), because libmicrohttpd is still
compiled against it. Nothing calls it in the normal path now.

`make WEB=0` removes libmicrohttpd altogether
(`web/stub/websrv_stub.c`) and leaves the rest of the app intact.

### Services and memory

`app/switch_init.c` does what a native PS5 title got for free:

- `appletLockExit()` and `appletSetAutoSleepDisabled(true)` — a file server
  that sleeps, or that the HOME menu can close mid-transfer, is useless.
- `plInitialize`, `setInitialize`, `nifmInitialize`, `socketInitialize`.
- `__nx_fs_num_sessions = 8`. Every fsdev session is a queue; with up to 30
  transfer slots plus the FTP server and the web app all doing I/O, libnx's
  default of 3 (and ftpd's 1) would serialise them.
- The socket driver gets 1 MB buffers growing to 4 MB with `sb_efficiency`
  8, from ftpd. The libnx defaults (32 KB/64 KB) are sized for a game's
  matchmaking traffic and throttle transfers badly.

`app/main.cpp` reports `appletGetAppletType()` to the start screen through
`ui::set_launch_mode()`, which is the slot the PS5's storage-access banner
used to occupy — same shape of information (what the console let us have),
different fact.

### Fonts

The PS5 mapped `/preinst/common/font/*.otf` for CJK and Arabic. The Switch
has pl:u shared fonts, handed over as pointers into memory — no `mmap`, no
file paths, and `app/platform_common.cpp` keeps the "language in use first"
ordering so Han characters Japanese and Chinese share take the right forms.

**The shared fonts are BFTTF, not TTF.** This only showed up on hardware:
the console's Japanese, Korean and Chinese text drew as empty boxes while
Latin and Cyrillic were fine. `plGetSharedFontByType` does not hand back a
plain font — it returns Nintendo's BFTTF container, whose header and body
are XOR-obfuscated. `check_font` runs stbtt over it, correctly concludes it
is not a font, and skips it, which is why the symptom was tofu rather than a
crash.

The decode is short: read two `u32`s, XOR each with the key, check the first
against the BFTTF magic and take the second as the length, then XOR the rest
of the body word by word into a buffer and hand *that* to ImGui. Until
someone does it, `load_shared_fonts()` is a no-op and `app/i18n.cpp` offers
only the languages Noto Sans can draw (English, Spanish, French, Italian,
Russian). The translations for the other four are untouched in
`i18n/translations.json`, so turning them back on is: decode BFTTF, add the
codes to `kRenderable`, and bundle a Noto Sans Arabic (the console has no
Arabic font at all).

### Translations

21 strings named the PS5; `tools/` has no retargeting script because it was
a one-off, but the rules were: `PS5` → `Switch` in all nine languages,
`TOUCHPAD` → `MINUS`, and five strings rewritten outright because they
described hardware the Switch hasn't got (`internal storage, USB drives and
extended storage`) or facts that changed (the firmware line, the two
storage-access banner strings). Two new strings were added for the
launch-mode banner. `tools/gen-i18n.py` enforces the rest: it fails the
build on a missing string, a missing language, or mismatched placeholders.

### Other small things

- `engine/notify.c` — Horizon has no notification service for homebrew, so
  `notify_user()` writes to the app log, which the FTP server and web app
  screens already show.
- `engine/sftp.c` — devkitPro's `switch-mbedtls` is patched to implement
  `mbedtls_hardware_poll` on `csrngGetRandomBytes`, so the PS5's RDRAND
  version is deleted rather than replaced.
- `web/websrv.h` was split: the four functions the UI calls, and
  `web/websrv_mhd.h` for the libmicrohttpd parts. That is what lets
  `WEB=0` compile `app/ui.cpp` without the library installed.
- `web/websrv.c` serves the brand mark as `/favicon.ico`; an `.nro` has no
  dashboard `icon0.png` to reuse.
- `app/host/backend_sdl.cpp` and `web/stub/websrv_stub.c` live in
  subdirectories because the devkitPro Makefile globs whole source
  directories, and neither should be in the Switch build.
- `main()` returns normally; `platform::can_quit()` is `true` and the Plus
  menu grew an **Exit chiral-ftp** item. A PS5 title had to be closed with
  the PS button and could never return from `main`.
- The crash handler is gone: it decoded x86-64 `ucontext` and walked the
  stack for PIE-relative return addresses. An aarch64 equivalent would be
  worth having — see below.

## Still to do

- **USB mass storage.** libusbhsfs mounts drives as `ums0:`…`ums7:`. Those
  are device-prefixed paths, which is exactly what the "paths stay
  unprefixed" decision above avoids, so this needs either a prefix-aware
  path layer in `engine/` or a mapping of `ums0:` onto a synthetic
  `/usb0` root.
- **Save data and NAND**, the way [xftpd](https://github.com/p-sam/xftpd)
  does it: `fsdev` mounts with elevated filesystem permissions, plus
  title-ID/user-ID name lookup so the listing is readable.
- **A crash handler.** libnx has `diagAbortWithResult` and Atmosphère writes
  crash reports to `/atmosphere/crash_reports`; worth wiring the startup log
  into that.
- **Touch screen**: done. `backend::touch` reports one finger in panel
  pixels, `platform::Touch` turns that into press/held/released/drag in the
  UI's coordinate space, and `app/ui.cpp` matches a tap against rectangles
  each screen records as it draws them (`hit_add`). Taps on a list move the
  cursor first and act on a second tap; menus and the start screen's cards
  act immediately. Dragging a list scrolls it. The SDL backend declines to
  have a touchscreen.
- **Performance measurement** of the linear-framebuffer path (above).

## What the first build turned up

The port above was written before a toolchain was available; these are the
things that only showed up once devkitA64 actually compiled it.

**No `sys/mman.h`.** Horizon has no `mmap`. `app/platform_common.cpp`'s
allocator used it for every block of 32 KB or more, to dodge the PS5's libc
heap cap. libnx hands the process its whole heap and newlib's malloc draws
from all of it, so on the Switch `app_alloc`/`app_free` are plain
`malloc`/`free` and the header-tagging scheme is compiled out. (The applet-mode
cap is the console's, and no allocator trick lifts it -- hence the banner.)

**No `timegm`.** It is a GNU extension. `engine/ftpd.c`'s MFMT handler needs
UTC-to-`time_t`, and `mktime` is wrong because the client sends UTC while
`mktime` would read the console's local time. There is now a days-from-civil
`chiral_timegm` in that file, used through a `#define` on Switch only.

**ImGui's shell helper.** `Platform_OpenInShellFn_DefaultImpl` calls `execvp`
and `waitpid`; neither exists. `IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS` in
`app/imgui_config.h` drops it -- nothing in chiral-ftp opens a shell.

**`vendor/imgui`'s SDL backends.** `imgui_impl_sdl2.*` and
`imgui_impl_sdlrenderer2.*` were vendored but used by neither build (the app
has its own software rasteriser and its own SDL backend). The Switch
Makefile globs whole directories, so they were being compiled and failing on
`SDL.h`. Deleted.

**`i18n.o` collided.** devkitPro compiles every object into one flat build
directory, so `app/i18n.cpp` and `gen/i18n.c` both wanted `i18n.o` -- and
the C++ one won, silently leaving the translation table out of the link
(which surfaced as undefined `chiral_i18n_*`). The generated file is now
`gen/i18n_table.c`. Worth remembering for any future generated source.

**The `gen/` glob was evaluated too early.** `CFILES` used
`$(wildcard gen/*.c)`, which make expands while parsing -- before the
generator recipes have run. On a clean tree that silently dropped every
generated object. The list is now named explicitly from `$(GENERATED)`.

**`$(BUILD)` has to be `.PHONY`.** It names a directory, so once `.build`
existed make called it up to date and never recursed into the second pass.
devkitPro's own template marks it phony; this one hadn't.

**The default goal.** The generated-source rules come first in the file, so a
bare `make` picked `gen` as its target and did nothing else.
`.DEFAULT_GOAL := all` settles it.

### libmicrohttpd

It cross-compiles, and the socketpair plan works -- but configure has to be
able to *link* against the shim, not just see a `-D`. So
`tools/build-switch-deps.sh` now compiles `engine/nx_net.c` into
`.deps/switch/lib/libchiral-nx-compat.a` first and configures MHD against
it; without that, configure concludes "socketpair(3) is not usable" and
stops. The app links `engine/nx_net.o` directly, so the archive only ever
serves MHD's link tests.

Three more things configure needed: `-specs=libnx/switch.specs` in `LDFLAGS`
(otherwise its "can the compiler create executables" test can't link
`-lnx`), `pkg-config`, and `--disable-dependency-tracking` (automake's
depfile bootstrap doesn't survive the cross setup, and a pinned one-shot
build is never rebuilt incrementally).

`switchvars.sh` sources `devkita64.sh`, which hardcodes
`DEVKITPRO=/opt/devkitpro` and bakes that into every `-I` and `-L` it
exports. The script now restores the prefix it was actually given, so a
devkitPro installed somewhere else still works.

### Verified

`make`, `make WEB=0` and `make release` all from clean, with devkitA64 16.1.0
/ libnx 4.12.0 / switch-tools 1.13.1 and switch-libssh2, switch-mbedtls,
switch-zlib. The .nro is 3.3 MB with the web app, 3.0 MB without, and
`chiral_socketpair`, `MHD_start_daemon`, `plGetSharedFontByType`,
`swkbdShow`, `framebufferMakeLinear`, `userAppInit` and `ftpd_start` are all
in the link.

**Not verified: none of this has run on a console.** It compiles, links and
packages; whether the loopback socketpair actually works on Horizon, whether
the 1080p software rasteriser is fast enough, and whether the shared fonts
merge cleanly are all still open. The host build (`Makefile.host`) wasn't
exercised either -- it needs SDL2 and a host compiler.
