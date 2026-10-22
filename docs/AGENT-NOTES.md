# Agent notes: keeping chiral-ftp multi-platform

These notes are for coding agents (and people) changing chiral-ftp. They are
the merged notes of the two sessions that built it:

- **the PS5 and unification session** — wrote the PS5 version, then got the
  unified tree building, booting and released on both consoles. It also did
  the Switch build's leftover `nx_net` paths, the per-platform buttons and
  About screen, devkitPro on macOS, deploying the `.nro`, and the v1.0.1
  release of both;
- **the Switch port session** — ported the app to libnx from a Windows
  machine with no console and no container, then debugged it on the
  maintainer's hardware: the crashes at launch, the tofu text, the slowness,
  the crash on exit, touch, and the four single-feature builds that found
  most of it.

Neither session could build every platform. That is the single most
important fact about this project's history, and most of the mistakes below
come from it.

The short version:

1. **Ask before making anything shared.** Many differences between the
   platforms are deliberate. Don't merge them into "one generic version"
   unless the maintainer asked for that.
2. **On the PS5, a symbol that links is not a symbol a title may call.**
   Test on the console, and read `/data/chiral-ftp/startup.log` when it
   doesn't start.
3. **Build all three platforms after any shared change.** `make check`
   passing only means every platform defines the contract, not that it
   compiles. And a build that succeeds is not a build that linked what you
   think it did — check the symbols.
4. **When other homebrew does cleanly the thing we do badly, the bug is
   ours.** Both sessions wasted time theorising about Atmosphère, libnx and
   libmicrohttpd before finding their own bug.
5. **Prove the toolchain and each layer separately before the whole app.**
   The ladder of single-feature builds in §4.2 found in one round trip each
   what guessing did not find in several.

---

## 1. How the tree is split, and the rules that keep it that way

- Shared code lives in `engine/`, `web/` and `app/`. Per-platform code lives
  in `platform/<ps5|switch|sdl>/`: `backend.cpp` (screen, pad, keyboard,
  About lines), `system.c` (storage, network, locale), `platform_config.h`
  (compile-time traits) and `platform.mk`.
- **Shared code never names a platform.** No `__PROSPERO__`, `__SWITCH__` or
  `__SCE__` in `engine/`, `web/` or `app/`. A difference is either a trait in
  `platform_config.h` (for example `CHIRAL_BUTTONS_PLAYSTATION`,
  `CHIRAL_BIG_ALLOC_VIA_MMAP`, `CHIRAL_FILE_MODE`) or a function in one of
  the two contracts, `app/backend.h` and `engine/system.h`.
- **`tools/check-platforms.py`** (`make check`, run by `make test`) checks
  that every platform defines every contract function and trait, and that
  shared code names no platform. When you add a contract function or trait,
  add it to the lists in that script too. It exists because three contract
  functions once existed for two platforms out of three and nothing noticed
  until a link failed on the one machine that could do that build (§3.5).
- **`app/ui.cpp` doesn't call `backend::` directly.** It goes through
  `app/platform.h`, whose functions in `app/platform_common.cpp` forward to
  the backend. Follow how `keyboard_failed` is done.
- **Translations:** every `tr()`, `trf()` or `N_()` string must exist in
  `i18n/translations.json` in all nine languages, or the build stops.
  `tools/gen-i18n.py` scans `$(APP)`, `platform/*/*.cpp`, `engine/ftpd.c` and
  `engine/fsjobs.c`. It doesn't scan headers, so a translatable
  per-platform string belongs in `platform/<name>/backend.cpp`, not in
  `platform_config.h`. When you add strings, **append** them. Re-sorting or
  re-dumping the file turned a two-string change into a 440-line diff once,
  and a nine-string change into a 787-line one another time.

### Deliberately different per platform (don't make these generic)

