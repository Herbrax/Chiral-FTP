# How the tree is arranged

One codebase, three platforms. About nine parts in ten is shared; the rest
lives in `platform/` behind two contracts, and **nothing outside
`platform/` asks which platform it is on** — there is not a single
`#ifdef __SWITCH__` or `__PROSPERO__` in `engine/`, `web/` or `app/`.

```
engine/      FTP and SFTP, the transfer queue, the FTP server, local
             filesystem jobs, settings, logging. Plain C on POSIX.
web/         the web app: libmicrohttpd, the JSON API, the embedded pages.
app/         the UI: Dear ImGui, the software rasteriser, translations.
vendor/      Dear ImGui.
assets/      fonts and the brand.
i18n/        every string the app shows, in nine languages.
platform/
  ps5/       VideoOut, ScePad, sceImeDialog, the jailbreak elevation client
  switch/    a libnx framebuffer, the pad, swkbd, the service startup
  sdl/       a window on a computer — Linux, macOS, Windows under MSYS2
tools/       the generators, the dependency builds, the release packaging
ps5/         ps5-native-app-boilerplate, vendored, as the PS5 toolchain
```

## The two contracts

**`engine/system.h`** — storage, network and locale. Where the data folder
lives, which drives exist, how much room is left, this machine's address,
how to notify the user, the system language, which languages the build can
draw, which extra fonts to merge, the FTP server's default root, and UTC to
`time_t`. One `system.c` per platform.

**`app/backend.h`** — the screen, the pad and the keyboard. Frames in and
out, input, the native text prompt, how the app was launched, teardown. One
`backend.cpp` per platform.

**`platform/<name>/platform_config.h`** — the traits that have to be known
at compile time: the file mode, the render size, whether libmicrohttpd owns
its listen socket, whether big allocations go through `mmap`, and what the
machine is called. The Makefile puts the right directory on the include
path, so shared code writes `#include "platform_config.h"` and gets its
own.

Adding a platform means adding one directory with `backend.cpp`,
`system.c`, `platform_config.h` and `platform.mk`. Nothing else changes.

`make check` verifies that each platform defines every function in both
contracts and every trait, and that no file in `engine/`, `web/` or `app/`
mentions `__SWITCH__`, `__PROSPERO__` or `__SCE__`. It needs no console
toolchain, so it runs anywhere — which matters, because the first thing the
unification got wrong was exactly this: `early_init`, `startup_notice` and
`report_startup_services` existed for two platforms out of three, and
nothing noticed until a PS5 link failed on a machine that could do one.

## Building

```
make PLATFORM=switch      # dist/chiral-ftp/chiral-ftp.nro
make PLATFORM=ps5         # dist/PPSA96024/
make PLATFORM=sdl         # ./chiral-ftp  (the default)
make test                 # unit tests, on this computer
```

`platform/<name>/platform.mk` holds the toolchain for each: devkitPro for
the Switch, the vendored boilerplate for the PS5, pkg-config and SDL2 for
the desktop.

## Why one tree

The two separate repos drifted in both directions within days. Fixes written
on one side didn't reach the other: the rasteriser rewrite (~90 ms a frame)
is a straight win on the PS5 too, and the PS5's permission-retry wrappers
were called from five shared files that the Switch tree called raw
`rename()`/`unlink()` from. Versions diverged, and a nine-line translation
change arrived as a 787-line diff because one side had sorted the
catalogue.

The cost is that a change can break a platform you can't compile. That is
what CI is for, and it is the one piece still missing: a matrix building all
three on every push (devkitPro in a container for the Switch, a macOS runner
for the PS5, native for the desktop).
