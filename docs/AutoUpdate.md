# Auto-update

How chiral-ftp finds out about a new version, and how each platform puts it
in place. The limits of each are listed too. Windows, Mac, Linux and
Android are built. Switch and PS5 are planned, and their sections say how
they would work and what stands in the way.

The code: `app/update.h` and `app/update.cpp` (shared),
`platform/sdl/update.cpp` with `platform/sdl/macos.mm` (the computers), and
`platform/android/update.cpp` with `ChiralActivity.java` (Android). The
working notes for whoever changes it are in `docs/AGENT-NOTES.md` §5b3.

## Contents

- [What every platform shares](#what-every-platform-shares)
- [Windows](#windows)
- [Mac](#mac)
- [Linux](#linux)
- [Nintendo Switch (planned)](#nintendo-switch-planned)
- [PS5 (planned)](#ps5-planned)
- [Android](#android)
- [Summary](#summary)

## What every platform shares

### The check

1. **At startup**, on a background thread, the app sends
   `POST https://chiral-ftp.com/api/v1/check` with
   `{"platform":"windows-x64","version":"1.2","device":"<32 hex>"}`, and
   waits 8 seconds at most.
2. **The worker** answers from `latest_version.json`. That file is on the
   `release-data` branch, and `tools/latest-version.py` rebuilds it every 6
   hours and whenever a release changes.
3. **The app checks the answer** rather than trusting it. The answer must say
   `"update":true`. Its version must be newer by the numbers (1.10 > 1.9).
   The URL must start with
   `https://github.com/Herbrax/Chiral-FTP/releases/download/` and end with
   the file name. The file name must be plain, with no folders in it.
4. **If all of that holds**, a dialog offers **Update**, **Not now** or
   **Skip this version**. Anything else (an error, no network, nothing
   newer) shows nothing, unless the user pressed **Check now** in Settings.

### What is remembered

`update.json` in the data folder keeps these, out of the web app's reach:

| Key | What it holds |
|---|---|
| `device` | 16 random bytes, made on first start. |
| `check` | The Settings switch. |
| `skip` | The version "Skip this version" was pressed for. |
| `snooze_until` | "Not now": quiet for 7 days. |

**Check now** ignores `skip` and `snooze_until`.

### Test builds

A version containing `-dev`, `-beta` or `-alpha` (such as `1.2-beta`) never
checks. Its Settings switch is greyed out and says why.

### The download and the swap

1. The new file is downloaded next to the current copy, so the last step is
   a rename on the same drive.
2. Its size must match the size in the answer.
3. The platform puts the new copy in place.
4. The app quits the usual way: the servers stop and the threads end.
5. After that, it starts the new copy.

### A file named for its version

A file named for its version (`chiral-ftp-1.2-windows-x64.exe`) takes the
new file's name (`chiral-ftp-1.3-windows-x64.exe`), so the name doesn't go
on saying 1.2. Any other name is kept.

### Limits on every platform

**What the download is trusted on**
- **No checksum or signature is checked.** The download is trusted because
  it comes over HTTPS, from the project's GitHub releases URL, at the
  expected size. Someone who can upload to those releases, or change the
  worker *and* the releases, could ship a file the app would install.
  Adding `sha256` to `latest_version.json`, and checking it, would close
  part of that. Signing the releases would close the rest.

**Which version is offered**
- **`latest_version.json` takes each platform's file from the most
  recently published release, by date, not by version number.** Drafts and
  pre-releases are skipped.
  - **A test version published as an ordinary release breaks offers.**
    Publish 0.9 that way, and the json lists 0.9 for its platforms. Everyone
    on 1.1 is then offered nothing, and the 0.9 copies are offered nothing
    either.
  - **Fix:** keep test builds as workflow artifacts, drafts or pre-releases.
  - **An older patch published after a newer release is never offered** to
    anyone already on the newer one, because the app refuses older versions.
    It also hides the newer release from those who still need it, until
    another release comes out.
- **A new version reaches people only once the json has been rebuilt**: at
  a release event, or at most 6 hours later.

**When the check happens**
- **One check per launch.** A copy left running for weeks hears about a new
  version only when it restarts, or when someone presses **Check now**.
- **Any failure is silent at startup.** If the worker is down or blocked,
  users simply aren't told. Only **Check now** says "Could not check".
- **"Skip this version" remembers one version.** The next version is
  offered as usual.

**What the update does to a running copy**
- **Updating restarts the app.** Running FTP/SFTP sessions, web app
  sessions and transfers are cut. The dialog says so.
- **Quitting during a download cancels it**, and the part already
  downloaded is deleted. Quitting while the new copy is being put in place
  waits for that to finish, so a copy is never left half replaced.

**Counting devices and renamed files**
- **Device counting is approximate.** The ID belongs to a data folder, not
  to a machine. Each of these counts as a new device:
  - a portable copy with its own `data` folder (Windows);
  - a reinstall after the settings were deleted;
  - a second user account.
- **A rename for the new version breaks pinned shortcuts.** This happens to
  a file named for its version: a taskbar pin on Windows, or a Linux desktop
  entry that points to the old name. Copies whose names have no version in
  them keep their names.

## Windows

**How it works**
1. **Where it is:** `GetModuleFileNameW`, the running `.exe`, wherever it
   is: Downloads, the Desktop, a USB stick or a network share.
2. **Can it be replaced?** A test file is created next to it. If that fails,
   the dialog's main button becomes **Download page**. This happens, for
   instance, under Program Files without admin rights.
3. **Download:** WinHTTP over TLS 1.2 or 1.3, following GitHub's redirect.
   It uses the system's proxy settings, and is written to
   `<new name>.download` next to the `.exe`.
4. **Swap:** a running `.exe` can't be deleted or overwritten, but it can
   be renamed.
   1. The current file becomes `<name>.old`.
   2. The download takes its place, under the new name or the old one.
   3. If that second step fails, the `.old` is renamed back.
5. **Restart:** at exit, `CreateProcessW` starts the new copy with
   `--updated-from <name>.old`. The new copy deletes the `.old` once the old
   process has gone. It tries for up to 10 seconds, and only deletes a
   `.exe.old` in its own folder.

**Limits**
- **No elevation.** A copy in a folder that needs admin rights to change
  sends the user to the download page; it doesn't ask for UAC.
- **Antivirus software** can lock or quarantine a freshly written `.exe`.
  The swap then fails, the old copy is put back, and the user sees "could
  not put the new version in place".
- **The `.old` file stays behind if:**
  - the new copy never starts;
  - or the update goes to a version without this code (1.1 and older).
  It does no harm, and the next update replaces it.
- **Windows on ARM: a copy can be the wrong build for its PC.** An x64
  copy runs on an ARM PC through Windows' emulation. Inside it,
  `GetNativeSystemInfo()` and `PROCESSOR_ARCHITECTURE` both say x64, and
  the program's own build says x64 too, so neither can be trusted.
  - **How the hardware is found:** `IsWow64Process2`, which reports the real
    machine to an emulated program as well.
  - **What the check sends:** `"platform":"windows-arm64"` (the machine),
    plus `"build":"windows-x64"` (the copy) when the two differ. The worker
    ignores the extra field today.
  - **An x64 copy on an ARM PC** takes the ARM `.exe` as soon as one is
    offered. It takes it even at the same version, but only when the worker
    answers `"update":true`. It still takes newer x64 releases while none
    has an ARM `.exe`, since `latest_version.json` falls back to x64 for
    `windows-arm64`.
  - **An ARM copy never trades itself for the x64 program**, even a newer
    one from a release without an ARM `.exe`. It waits for the next ARM
    build.
  - **For the worker to move a copy at the same version:** when `build`
    differs from `platform`, answer `"update":true` with the platform's
    latest file even if its version equals the one sent, as long as that
    file is for the platform (`-windows-arm64.exe`). Without that change,
    the move happens at the next release.
- **Proxies that need a login** aren't supported. The check fails silently.
- **Two windows of the same `.exe`:** the other one keeps running the old
  code until it is closed.
- **SmartScreen and Mark of the Web:** the downloaded file has no "came from
  the internet" mark, because the app wrote it itself. The first start after
  an update shows no SmartScreen prompt, unlike a copy downloaded by hand.

## Mac

**How it works**
1. **Where it is:** `[NSBundle mainBundle]`, the running `.app`, wherever
   it is: `/Applications`, `~/Applications`, the Desktop or anywhere else.
2. **Can it be replaced?** Both the bundle and the folder it is in must be
   writable. If not, the dialog offers the download page.
3. **Download:** `NSURLSession`. It needs the release's `chiral-ftp.app.zip`
   and is saved as `.chiral-ftp-update.zip` next to the bundle.
4. **Swap:**
   1. `ditto -x -k` unpacks the zip into `.chiral-ftp-update/` beside the
      bundle.
   2. The new bundle must contain `Contents/MacOS/chiral-ftp`.
   3. Everything in `Contents/MacOS` is made executable again, and the
      quarantine attribute is removed. A zip made off a Mac (v1.1's was made
      on Windows) has no Unix permissions. Without the first step, the app
      comes out "can't be opened".
   4. The old bundle becomes `.chiral-ftp-old.app`, and the new one takes its
      name.
   5. If the second rename fails, the old bundle is put back.
5. **Restart:** at exit, the old bundle and the unpacking folder are
   deleted, and `open -n` starts the new app.

**Limits**
- **Opened from Downloads without being moved:** macOS runs the app from a
  hidden, read-only copy (App Translocation). It can't be replaced there,
  and the dialog says to move chiral-ftp to Applications first.
- **Standard (non-admin) accounts usually can't write to `/Applications`**,
  so they get the download page.
- **Only a `.zip` updates in place.** `latest_version.json` prefers a
  `.dmg` to a `.app.zip` when a release has both. Upload a `.dmg` alongside
  the zip, and every Mac gets the download page instead. Either keep
  releasing the zip alone, or put `*.app.zip` first in
  `tools/latest-version.py`.
- **Permissions may be asked again after each update.** The app is
  ad-hoc signed, so each build has a different code identity, and macOS can
  ask again for permissions it ties to that identity. These are the Local
  Network permission (macOS 15 and later) and the firewall's "accept
  incoming connections". Signing with a Developer ID would keep them.
- **Running from a read-only volume** (a mounted disk image) is treated as
  not writable.
- **Not yet run on a Mac.** The Mac code compiles in its C++ parts, but the
  Objective-C (`macos.mm`) has only been read, not built or run. The first
  GitHub build and a real update test are still to do.

## Linux

**How it works**
1. **Where it is:** `$APPIMAGE`, which the AppImage runtime sets to the
   `.AppImage` file. There's nothing to search for.
2. **Can it be replaced?** The file and its folder must be writable.
   Without `$APPIMAGE` (a plain build, or a copy from a distribution
   package), the dialog offers the download page.
3. **Download:** the system's `curl`, or `wget` if there is no curl. They
   run with a clean `LD_LIBRARY_PATH`, so the AppImage's own libraries don't
   get in their way. Nothing extra is linked into the app. The file is saved
   as `.<new name>.download` next to the AppImage.
4. **Swap:** the file is made executable (755) and renamed over the old
   AppImage, or to the new name, with the old one deleted. The running copy
   stays mounted until it exits.
5. **Restart:** at exit, the new AppImage is started, detached from the old
   process.

**Limits**
- **Only the AppImage updates itself.** A build run as `./chiral-ftp`, or
  any packaged copy, gets the download page.
- **Needs `curl` or `wget`.** Some minimal systems have neither, and the
  check then fails silently. Ubuntu desktop ships `wget`.
- **AppImage integration tools** (AppImageLauncher, Gear Lever) may still
  point at the old file name after a rename for the new version. They
  usually notice and integrate the new file.
- **The new AppImage needs FUSE to start**, just as the old one did.
- **Architecture is fixed at build time.** An x86_64 AppImage asks for
  `linux-x86_64` and an ARM one for `linux-aarch64`.
- **Not yet run on Linux.** The check, the download, cancelling and the swap
  were run on Windows through MSYS2's POSIX layer, with real `curl`. A real
  AppImage update on a Linux machine is still to do.

## Nintendo Switch (planned)

**How it would work**
1. **Where it is:** hbmenu passes the `.nro`'s own path in `argv[0]`
   (`sdmc:/switch/chiral-ftp.nro`), so there's nothing to search for.
2. **Download** into `chiral-ftp.new.nro` beside it.
3. **Swap:** rename the old file to `chiral-ftp.old`, then the new one to
   `chiral-ftp.nro`.
4. **Restart:** `envSetNextLoad()` can ask hbloader to start the new `.nro`
   as chiral-ftp exits. Otherwise, the user is asked to start it again from
   hbmenu.
5. **Clean-up:** the new version deletes the `.old`.

**Limits**
- **HTTPS has to be added.** The Switch build has no HTTPS client. devkitPro
  has `switch-curl` and `switch-mbedtls`, which would have to be linked, and
  that adds size.
- **Memory.** Started from the album, the app runs as an applet, with
  little memory. The download must be written to the SD card as it comes,
  never held in memory.
- **The SD card is FAT32 or exFAT,** where a rename won't replace an
  existing file. The old file has to be moved out of the way first (as in
  `engine/settings.c`). A power cut between the two renames would leave no
  `chiral-ftp.nro`. The next start must then notice the `.new.nro` or
  `.old` and finish the job, or put the old one back.
- **The `.nro` file name is the same in every release** (`chiral-ftp.nro`),
  so the version-rename rule never applies here.
- **Switch 2:** `latest_version.json` has a `switch2` key but no build.
  Until there is one, a Switch 2 asks as `switch`.

## PS5 (planned)

**How it would work**
1. **Where it is:** a title can't easily tell which folder it was started
   from, so it searches:
   - `/data/homebrew`;
   - the root of every drive the app already lists (internal storage, USB,
     M.2 SSD);
   - two folders deep at most in each.

   A copy is any folder whose `sce_sys/param.json` has `titleId` set to
   `PPSA96024`, whatever the folder is called. Folders named `PPSA96024-2`
   and `PPSA96024-old` (an update's leftovers) are ignored.
2. **Duplicates:** two copies or more means "Duplicate copies found, update
   impossible". Nothing is touched.
3. **Download** the release zip, the way ProsperoStore does on an
   elevated title:
   - **Transport:** PacBrew's libcurl with OpenSSL (`PACBREW_PACKAGES +=
     libcurl`). The console's own `sceHttp`/`sceSsl` rejects every public
     site once a title is elevated (`0x8095f00c`), and chiral-ftp elevates
     through Lapy.
   - **Shims:** `ps5/examples/update-check/console_curl.c`, with
     `APP_WRAP_SYMBOLS += fcntl`. It provides name lookup on
     `sceNetResolver`, the `fcntl` fix and non-blocking sockets.
     `console_curl_setup()` goes on every curl handle.
   - **Certificates:** checked against the console's own list,
     `/system/common/cert/CA_LIST.cer`, passed as `CURLOPT_CAINFO`.
   - **Settings:** HTTPS only, IPv4, HTTP/1.1, a 15 s connect timeout, and
     a stall limit (`CURLOPT_LOW_SPEED_LIMIT 1` for 20 s) rather than a
     total time.
   - **Redirects** are followed by hand (curl's `FOLLOWLOCATION` off), as
     ProsperoStore does, and only to GitHub's release file host.
   - **Where it goes:** the zip is written to the same drive as the copy as
     it arrives, never held in memory.
   - **Unpacking:** miniz, as ProsperoStore does, into `PPSA96024-2` beside
     the copy. Refuse absolute paths, `..` and links, and require the
     unpacked `sce_sys/param.json` to name `PPSA96024`.
4. **Set the permissions explicitly:** `chmod 0777` on `PPSA96024-2` itself
   and on every folder and file inside it. Don't rely on the modes stored in
   the zip. Then check them all before going on; the console won't start a
   title whose folder or files aren't 777. This is the PS5 version of the Mac
   lesson: v1.1's Mac zip had no Unix modes, and the updated app "can't be
   opened".
5. **Swap:** rename `PPSA96024` to `PPSA96024-old`, then `PPSA96024-2` to
   `PPSA96024`.
6. **Restart:** ask the user to close chiral-ftp and open it again. A title
   can't restart itself.
7. **Clean-up:** the new version deletes `PPSA96024-old` once it has started
   successfully.

**Limits**
- **Renaming the folder of the running title may not be possible.**
  ShadowMountPlus may hold it open or refuse the rename. Try it with a
  throwaway title first. If it refuses, the swap has to happen on the next
  start, before anything from the folder is loaded, or from another title.
- **The renaming is ours alone.** ProsperoStore and the boilerplate's
  self-update kit avoid renaming the running title's folder. They swap the
  files inside it from a helper program after the app has closed, because
  the console's mount points at that folder. Renaming is the maintainer's
  chosen plan; test it on a console first.
- **New code in the PS5 build:** libcurl, OpenSSL and their
  dependencies (and their licence notices), `console_curl.c`, and miniz.
  `tools/build-ps5.sh` links by hand, so the PacBrew archives and the
  `--wrap=fcntl` have to be added there, not only to the boilerplate's
  Makefile.
- **Space for two copies:** the old and the new copy exist side by side
  until clean-up, so the drive needs room for both.
- **exFAT USB drives** have the same no-replace rename behaviour as the
  Switch's SD card. And a power cut mid-swap can leave only `-old` and `-2`.
  The next start must notice that and finish the swap.
- **The dashboard entry and the homebrew catalog** use `contentVersion` and
  the title ID. The version shown on the dashboard updates only when the
  new folder's `param.json` is read, which may take a restart of the
  console.
- **The search is slow on big drives.** It goes two levels deep on every
  drive, so it should run in the background, and only when an update is
  accepted, not at every start.

## Android

**How it works**
1. **The check and the download** go through Java's `HttpsURLConnection`
   (`ChiralActivity.updatePost` and `updateDownload`), called over JNI from
   `platform/android/update.cpp`. The download follows GitHub's redirect to
   its file host, and reports each block to `updateProgress()`, which is
   how the dialog shows progress and how Cancel stops it.
2. **Where it goes:** `chiral-ftp-update.apk` in the app's private data
   folder. It is deleted at the next start, whether the update was installed
   or turned down.
3. **Install:** the APK goes to Android's own installer, the same way the
   file manager's Open hands it an `.apk` (`backend::open_file`, through the
   app's file provider). The dialog then says to confirm there.
4. **Restart:** Android closes chiral-ftp while it replaces it, and its
   installer offers **Open** when it's done.

**Limits**
- **"Install unknown apps" must be allowed for chiral-ftp.** Android asks
  the first time, and offers the setting. The manifest already declares
  `REQUEST_INSTALL_PACKAGES`, which only makes the setting available.
- **The signature must match.** Android installs over the existing app only
  if both APKs are signed with the same key.
  - **Which key:** release APKs are signed with the keystore in the
    `ANDROID_DEBUG_KEYSTORE` GitHub secret. Builds made on the maintainer's
    PC use `~/.android/debug.keystore`, which is the same key if the secret
    was made from that file.
  - **Anything else fails** with "App not installed": an APK signed with
    another key, or a copy installed from somewhere else.
- **`versionCode` may not go down.** It is made from the version (1.2
  becomes 1002000), so a newer release always has a higher one.
- **Nothing is offered until a release has an APK.** `latest_version.json`
  lists no Android file yet.
- **Play Protect** may warn about, or slow down, installs of apps that
  aren't from the Play Store.
- **Android TV:** the update dialog is drawn in the TV layout, but the
  system installer may be hard to reach with a remote on some TVs.
- **Not yet run on a device.** It builds, and the native callback is
  exported for all three architectures, but no update has been installed
  on a phone or TV yet.

## Summary

| | Finds itself by | Download | Swap | Restart | Main limits |
|---|---|---|---|---|---|
| **Windows** | its own `.exe` path | WinHTTP | rename running `.exe` to `.old` | starts new `.exe`, which deletes `.old` | no UAC; antivirus locks; leftover `.old` from 1.1; same-version move to ARM needs the worker |
| **Mac** | its own bundle | NSURLSession, `.app.zip` | unzip beside, swap bundles | `open -n` | App Translocation; non-admin; `.dmg` blocks it; permissions re-asked; not yet run on a Mac |
| **Linux** | `$APPIMAGE` | `curl` or `wget` | one rename | starts new AppImage | AppImage only; needs curl or wget; not yet run on Linux |
| **Switch** | `argv[0]` | to add (`switch-curl`) | `.new.nro`, `.old`, rename | `envSetNextLoad` or by hand | FAT renames; applet memory |
| **PS5** | search, two levels deep | PacBrew libcurl, then miniz | `PPSA96024-2` to `PPSA96024` | user reopens | renaming the running title; no unzip yet; duplicates |
| **Android** | n/a (system installer) | `HttpsURLConnection` | Android's installer | its **Open** button | unknown-sources setting; same signing key; not yet run on a device |
