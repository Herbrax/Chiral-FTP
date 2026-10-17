<p align="center"><img src="assets/brand/chiral-ftp-logo-dark.svg" alt="chiral-ftp" width="520"></p>

<p align="center">
  <a href="https://github.com/Herbrax/Chiral-FTP-PS5/releases"><img src="https://img.shields.io/github/downloads/Herbrax/Chiral-FTP-PS5/total?color=D9B65E&label=downloads" alt="Downloads"></a>
  <a href="https://github.com/Herbrax/Chiral-FTP-PS5/stargazers"><img src="https://img.shields.io/github/stars/Herbrax/Chiral-FTP-PS5?color=D9B65E&label=stars" alt="Stars"></a>
</p>

# chiral-ftp

**The ultimate file manager and FTP interface for PS5.** It runs as a native
homebrew title from the dashboard (via ShadowMountPlus), is used with the
controller on the TV, and works in the spirit of FileZilla.

When it opens, you choose what to do:

- **FTP server**: turn the PS5 into an FTP server. The screen shows the
  address and login; change the username, password or port, or allow
  anonymous logins. Connected clients, transfers and speed are shown live.
- **Web app**: open chiral-ftp in a browser on your PC or phone. Browse the
  PS5's files, download them, upload from the browser (drag and drop works),
  copy, move, rename and delete. An FTP client tab is also there.
- **File manager**: two panes for copying and moving files on the PS5:
  internal storage, Homebrew, USB drives, extended storage.
- **FTP / SFTP client**: connect from the PS5 to a laptop, PC or NAS and
  transfer both ways. Saved sites and recent connections, resume, and up to
  30 simultaneous transfers (5 by default).

The servers keep running while you use another mode or go to the PS5 home
screen. Closing chiral-ftp stops them.

It speaks **nine languages**: English, Spanish, French, Italian, Japanese,
Korean, Chinese, Russian and Arabic (Settings → Language; "Automatic" follows
the PS5's system language).

| | |
|---|---|
| ![Start screen](docs/screenshots/start.png) | ![FTP server](docs/screenshots/ftp-server.png) |
| ![Web app](docs/screenshots/web-app.png) | ![About](docs/screenshots/about.png) |

What's new in each version is in [CHANGELOG.md](CHANGELOG.md).

## Requirements

- A jailbroken PS5 with **ShadowMountPlus** and an **ELF loader (elfldr)
  listening on port 9021** when chiral-ftp starts. chiral-ftp uses it to
  get full access to the console's storage (see Lapy under
  [Thanks](#thanks)); without it, the app only sees its own sandbox.
- Tested on PS5 firmware **13.60**. Other firmware may work but hasn't been
  tested.

## Install

1. Download `chiral-ftp-1.0.zip` from the releases and unpack it. It
   contains the folder `PPSA96024`.
2. Copy that folder to `/data/homebrew/PPSA96024` on the PS5.
3. Make sure the folder and **every file in it are 777**. The PS5 refuses to
   start a title otherwise ("can't start the game or app"). The zip stores
   all modes as 777, but some FTP clients don't keep them.
4. Start chiral-ftp from the dashboard. The start screen should say "Full
   access to the PS5's storage".

**Updating:** close chiral-ftp completely before you replace its files. Most
FTP clients overwrite `eboot.bin` in place, and the console silently refuses
that while the title is in use. Upload it under another name and rename it
over the old one, or use `make deploy` (see below).

chiral-ftp keeps its settings, saved sites and history in `/data/chiral-ftp`,
so updates keep them.

## Using it

- **D-pad / sticks** move, **Cross** opens or selects, **Circle** goes back.
- **OPTIONS** opens the menu of every screen; its first item, **Start
  screen**, always brings you back. On the start screen it opens Settings and
  About.
- In the file manager and the client: **Square** marks, **Triangle** copies
  (file manager) or uploads/downloads (client) the marked items, and **R1**
  switches sides. In the client, **Touchpad** shows the transfer queue.
- Every file chiral-ftp writes to the PS5 is set to 777. Files it uploads to
  a server are set to 777 there too, when the server allows it.

**Security.** The FTP server and the web app run on your local network. The
FTP server's default login is `ps5` / `chiral`; change it. The web app has no
login: anyone on your network can use it while it runs, so stop it when
you're done. With full storage access, both can change anything on the
console.

## Building

chiral-ftp is built on macOS (Linux should work with minor changes) with the
tooling of
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate),
which is included in `ps5/`.

1. Install the tools: `brew install llvm@18 coreutils gnu-sed bash cmake sdl2 libmicrohttpd`.
2. Build the boilerplate's tooling once: `cd ps5 && make` (on macOS, the
   wrappers in `ps5/.hostbin` and the Homebrew GNU tools must be on `PATH`).