| What | Where | PS5 | Switch | Desktop |
|---|---|---|---|---|
| Button glyphs in the bottom bar | `CHIRAL_BUTTONS_PLAYSTATION`, `CHIRAL_BUTTONS_KEYBOARD`, `draw::button()` in `platform_common.cpp` | ✕ ○ □ △, L1/R1/L2/R2, OPTIONS, TOUCHPAD | A B Y X, L/R/ZL/ZR, Plus/Minus | key caps: Enter, Esc, Space, T, Q/E, PgUp/PgDn, M, G, arrows |
| About: file-manager line and thanks | `about_file_manager()`, `about_thanks()` in each `backend.cpp` | internal storage and USB; boilerplate, ProsperoEden, Lapy, PS5 Payload SDK… | SD card; libnx/devkitPro, ftpd, NX-Shell… | this computer; SDL2… |
| Languages offered | `system_renderable_languages()` | all nine (the console's fonts) | five: en, es, fr, it, ru (§4.4) | all nine |
| Free space | `system_free_space()` | **unknown (-1)**, see §3.3 | **unknown on purpose**, see §4.5 | statvfs |
| Drives listed | `system_storage_roots()` | /data, /data/homebrew, /mnt/usb0-7, /mnt/ext0-1, /mnt/disc… | SD card, /switch, /Nintendo | home, /tmp, / |
| Mode of written files | `CHIRAL_FILE_MODE` | **0777** (the maintainer wants this, not 755) | 0777 | 0644 |
| Settings folder | `system_data_dir_candidates()` | /data/chiral-ftp | /switch/chiral-ftp | ./data; `~/Library/Application Support/chiral-ftp` when run as the macOS `.app` |
| Text that names the machine | `CHIRAL_DEVICE_COMPUTER`, `device_text()` in `ui.cpp` | "the console" | "the console" | "this computer", no SD card or TV |
| Layout and render size | `backend::window_size()`, `begin_frame()` in `platform_common.cpp` | fixed 1920x1080 layout | fixed 1920x1080 layout | the layout *is* the window: 1.5 layout units per point (zoomable, View > Zoom), drawn at the window's exact pixels, glyphs baked at that density and not oversampled, through FreeType with full hinting (`IMGUI_ENABLE_FREETYPE`, `vendor/imgui/misc/freetype`, this build only; the consoles keep stb_truetype). Never a 1080p picture scaled into a window: that was tried and looked soft and tiny. The maintainer's Mac runs a non-Retina scaled mode (2294x1490 points at backing scale 1, stretched 1.32x onto the panel), which softens every app; hinting is what's left to win there |
| Networks to choose from | `system_networks()`, the Settings row | none (row hidden) | none (row hidden) | every IPv4 interface; Automatic prefers 192.168.x.x over a VPN |
| Mouse | `platform::detail::pointer_*`, `handle_pointer()` in `ui.cpp` | none | none (touch has its own path) | hover highlights, click acts, right click = menu, wheel scrolls, click outside a panel = Esc |
| SFTP server | `CHIRAL_HAS_SFTPD`, `engine/sftpd.c` (libssh), built only by `platform/sdl/platform.mk` | none: FTP only | none: FTP only | one server at a time, SFTP by default (port 2222), FTP on request; same login and shared folder. Uses libssh's deprecated message API on purpose: its callback API only offers handlers that serve the whole disk |
| Shared folder | `system_ftpd_default_root()`, the "Shared folder" row | the whole console | the whole SD card | the person's own folder (`$HOME`, `%USERPROFILE%` on Windows); "Whole computer" (`system_ftpd_full_root()`) shares everything after a confirmation. Only a folder picked on the server screen is saved (`ftp_root_chosen`). Whatever is shared, both servers refuse the app's data folder (`localfs_private()`: saved passwords, the server login, the SFTP host key) -- on a computer only |
| File manager and client on the start screen | `mode_shown()` in `ui.cpp`, Settings "File manager and client in this window" | always | always | hidden by default: they live in the web app, which opens in the browser |
| Web app reach | `websrv_set_local_only()`, the web screen's "Reachable from" | network | network (libmicrohttpd binds itself) | this computer only (127.0.0.1) by default; home network on request |
| Web pages' wording | `data-computer` attributes, `dev()` in `app.js`/`files.js`, from `/api/state`'s `platform` | "the console" | "the console" | "this computer" |
| Desktop interface | `CHIRAL_UI_DESKTOP`, `app/ui_desktop.inc` (included by `ui.cpp`) | the TV layout | the TV layout | every computer (macOS, Windows, Linux): a sidebar of pages (server, web app, Quick connect, message log, settings) with switches, segmented choices, inline text fields. The file manager and client live in the web app only. The three interfaces: console (PS5, Switch), computer (this one), Android (to come) |
| Menu bar | `platform/sdl/macos.mm` (also the folder chooser) | none | none | macOS only: About, Settings, Go (the modes), View, Window, Help; commands arrive as `ACT_CMD_*` |
| Who owns the HTTP listener | `CHIRAL_WEB_MHD_OWNS_LISTENER` | us (`MHD_add_connection`) | **libmicrohttpd**, see §4.3 | us |
| Big allocations | `CHIRAL_BIG_ALLOC_VIA_MMAP` | via `mmap`, see §3.4 | plain malloc (no `mmap` on Horizon) | plain malloc |

The actions keep their PS5 names everywhere (`ACT_CROSS` confirms,
`ACT_CIRCLE` goes back, `ACT_TOUCHPAD` opens the transfer queue). Only the
glyph and the physical button change per platform. Holding Back goes to the
start screen on every platform. The maintainer accepted the touchpad
opening the transfer queue on the PS5, and Minus doing it on the Switch.

**The mistake to avoid:** after unification, the shared button drawer only
drew Switch glyphs, so the PS5 showed A/B/Plus/Minus. The shared About
screen likewise said "SD card" and thanked only the Switch projects on the
PS5. Both are fixed through the trait and the two About functions above.
When something reads as "the other console", it belongs behind the
contract, not in shared code.

**Paths stay unprefixed on every platform.** libnx makes `sdmc` the default
device, so `/switch/chiral-ftp` *is* `sdmc:/switch/chiral-ftp`. Writing it
that way means `path_join`, `path_parent`, `path_basename`, the FTP server's
root and every stored site path behave exactly as they do on a PC — no
device-prefix special cases anywhere. This is the single most important
decision in the Switch port; breaking it means auditing all of `engine/`.
USB drives keep to it: libusbhsfs (Goldleaf's library, Goldleaf's commit
`b1ff881`, GPL build; `tools/build-switch-usbhsfs.sh`) mounts them as
`ums0:`, and `platform/switch/usb_drives.c` puts a router in as newlib's
default device -- `/usbN/...` goes to `umsN:/...`, everything else to
`sdmc` as before, and listing `/` adds a `usbN` folder per drive. Engine
code never sees a prefix. Proved first by a probe homebrew (§5c). The router
sits under every SD access too, so test the SD card after touching it.

---

## 2. Building

### 2.1 On macOS (the PS5 + unification session's machine)

- **PATH:** `ps5/.hostbin` and the Homebrew GNU tools must come first:
  `export PATH="$PWD/ps5/.hostbin:/opt/homebrew/opt/make/libexec/gnubin:/opt/homebrew/opt/coreutils/libexec/gnubin:/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"`.
- **PS5:** build the boilerplate once (`cd ps5 && make`), then
  `make PLATFORM=ps5` builds `dist/PPSA96024/` and `dist/chiral-ftp-<v>.zip`.
- **Switch:** devkitPro. Install with
  `dkp-pacman -S switch-dev switch-libssh2 switch-mbedtls switch-zlib dkp-toolchain-vars`
  and `export DEVKITPRO=/opt/devkitpro`. `switchvars.sh`, which the
  libmicrohttpd cross-build needs, comes from **dkp-toolchain-vars**, not
  from `switch-dev`.
- **devkitPro's `pkg-config`** (in `/opt/devkitpro/tools/bin`) hides
  Homebrew's. With it first on PATH, the desktop build can't find
  `microhttpd.h`. Build `PLATFORM=sdl` without devkitPro on PATH.
- **Desktop:** needs SDL2, libmicrohttpd and pkg-config from Homebrew, plus
  `tools/build-sftp-deps.sh host` for libssh2 and mbedTLS in `.deps/host`.
- **Installing devkitPro from an agent shell:** `sudo` can't prompt there.
  Use `osascript -e 'do shell script "…" with administrator privileges'`,
  which shows the user a macOS password window. The installer `.pkg` is
  unsigned. Check its SHA-256 against the digest GitHub publishes for the
  release asset, and read its `postinstall` before running it.
- **Downloads that fail:** ftp.gnu.org has failed from here with a
  LibreSSL `SSL_ERROR_SYSCALL`, and the Lapy git fetch has failed too. Both
  scripts check what they use (a pinned SHA-256 for archives in
  `.deps/src`; the pinned commit and a clean tree for
  `ps5/.deps/lapy/PS5-Lapy-JB-Daemon-*`), so copying them from another
  checkout is safe.

### 2.2 On Windows, with no admin rights and no virtualization

This is the maintainer's machine and the one the Switch port was written on.
It is worth describing, because an hour went into approaches that cannot
work here.

- **WSL2 and Docker are not available**, and no amount of retrying the
  installer changes that: VT-x is **disabled in the BIOS**
  (`(Get-ComputerInfo).HyperVRequirementVirtualizationFirmwareEnabled` is
  `False`). The symptom is misleading — the Docker/WSL distro deployment
  fails with *"The specified network name is no longer available"*, which
  reads like a download problem. **Check the virtualization flags first.**
- **The devkitPro Windows installer wants MSYS2 and admin rights.** The
  toolchain in `C:\devkitPro` was assembled by hand instead: the pacman
  packages (`devkitA64`, `libnx`, `switch-tools`, `general-tools`,
  `dkp-toolchain-vars`, and the `switch-*` portlibs) were downloaded and
  extracted into that prefix directly. `mingw32-make.exe`, `pkg-config` and
  friends sit in the session scratch `bin/` and go on PATH.
- So on this machine the build is
  `mingw32-make.exe PLATFORM=switch PYTHON=python` — **`python3` does not
  exist here, only `python`**, which is why the Makefile takes `PYTHON` as a
  variable.
- The Bash tool is Git Bash, not MSYS2 and not WSL. `make`, `gcc` and
  `python3` are absent from it; the devkitPro toolchain is reached by
  absolute path or by putting the scratch `bin/` first.
- **The Windows build (`PLATFORM=sdl`) has its own MSYS2** in
  `.deps/msys64`, unpacked from the msys2-base tarball (no installer, no
  admin rights) with the UCRT64 packages the README lists; the one in
  `C:\tools\msys64` is from 2020 and empty. Run things there through its
  login shell, which ignores `CHERE_INVOKING` and starts in `~`:
  `MSYSTEM=UCRT64 .deps/msys64/usr/bin/bash.exe -lc 'cd /c/Users/Simo/eclipse-workspace/Chiral-FTP && make PLATFORM=sdl dist'`.
  Its first `pacman-key` leaves a `gpg-agent` running that holds the
  calling shell open; end it. The checkout has CRLF line endings
  (`core.autocrlf`), so shell scripts run as `bash -o igncr script.sh`.
- **A running `chiral-ftp.exe` can't be overwritten**, so a rebuild fails to
  link while the maintainer has it open; `make TARGET=.build/sdl/chiral-ftp.exe dist`
  goes around it.
- **Consequence to keep in mind:** this machine builds `PLATFORM=switch` and
  `PLATFORM=sdl` (Windows); `PLATFORM=ps5` needs the macOS setup above.
  `make check` is *not* a substitute for a compiler.

### 2.3 Output locations, releases, and what goes stale

- **Where builds write:** the PS5 writes to `.build/ps5-app/` and `dist/`.
  The Switch build (devkitPro's `switch_rules`) leaves `chiral-ftp.elf`,
  `.nacp` and `.nro` in the repo root and copies the `.nro` to
  `dist/chiral-ftp/`. The desktop build writes `./chiral-ftp` to the root.
  All of them are in `.gitignore`. An old `.gitignore` once let them show
  up as new files, so check `git status` before committing.
- **Release:** `make PLATFORM=ps5 release && make PLATFORM=switch release`.
  Each run adds its file to `release/`:
  - `chiral-ftp-ps5-<v>.zip`, the build's own zip, with every mode 777;
  - `chiral-ftp.nro`, unversioned on purpose, so an update replaces the old
    one in `/switch` instead of making hbmenu list the app twice.

  It also writes `SHA256SUMS` and `RELEASE_NOTES.md`, the `## <v>` section
  of `CHANGELOG.md` plus install steps and checksums. Tag the release
  `v<v>`, or the download badge won't count it.
- **The hbmenu icon is committed, deliberately.** An `.nro` whose icon is not
  *exactly* a 256×256 **baseline** JPEG falls back to libnx's default without
  saying anything — which is how the app shipped once with a generic icon and
  nobody noticed until the maintainer did. `tools/render-icon.py` regenerates
  it (`make icon`), and it needs Pillow, which is why the result is in git
  rather than built every time.
- **Leftovers from unification that hid behind stale caches:**
  `tools/build-ps5.sh` didn't use `CHIRAL_INCLUDES` (so `platform_config.h`
  wasn't found), still named the old `gen/i18n.c`, and listed the web
  sources twice. `platform/switch/net.c` still included `"nx_net.h"`, a
  header renamed to `net.h`; it only kept building on the other machine
  because an old `nx_net.h` was left in `.deps/switch/include`.
  **Lesson:** after moving files, do a clean build (`make clean`, and clear
  `.deps/switch/include` of headers you renamed). A cache holding the old
  file hides the break.
- **`docs/PORTING.md` is a historical document and its file paths are
  stale.** It still says `app/backend_switch.cpp`, `app/switch_init.c`,
  `engine/nx_net.c`, `Makefile.host` — all of which moved into
  `platform/<name>/` during unification. Its *reasoning* (BFTTF, the
  socketpair, the service setup, the unprefixed-paths rule) is still the best
  write-up we have. Read it for the why, not for the where.

### 2.4 Checking UI changes without a console

The desktop build runs headless and takes screenshots:

```sh
./chiral-ftp --headless --script "wait=800 OPTIONS wait=300 DOWN DOWN CROSS wait=500 shot=/tmp/about.bmp quit"
```

To see another platform's glyphs, flip its trait in
`platform/sdl/platform_config.h`, build, take the screenshot, and **put the
file back**. The About text comes from the desktop backend, so on a
computer you only see that platform's lines.

---

## 3. PS5: what bit us, and what worked

Each entry: what you see → why → what fixed it.

### 3.1 It won't start

- **"Can't start the game or app"** → the title folder or a file in it isn't
  777. → The build runs `chmod -R 777` on `dist/PPSA96024`, and the zip
  stores 0777 modes (`ps5/tools/zip-open-modes.py`). Some FTP clients drop
  them: check before you debug the program itself.
- **SYSTEM_ILLEGAL_FUNCTION_CALL at launch** → a raw system call from the
  SDK's `libc.a(syscalls.o)` (`umask` was the first one). A title may only
  make system calls through libkernel. → `tools/build-ps5.sh` reads the link
  map and refuses to package a build that has one. Use the libkernel
  function, or implement the call in `platform/ps5/runtime.c`.
- **"abort is called (system)", or signal 11 at launch after adding
  fonts** → an ImGui assertion (`Pixels != NULL`): the glyph atlas
  allocation failed. → See §3.4. `app/imgui_config.h` now sends failed
  `IM_ASSERT`s to the startup log instead of aborting. It also defines
  `IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS`, so don't pass `-D` for it as
  well.
- **Updating a title in use:** most FTP clients overwrite `eboot.bin` in
  place, and the console silently refuses that while the title runs.
  `make PLATFORM=ps5 deploy` uploads each file under another name and
  renames it into place. That can leave an `eboot.bin.old` behind, which is
  harmless.

### 3.2 Sandbox rules (what a dashboard title may not do)

- `opendir` is blocked: use `open(O_DIRECTORY)` and `sceKernelGetdents` with
  a **64 KiB** buffer (as ProsperoEden's `native_directory.h` does). Smaller
  buffers gave empty listings.
- `fcntl` on sockets fails: non-blocking goes through `SO_NBIO`. The link
  wraps `fcntl`, `socket` and `accept` (`--wrap=…`).
- `lstat` is denied: it's wrapped to fall back to `stat`.
- `getaddrinfo`: use `sceNetResolver` (`platform/ps5/net.c`).
- `umask` is a raw system call: don't call it. Set modes explicitly with
  `chmod`.

### 3.3 "Does it link?" is not "may it be called?" (free space)

The app stopped at `ui::init: listing drives` with nothing after it.
Listing drives had started calling `statvfs` for every drive, and the
SDK's stub libraries showed why:

- `statvfs` comes from **libSceLibcInternal**, not libkernel. Calling it hung
  or crashed the title.
- libkernel's `statfs` exists only in **libkernel_sys.so**, for payloads.
  The `libkernel.so` a title links against doesn't export it.
- So with a call to `statfs`, the linker takes **`libc.a`'s raw syscall**
  instead, and the build guard rejects it.

**Settled by a probe homebrew (October 2026): `statvfs(path)` kills a
title on any path, `/data` included; `fstatvfs` on the opened folder
answers every folder in under 0.1 ms with the SDK's 88-byte layout.**
`system_free_space()` therefore opens the folder and calls `fstatvfs`, never
`statvfs`. Empty slots (`/mnt/usbN`, `/mnt/ext0`) are ~2 MB placeholders and
count as unknown. `sceShellCoreUtilGetFreeSizeOfUserPartition` and
`...GetEffectiveTotalSizeOfUserPartition` also kill a title. On the test
console: `/data` = `/user` (internal SSD) 673.86 GB, `/mnt/ext1` =
`/mnt/original_m2` (M.2) 256 GB, `/mnt/usb0` a 1 TB USB drive.

Before calling a new system function, find which stub exports it:

```sh
for f in ps5/.deps/native/ps5-payload-sdk/target/lib/*.so; do
  llvm-nm -D --defined-only "$f" | grep -w statfs && echo "  ^ $f"; done
```

Then try it on the console. Only symbols from `libkernel.so` are known to be
safe.

(The Switch also reports free space as unknown, for a completely unrelated
reason — §4.5. Don't "fix" one by copying the other.)

### 3.4 Memory: the C library heap is small

A 130 KB `malloc` failed with 448 MB free. The C library's heap on the PS5
is capped far below what the title may use, and the glyph atlas for the
Japanese, Korean, Chinese or Arabic fonts outgrows it. → With
`CHIRAL_BIG_ALLOC_VIA_MMAP`, blocks of 32 KB and up come from `mmap`
(MAP_ANON) and small ones from `malloc`. A 16-byte header says which. This
allocator serves both ImGui and global `operator new`. Fonts are mapped
read-only, not loaded into the heap.

**Include-order trap:** the `#if CHIRAL_BIG_ALLOC_VIA_MMAP` around
`<sys/mman.h>` in `platform_common.cpp` only works if `platform_config.h`
is included **before** it. Otherwise the trait reads as 0 and `mmap` is
undeclared on the PS5 only.

### 3.5 Full storage access (Lapy)

- The title starts sandboxed (uid 1). At startup it sends the packaged Lapy
  helper to elfldr on port 9021 (or uses OnionHEN or a resident Lapy)
  through ProsperoEden's client in `platform/ps5/elevation/`.
- **`/data` looks empty** = storage access didn't happen.
  ShadowMountPlus shows `/data` even to a sandboxed process, which fooled
  the original "do we have access?" check. The client now also requires
  `geteuid() == 0`, then matches the effective group (`setegid`).
- **Order in `backend::early_init()`:** load the system keyboard's module
  **first**, then request storage access. Afterwards the title may no
  longer load it, and every text prompt (new folder, rename) silently does
  nothing. The app shows "The PS5 keyboard couldn't open" if it fails.
- **Unification dropped this sequence once**, and this is the cautionary
  tale of the whole project. The shared `main.cpp` was based on the Switch
  tree, which has no sandbox to escape, so `elevation::request` was simply
  never called. The build still *compiled*; it would have started, found
  `/data` empty, and read to a user as "the app does nothing". Two other
  contract functions were missing outright, so the PS5 didn't link at all,
  and a third gap (`mbedtls_hardware_poll`) was papered over by a comment
  claiming `platform/ps5/runtime.c` provided it — a comment written without
  checking, which was wrong. All three were found by a *different* session
  on a machine that could run the PS5 compiler. `tools/check-platforms.py`
  exists so the first two cannot recur silently. Nothing catches the third
  kind but reading the code you are describing.
- The start-screen banner says only "Full access to the PS5's storage".
  The maintainer didn't want "granted by the Lapy helper" on screen; the
  log can say more.

### 3.6 Permissions while changing files

- The sandbox refuses changes inside folders that are 755 or 766. → Every
  write path uses the `engine/localfs` wrappers: `localfs_mkdir`,
  `localfs_rename`, `localfs_open_write`, `localfs_unlink`, `localfs_rmdir`
  and `localfs_delete`. On EACCES or EPERM they `chmod` the parent folder
  (and, for rename and write, the item) to `CHIRAL_FILE_MODE` (777) and try
  once more. **New write paths must use these wrappers, not raw
  `rename`, `open` or `unlink`.**
- **Folders get `CHIRAL_FILE_MODE` plus x wherever it has r** (`DIR_MODE`
  in localfs.c, `localfs_dir_mode()`): 0777 on a console, 0755 on a
  computer. Until this was fixed the desktop build made 0644 folders
  (server MKD, web app, file manager) that nobody could open.
- **Uploads into a PS5 homebrew folder end up 0777** (`CHIRAL_HOMEBREW_777`
  in engine/config.h, on in every build; set it to 0 to build without).
  `ftp_homebrew_path()` is any remote path with `/data/homebrew` in it,
  whatever the server (chiral-ftp, ftpsrv...): `ftp_mkdir` chmods new
  folders there, and the queue chmods uploaded files and folders that were
  already there. A computer's files are otherwise 0644, which the console
  refuses to start a title from (§3.1). A server that refuses a chmod isn't
  asked again on that connection (`no_chmod`).
- **Recursive permissions:** `remote_chmod_tree()` and
  `localfs_chmod_tree()`, offered as its own entry, "Change permissions
  recursively", next to "Change permissions" whenever a folder is selected
  (web app toolbar and right-click menu, web file manager, console UI
  menus). On a console the client's Options menu carries Rename,
  permissions and Delete for the item under the cursor or the marked ones:
  Cross opens a folder, so that menu is the only way to act on one. Folders get
  `localfs_dir_mode(mode)`, links are skipped, failures don't stop the
  walk. A folder is opened before going in when its new mode keeps r+x for
  the owner, and closed after coming out otherwise.
- FTP `RMD` must stay non-recursive: use `localfs_rmdir`, not
  `localfs_delete`.
- An earlier, wrong theory replaced the system `chmod` with open+`fchmod`,
  which broke "change permissions". Use the system `chmod`.
- What still fails: system files the elevated app can't touch, read-only
  drives (EROFS isn't retried), and blocked folders more than one level
  up.

### 3.6b A server that goes away (the client's session)

- `engine/session.c` pings the browsing connection after 10 s of quiet
  (`ftp_ping`: NOOP or an SFTP realpath that must answer within 5 s, kept
  out of the message log with `ftp_conn_t.quiet`), reconnects a dropped
  one every 5 s, and **gives it up 30 s after the server last answered**:
  `session_server()` says not connected and `session_drops()` goes up. The
  native UI (`watch_connection`) then forgets the connection, leaves the
  client screen and says "Lost the connection to …"; the web app's poll
  carries `drops` and says the same.
- The connected/server state has its own lock (`g_state`), separate from
  the one network calls hold, so the UI thread never waits on a server
  that stopped answering. A background reconnect is cancelled by
  `session_disconnect()` and at exit (`g_abort`).

### 3.7 Logs and diagnosing

- `/data/chiral-ftp/startup.log` records each startup step (`ui::init: …`).
  Read it over FTP. The last line written tells you where it stopped.
- `startup_trace()` lines also appear in the **in-app message log**, which
  users see. Don't leave diagnostics in (the "Frame 1, Frame 2…" timing
  lines had to come out).
- **libmicrohttpd:** `MHD_OPTION_EXTERNAL_LOGGER` must be the **first**
  option passed to `MHD_start_daemon`. Otherwise MHD writes a warning about
  it to the message log — and, worse, hides its own early errors (§4.3).
- The old PS5 repo had a SIGSEGV handler that logged the faulting address.
  The unified tree doesn't, so a crash now leaves no line. Worth adding back
  in `platform/ps5/` if crashes come back.

### 3.8 Fonts and languages

- Japanese, Korean, Chinese and Arabic are merged from the console's own
  fonts in `/preinst/common/font`. They're mapped read-only and checked with
  stb_truetype before ImGui sees them. Arabic is shaped into presentation
  forms at build time by `gen-i18n.py`.
- The Switch can't read its shared CJK fonts yet (§4.4), so it offers only
  five languages. The PS5 offers all nine. Keep that difference in
  `system_renderable_languages()`.

---

## 4. Switch: what bit us, and what worked

Everything below was found on the maintainer's console. None of it showed up
in a compiler.

### 4.1 The app wouldn't start at all

The first build after the toolchain came up was the *whole app*, and it
crashed Atmosphère. That tells you nothing: the fault could be the
packaging, libnx, a service, a thread, the renderer or the engine. The
maintainer's instruction — *"build just a helloworld so we know the building
thing works"* — was the right call, and the session should have started
there rather than being told.

Once a hello-world `.nro` ran, the useful thing was a **ladder** of `.nro`s,
each adding exactly one layer on top of the last, each run on hardware:

1. hello world (console output only)
2. framebuffer: `framebufferCreate` + `MakeLinear`, a solid colour
3. pad input
4. ImGui + the software rasteriser
5. the engine (`ftpd_start`)
6. the web app (`websrv_start`)
7. everything

Every crash below was located in roughly one round trip each this way.
**Build the ladder before guessing.** It is the cheapest tool available when
the only debugger is a maintainer with a console and a photo of an error
code.

> **The trap, which bit twice:** a ladder stage that omits
> `platform/switch/init.c` has **no `userAppInit`, and therefore no socket
> driver at all**. Two stages "proved" that Horizon doesn't route loopback;
> it was the harness. That false conclusion was even committed to the docs
> before being caught and corrected. **If a stage touches sockets it must
> link `init.c`, and it should print whether the driver came up.**

### 4.2 The four single-feature builds

When the report is "it crashes" and the app has four subsystems, the fastest
instrument is four `.nro`s, each with exactly **one** feature compiled in and
the rest greyed out in the UI (`CHIRAL_ONLY_MODE`). That is what separated
"the web app has a bug" from "exiting has a bug", each in a single round
trip.

**Trap:** all four appeared in hbmenu as `chiral-ftp`, because the build
passed `DEFINES` but not `APP_TITLE` — the NACP is what names an `.nro`, not
the define. Four indistinguishable test builds are worse than one.

### 4.3 The web app: libmicrohttpd on Horizon

Four separate problems wearing one costume ("crashes after
`websrv_start() ok`", error 2144-0001 / 0x290):

- **`MHD_USE_ITC` is not optional.** libmicrohttpd builds an inter-thread
  channel inside `MHD_start_daemon` whether or not you pass the flag,
  because its polling thread has no other way to be woken. So the
  socketpair is on the critical path no matter what, and dropping the flag
  fixes nothing.
- **Horizon has no `AF_UNIX`, no `pipe` and no `eventfd`**, and libnx's
  `socketpair` returns `ENOSYS`. `platform/switch/net.c` builds a pair from
  a loopback TCP listener, and `tools/build-switch-deps.sh` compiles MHD
  with `-Dsocketpair=chiral_socketpair` (a rename, because libnx's own
  `socketpair` is in the same archive we link).
- **The actual bug** was ours: MHD asks for
  `SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK`, and the shim compared the
  whole `type` argument against `SOCK_STREAM`, so it answered `EPROTOTYPE`.
  It now masks the flag bits off, and applies `O_NONBLOCK` when any were
  set. **MHD's own error log said "Protocol wrong type for socket" in plain
  language the entire time.** It was invisible because
  `MHD_OPTION_EXTERNAL_LOGGER` wasn't the first option in the list, which
  MHD warns about and which suppresses its early messages. *Put the logger
  first and read what the library is telling you before theorising.*
- **Horizon fails socket calls without setting `errno`.** A failure
  reported itself as `FAILED errno=0 (Success)`, which cost a round trip.
  `chiral_socketpair` now always sets a meaningful errno. Anything wrapping
  a Horizon socket call should.
- **The daemon's shape matters.** The PS5 runs MHD with
  `MHD_USE_NO_LISTEN_SOCKET` and feeds it connections from our own accept
  loop via `MHD_add_connection` — a corner of libmicrohttpd that almost
  nothing exercises, and on Horizon `MHD_start_daemon` died inside it. The
  Switch lets MHD bind, listen and accept for itself
  (`CHIRAL_WEB_MHD_OWNS_LISTENER`), with `MHD_USE_POLL`, since Horizon has
  `poll()` and MHD's default `select()` walks a fixed-size `fd_set`.

Loopback itself was never the problem: once a test stage had a socket
driver, 127.0.0.1 binds, connects, accepts and round-trips a byte.

### 4.4 The language problem: BFTTF, not a missing font

**Symptom:** on hardware, Latin and Cyrillic drew perfectly; Japanese,
Korean and Chinese drew as rows of tofu (☐). The maintainer's description —
*"I only see latin abcdef0123 and the accents, and the text in russian, the
rest shows as losanges with question mark inside"* — is precisely a font
that loaded but has no glyphs, which sent the first theory in the wrong
direction.

**Cause:** `plGetSharedFontByType` does **not** hand back a TTF. It returns
Nintendo's **BFTTF** container, whose header and body are XOR-obfuscated.
`check_font` ran stb_truetype over it, correctly concluded "this is not a
font", and skipped it — so the failure mode was silent boxes rather than a
crash or a log line.

**Where it stands:** `system_script_fonts()` returns 0 on the Switch and
`system_renderable_languages()` offers only the five languages the bundled
Noto Sans covers (en, es, fr, it, ru). The other four translations stay in
the catalogue untouched. The fix is short and still open: read two `u32`s,
XOR each with the key, check the first against the BFTTF magic and take the
second as the length, XOR the body word by word, hand *that* to ImGui. Then
add the codes back and bundle a Noto Sans Arabic — the console has no
Arabic face at all.

**Lesson:** a font that fails to parse is indistinguishable, on screen, from
a font missing a glyph range. Log when a font is rejected, and say why.

### 4.5 "Could not save on Switch", and everything went slow

Reported as one problem; it was three.

- **Saving settings failed.** `rename()` over an existing file: FAT and
  exFAT have no atomic replace and libnx's `rename` refuses it.
  `engine/settings.c` now tries `rename`, then falls back to
  `unlink` + `rename`. Atomic platforms keep the atomic path.
- **Everything became slow after a settings change** — and the drive list
  took minutes. `statvfs` on FAT32/exFAT can count **every free cluster on
  the card**; doing that for three roots read as a hang. A probe
  later measured every route (ns, ncm, fs, `statvfs`) at about 1 ms on
  firmware 21.1.0 with an exFAT card, so free space is back -- but asked on
  a thread of its own (`system_free_space()` in `platform/switch/system.c`)
  and kept for ten seconds, never on the caller's thread, since a fuller
  FAT32 card may still be slow. Until the first answer the UI omits it. The PS5 shows free space and lost it for an
  entirely different reason (§3.3) — these two look alike and are not.
- **The D-pad felt detached from the UI.** Frame timing (`ui / wait /
  raster / show`) put 91 ms of a 95 ms frame in the rasteriser: the triangle
  filler walked each triangle's bounding box and tested barycentrics per
  pixel. `app/raster.cpp` now solves the span per scanline — three divisions
  a row, two adds a pixel, and `std::fill` for solid untextured spans, which
  is most of a UI. That is what made the app feel attached to the stick.

**Lesson:** on a console, "it's slow" usually has more than one cause. The
frame-timing line is how we found this one — and then it had to come out
before shipping, because `startup_trace` lines appear in the message log the
user reads.

### 4.6 The crash on exit (and the hard reboot in applet mode)

The longest-lived bug in the port. Leaving chiral-ftp crashed the software;
from applet mode it wedged the console badly enough to need a hard reboot.

The first theory was that this is a known Atmosphère/libnx wart. It is not,
and the maintainer ended it in one sentence: *"when I leave DBI or Goldleaf
it doesn't crash."* **When other homebrew exits cleanly and ours does not,
the bug is ours.** The single-feature builds then split it: the UI-only
build exited cleanly, the full one didn't. Two causes, both ours:

1. **The framebuffer was never released.** The process exited still owning
   both buffers and the window's hold on them, which is exactly what wedged
   the compositor. `backend::shutdown()` calls `framebufferClose`, and
   `main()` calls it even when `platform::init` *failed*, since a failed
   init may still have taken a framebuffer before giving up.
2. **Three detached threads outlived `main`.** Returning from `main` runs
   the static destructors, which take apart the UI state, the job queues and
   the translation tables — while the UI worker, `fsjobs` and the session
   keepalive were still reading them. (The transfer workers retire
   themselves when idle -- but only after 30 s, and never mid-transfer, so
   `queue_stop()` now cancels them at exit and the wait covers
   `queue_workers()`.)

The teardown order in [app/main.cpp:183-222](app/main.cpp#L183-L222) is
**load-bearing**, and anything added to it must keep the shape:

```
ftpd_stop(); SFTPD_STOP(); websrv_stop(); discovery_stop(); queue_stop();
                                     ask the servers, quick connect and the
                                     transfer workers to stop
  → wait up to 4 s by the clock for them to actually be down (pumping the
    applet loop; pump() returns early on input, so don't count its calls)
session_disconnect();
ui::stop_workers(); fsjob_stop(); session_stop_keepalive();
platform::shutdown();                framebufferClose
return 0;                            → static destructors, nothing left running
```

`appletLockExit()` in `platform/switch/init.c` means **we** decide when the
process ends, so all of this is on us. If you add a thread, add its stop
here. If you add a server, it must have a `*_running()` the wait can see.

### 4.7 Touch

- `hidInitializeTouchScreen()` at init; `backend::touch` reports one finger
  in **panel pixels (1280×720 regardless of the framebuffer size)**, and
  `platform_common` converts into the UI's 1920×1080 space.
- The first attempt drew a touch cursor and registered nothing, because the
  UI had no idea where anything was on screen. Screens now record rectangles
  as they draw them (`hit_add`) and taps are matched against them
  (`hit_at`). Lists move the cursor on the first tap and act on the second;
  cards and menu items act immediately; dragging a list scrolls it.
- **Anything reachable by touch must also be reachable by the D-pad.** The
  Disconnect button was added as a touch target and was unreachable with the
  stick until it got a focus slot (`g.disconnect_focus`). The maintainer
  found that, not the session that wrote it. Walk every new control with the
  stick before calling it done.

### 4.8 Services, memory and the launch-mode banner

`platform/switch/init.c` does what a native PS5 title got for free:

- `appletLockExit()` and `appletSetAutoSleepDisabled(true)` — a file server
  that falls asleep, or that HOME can close mid-transfer, is not a server.
- `plInitialize`, `setInitialize`, `nifmInitialize`, `socketInitialize`.
- `__nx_fs_num_sessions = 8`. Every fsdev session is a queue; with up to 30
  transfer slots plus the FTP server and the web app all doing I/O, libnx's
  default of 3 (and ftpd's 1) serialises them.
- Socket buffers of 1 MB growing to 4 MB with `sb_efficiency` 8, taken from
  mtheall's ftpd. The libnx defaults (32 KB/64 KB) are sized for a game's
  matchmaking traffic and throttle transfers badly.
- `userAppInit` runs **before** the app has chosen its data folder, so it
  cannot write the startup log. Each service's `Result` is recorded and
  replayed later by `report_startup_services()`. Without that, a failed
  `socketInitialize` appears only as "the FTP server won't start".

`startup_notice()` reports `appletGetAppletType()`: launched over a game the
app is a full application with the console's memory; from the album it is a
library applet capped near 448 MB. No allocator trick lifts that cap — hence
the banner, which exists because it is the one thing the user can change.

### 4.9 Smaller Switch things worth not rediscovering

- **devkitPro compiles every object into one flat build directory.**
  `app/i18n.cpp` and `gen/i18n.c` both wanted `i18n.o`; the C++ one won and
  silently left the translation table out of the link. The generated file is
  `gen/i18n_table.c` for that reason. Remember it for any new generated
  source.
- **`$(wildcard gen/*.c)` is expanded while make parses**, before the
  generator recipes run, so on a clean tree it silently dropped every
  generated object. Name generated sources explicitly.
- **`$(BUILD)` must be `.PHONY`** — it names a directory, so once `.build`
  exists make calls it up to date and never recurses into the second pass.
- **`.DEFAULT_GOAL := all`**, or a bare `make` picks the first rule in the
  file (a generated-source rule) and does nothing useful.
- **No `mmap`, no `timegm`, no `execvp`** on Horizon. Hence
  `CHIRAL_BIG_ALLOC_VIA_MMAP 0`, `system_timegm()` in the contract, and
  `IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS`.
- **libmicrohttpd's `configure` must be able to *link* against the
  socketpair shim**, not merely see a `-D`. `tools/build-switch-deps.sh`
  compiles it into `.deps/switch/lib/libchiral-nx-compat.a` first; without
  that, configure concludes "socketpair(3) is not usable" and stops. It also
  needs `-specs=libnx/switch.specs` in `LDFLAGS`, `pkg-config`, and
  `--disable-dependency-tracking`.
- **`switchvars.sh` hardcodes `DEVKITPRO=/opt/devkitpro`** via
  `devkita64.sh` and bakes it into every `-I` and `-L`. Our script restores
  the prefix it was actually given, so a devkitPro installed elsewhere (such
  as `C:\devkitPro`) still works.
- `web/websrv.c` serves the brand mark as `/favicon.ico`; an `.nro` has no
  dashboard `icon0.png` to reuse.

---

## 5. Shared-code lessons that aren't about either console

- **Both servers re-check their own listener every 20 seconds.** A console
  that has slept comes back with its sockets dead while `g_running` is still
  true, and the UI happily claims to be serving. `FTPD_HEALTH_SECONDS` and
  `WEBSRV_HEALTH_SECONDS` clear the flag so the status stops lying. Written
  for the PS5's sleep/resume and applied to both, which is the right instinct
  for anything in `engine/` or `web/`.
- **A build that succeeds is not a build that linked what you think.**
  `WEB ?= 1` never took effect because `WEB` already held the web source
  list, so `make WEB=0`'s stub was selected silently and the shipped `.nro`
  had no web app — caught only by noticing it was 278 KB smaller and that
  `MHD_start_daemon` was absent from the symbol table. The variable is
  `WEB_SRCS` now. **Check a handful of expected symbols in the artefact
  after any build-system change**, the way `tools/build-ps5.sh` does.
- **The SFTP client's functions are `sftpc_*`, not `sftp_*`.** libssh,
  which the computer build links for the SFTP server, exports `sftp_open`,
  `sftp_close` and others under the plain names. On macOS the app's own
  copies would quietly win; on Linux they would hijack libssh's calls.
- **Never bulk-rewrite the translation catalogue.** Beyond the diff churn:
  a `\b(Switch|PS5)\b` substitution turned "Switch side" — meaning *swap the
  panes* — into "console side". Mechanical neutralisation needs reading
  afterwards, and roughly twenty strings still want a native speaker's eye.
- **`app/ui.cpp`'s background work uses raw pthreads, not `std::thread`.**
  `std::thread`'s destructor calls `std::terminate` on a joinable thread,
  which is a bad way to find out your teardown order is wrong — and the
  teardown order is the thing most likely to be wrong (§4.6).

---

## 5b. Quick connect (engine/discovery.c)

- **What it is:** every running copy says hello on the network (UDP
  broadcast to 255.255.255.255, port **47121**, every 3 s) and lists the
  others. One asks another to share (REQUEST, resent every 1.5 s); the
  other shows the request with a 4-digit code, and on Allow starts its
  server if needed and answers with a login that works **only from the
  asker's address** (`ftpd_add_guest`, `sftpd_add_guest`; cleared when
  the server stops). Replies go back to the address and port a message
  came from.
- **Only calls the app already makes, plus `sendto`/`recvfrom` and
  `SO_BROADCAST`.** On the PS5 both come from `libkernel.so` (the linked
  ELF imports them; nothing from `libc.a(syscalls.o)`). Whether each
  console actually delivers broadcasts is unproven until run on hardware.
- **Never fatal.** Each step that can fail (socket, `SO_BROADCAST`, binding
  the port, the thread) is kept in `discovery_problem()` and written to the
  startup log **once per distinct reason** (the UI retries every 5 s; the
  message log is the user's), e.g. `Quick connect off: broadcast refused
  (...)`, and the app carries on without it. The first success of a launch
  writes `Quick connect: visible as "PS5" on UDP 47121`. Check that line
  first on a console.
- **When it runs** (`quick_lifecycle()` in ui.cpp, the one place that
  starts it): on a computer (`CHIRAL_DEVICE_COMPUTER`) for as long as the
  app is open, whatever page or window is in front -- the maintainer's
  choice. On a console only while it can be used: app in front
  (`backend::in_foreground()`) and on the start screen, or with an ask or
  a request still going. Anywhere else it stops and broadcasts **BYE**, so
  the others drop the device at once; coming back starts it again with a
  PROBE, so the list fills at once. The device id stays the same for the
  whole launch.
- **Broadcasts get lost; direct datagrams don't.** Wi-Fi drops broadcast
  frames often (a console saving power hears few), which made the "N
  nearby" count flicker and only catch up when Quick connect was opened
  (its PROBE gets direct answers). So each hello also goes straight to
  every device already known, a PROBE carries the hello's fields (whoever
  hears it lists the sender), and a device is forgotten after 12 s
  (four missed hellos) unless it said BYE.
  `in_foreground()`: Switch reads libnx's focus state; desktop is window
  focus and not minimised; **the PS5 always says true** -- telling would
  take `sceSystemServiceGetStatus`, a call no build has made yet, so it
  needs discussing before it is added.
- **Self-healing.** The thread ends by itself on `POLLERR`/`POLLNVAL` or
  when three hellos in a row fail to send (network gone, console asleep and
  back); it never restarts itself. On the way out it empties the peer
  list, drops a request on screen and turns our own ask into "didn't
  answer". `quick_lifecycle()` opens a fresh socket at most every 5 s.
  `discovery_start()` returns -1 with `EBUSY` while the last thread is
  still stopping.
- **Testing on one computer:** two copies can't both own UDP 47121, and
  neither can a listener (the app sets only `SO_REUSEADDR`). A small script
  that speaks the protocol from another port stands in for the other device
  (that is why replies go to the source port). To watch the app's own
  broadcasts, build a throwaway copy with another `DISCOVERY_PORT` and
  `SO_REUSEPORT` added (build dir `.build/sdl-qt`), then restore both files.
- The thread is stopped and waited for in `main()`'s teardown with the
  servers (§4.6); `quick_lifecycle()` won't start it once `g.quit` is set.

## 5b2. Releasing (1.1 onwards)

- `make PLATFORM=ps5 release && make PLATFORM=switch release && make
  PLATFORM=sdl release` fills `release/`: the PS5 zip, the `.nro`, the Mac
  app zipped with `ditto` (keeps the bundle's signature), `SHA256SUMS`, and
  `RELEASE_NOTES.md` from the version's CHANGELOG section (pictures get
  absolute raw.githubusercontent.com addresses).
- **Linux** builds only on Linux: `tools/make-appimage.sh` (linuxdeploy), run
  by `.github/workflows/linux-appimage.yml` on Ubuntu 22.04 for a `v*` tag or
  by hand; then `tools/make-release.sh linux` adds it to `release/`. An
  AppImage keeps its settings in `$XDG_DATA_HOME/chiral-ftp`
  (`~/.local/share/chiral-ftp`; its parents are made, a new account may lack
  them). Linux links libstdc++ and libm, macOS libc++ (`CXX_LIB` in
  platform/sdl/platform.mk).
- **Linux, what the first build taught (1.1):** build on Ubuntu 22.04, one
  AppImage per architecture (x86_64, aarch64). Its libssh is 0.9, without
  `ssh_pki_generate_key` (0.12): `generate_key()` in engine/sftpd.c picks
  the call by version. An x86_64 build run under emulation (Rosetta, qemu)
  can't start an AppImage as it is ("Exec format error" on the "AI\x02"
  mark in its ELF header), so `make-appimage.sh` blanks that mark in its
  copy of linuxdeploy. The AppImage leaves out, on purpose, what every
  desktop has (X11/Wayland, FreeType, ALSA): a bare server can't start it,
  a desktop can. Tested: both start headless, render, and exit cleanly.
- **The Mac app requires macOS 27 on Apple silicon**: its Homebrew libraries
  were built for the build Mac. Building them with a lower
  `MACOSX_DEPLOYMENT_TARGET` would widen that. It is ad-hoc signed, not
  notarized: users choose "Open Anyway".

## 5c. Probe first

When a console API is in doubt -- will it link, will a title be allowed to
call it, how slow is it -- build a throwaway homebrew that tries every
candidate one at a time before touching the app (kept out of the repo). Log each call
before making it, time it, give undocumented calls oversized marker-filled
buffers, and skip on the next launch whatever crashed. Two runs of
a storage probe settled what months of theory had not (§3.3, §4.5); a USB
probe built exactly like Goldleaf proved the Switch's USB drives (§1).

## 6. Working with the maintainer

- **Ask before turning per-platform behaviour into shared behaviour**, and
  don't add features nobody asked for. When an instruction could mean "this
  platform" or "all of them", ask. (When the maintainer says "a move to apply
  to both the PS5 and Switch version", that is the answer — do both.)
- **Do what was asked, and only that.** Asked for a hello world to prove the
  toolchain, one session built the whole app as well; the maintainer's reply
  — *"I didn't ask you to build the full app and you did nonetheless"* — is
  worth remembering. A scoped request on a console project is usually a
  debugging instrument, and widening it destroys the instrument.
- **Deploying a build to the PS5 is fine. Any other write or delete on a
  console needs asking first.** Check the target folder before uploading.
  A connection refused on port 2121 usually means ftpsrv isn't running, so
  ask the maintainer to start it rather than retrying.
- **The maintainer's hardware reports outrank your theory.** Every time a
  session argued with one ("it must be an upstream bug", "loopback must not
  work"), the session was wrong.
- **Changelog and About text are for users.** No internal diagnostics,
  cosmetic tweaks or implementation names. Releases only list what users
  can notice. Diagnostics that reach `startup_trace` reach the in-app
  message log, which users read — take them out before shipping.
- **Only thank projects whose work is actually used.** The PS5 Payload SDK
  is thanked (it builds the app, its libraries and the Lapy helper).
  ShadowMountPlus isn't (it only runs the title). Lapy, ProsperoEden and
  ps5-native-app-boilerplate are always thanked on the PS5 — and **not** on
  the Switch, which thanks libnx/devkitPro, ftpd, NX-Shell, websrv/ftpsrv and
  the libraries. The About screens are deliberately different.
- The credit reads "Made by Herbrax (Simo Hakim)". The wordmark is lowercase
  `chiral-ftp` with a gold `-ftp`; the dashboard and hbmenu name is
  `Chiral-FTP`.
- The FTP server's default login is `chiral` / `ftp` on every platform
  (`engine/ftpd.h`), chosen by the maintainer. A login saved in settings
  takes precedence.
- Files written to the PS5 are 777, deliberately. The console is
  jailbroken, and that's an accepted trade-off.
- Don't claim something is tested when it is compiled. Don't write a comment
  asserting behaviour you haven't read. When asked to explain, explain
  briefly and don't start fixing things.

---

## 7. Still open (as of v1.0.1)

**PS5**

- No free space (§3.3). It needs a call a title is allowed to make.
- No crash handler in the unified tree (§3.7).

**Switch**

- **BFTTF decoding** (§4.4) — the whole of Japanese, Korean and Chinese
  depends on it, plus bundling a Noto Sans Arabic.
- **USB mass storage** via libusbhsfs — needs either a prefix-aware path
  layer in `engine/` or a mapping of `ums0:` onto a synthetic `/usb0` root,
  because of the unprefixed-paths rule in §1.
- **Save data and NAND**, the way [xftpd](https://github.com/p-sam/xftpd)
  does it: `fsdev` mounts with elevated filesystem permissions, plus
  title-ID/user-ID lookup so the listing is readable.
- **A crash handler**: libnx has `diagAbortWithResult` and Atmosphère writes
  reports to `/atmosphere/crash_reports`; worth wiring the startup log in.

**Everywhere**

- The desktop build shows key caps even when a game controller is the
  thing in use.
- The web app still says "the console" everywhere, including when the
  desktop build serves it, and it is English only. `/api/state` already
  reports `platform`, which is what it would branch on.
- The macOS menu bar is built once at startup, so changing the language in
  Settings shows in it only after a restart. In Arabic it stays English:
  the catalogue's Arabic is pre-shaped for the app's own renderer, and Cocoa
  would reverse it.
- The Switch and desktop builds leave their outputs in the repo root; they
  could go to `.build/` and `dist/`.
- `docs/PORTING.md`'s file paths predate unification (§2.3).
- About twenty mechanically-neutralised translations want a native speaker.
- **No CI.** Nothing in this repo has ever had all three platforms compiled
  by the same actor at the same time. A matrix — the devkitPro container for
  Switch, a macOS runner for the PS5, a plain runner for the desktop — is
  what would make the single tree safe, and is the most valuable thing
  anyone could add next.