3. `make ps5` builds `dist/PPSA96024/` and `dist/chiral-ftp-1.0.zip`. The
   first run fetches and builds mbedTLS, libssh2 and libmicrohttpd for the
   PS5 (checksums pinned in `tools/build-ps5-deps.sh`), and the Lapy helper.
4. `make deploy PS5_HOST=<PS5 IP>` sends the build to the PS5 through an FTP
   server that runs outside the app's sandbox (ftpsrv on port 2121). Only
   changed files are sent, each is verified and renamed into place, and
   everything is set to 777.

`make host` builds the same app as a window on your computer (keyboard or a
game controller), which is how most of the UI is tested; `make test` runs the
unit tests. `tools/render-icon.sh` regenerates the dashboard icon from
`assets/brand/chiral-ftp-icon.svg`, and `tools/render-background.sh` the
dashboard background (`sce_sys/pic0.dds`, `pic1.dds`) from
`assets/brand/background.html`.

**Releases:** `make release` builds the package and writes `release/`: the
zip, `SHA256SUMS`, and `RELEASE_NOTES.md` (this version's section of
CHANGELOG.md plus install steps and the checksum), ready for a GitHub release.

**Translations** live in [i18n/translations.json](i18n/translations.json):
every string the app shows, in English and the eight other languages. The
build refuses a string with no entry, a missing language, or a translation
whose placeholders (`{0}`, `%s`) differ from the English. Corrections from
native speakers are very welcome.

## Bugs and contributions

Found a bug? Please [open an issue](https://github.com/Herbrax/Chiral-FTP-PS5/issues). Say what you did, what happened,
your firmware and jailbreak, and if possible the contents of
`/data/chiral-ftp/startup.log`. [Pull requests](https://github.com/Herbrax/Chiral-FTP-PS5/pulls) are welcome.

## Thanks

chiral-ftp stands on the work of others. Thank you:

- **[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)**
  by BlackBearReloaded. It is how chiral-ftp exists as a native dashboard
  title at all. We use its toolchain unchanged (included in `ps5/`): the
  compiler wrapper, the link script and program startup code, the tool that
  converts and signs `eboot.bin`, and its clean-room `libc.prx` runtime,
  which ships in `sce_module/`. The code that sets up the TV display follows
  its VideoOut example, and its notes on what a sandboxed title may and may
  not do (sockets, `fcntl`, name lookups) saved us a lot of trial and error.
- **[ProsperoEden](https://github.com/blackbearreloaded/ProsperoEden)** by
  BlackBearReloaded and contributors. Its storage access client
  (`app/elevation/`) is used as is, with one change: chiral-ftp also checks
  that the app really became root before trusting what it can see. We also
  follow its approach to listing folders on the PS5 (reading directories with
  `sceKernelGetdents` and a large buffer), its link recipe for the C++
  runtime, and how it talks to the system keyboard.
- **[PS5-Lapy-JB-Daemon](https://github.com/ArkSama/PS5-Lapy-JB-Daemon)**
  (Lapy) by ArkSama, with the cooperative one-shot helper from
  [mpereiraesaa's fork](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon),
  pinned through BlackBearReloaded's fork. The packaged `lapy.elf` is built
  from their unmodified source; at startup chiral-ftp sends it to the ELF
  loader, and it gives the app access to the console's real storage. That is
  what lets the file manager, the FTP server and the web app reach your
  files.
- **[websrv](https://github.com/ps5-payload-dev/websrv)** by John Törnblom.
  The web app's embedded-file server comes from it: `web/asset.c`,
  `web/asset.h` and `tools/gen-asset-module.py`, which turn the web pages
  into C arrays served from memory (we added compressed files). Our
  libmicrohttpd setup follows websrv and
  [ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager).

Also: [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) by John Törnblom,
whose command set our FTP server follows (our code is written separately);
[FileZilla](https://filezilla-project.org), whose behaviour the client
follows; [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus), which
runs chiral-ftp on the console; the
[PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk); and the libraries
built into the app: libssh2, Mbed TLS, GNU libmicrohttpd, Dear ImGui, Noto
Sans and IBM Plex Mono.

## License

chiral-ftp is free software under the GNU General Public License v3.0 or
later; see [LICENSE](LICENSE). Files taken from other projects keep their
original copyright headers. The licenses of everything bundled in the app are
in its `licenses` folder (GPL-3.0, LGPL-2.1, MIT, BSD, Apache-2.0, OFL).

Made by **Herbrax** (Simo Hakim): [simohakim.com](https://simohakim.com) ·
[LinkedIn](https://www.linkedin.com/in/simo-hakim/)
