#!/usr/bin/env python3
"""A release's notes: the version's CHANGELOG section, how to install each file
the release has, and their checksums. For tools/make-release.sh and the New
Release workflow.

    release-notes.py [--allow-missing] <CHANGELOG.md> <version> <out.md> <SHA256SUMS>

--allow-missing: a version the changelog doesn't have yet gets a line to replace
instead of stopping, for a draft that a person will read before publishing.
"""
import re, sys

args = sys.argv[1:]
allow_missing = "--allow-missing" in args
if allow_missing:
    args.remove("--allow-missing")
if len(args) != 4:
    sys.exit(__doc__)
changelog, version, out, sums = args
text = open(changelog, encoding="utf-8").read()
m = re.search(r"^## " + re.escape(version) + r"\n(.*?)(?=^## |\Z)", text, re.S | re.M)
if not m and not allow_missing:
    sys.exit("CHANGELOG.md has no section for " + version)
digests = open(sums, encoding="utf-8").read().strip()
present = digests.split()
# Pictures in the changelog point into the repository; a release page needs the full address.
body = (m.group(1).strip().replace("](docs/", "](https://raw.githubusercontent.com/Herbrax/chiral-ftp/main/docs/")
        if m else "_What's new in this version: write it here before publishing._")
notes = body + "\n\n### Install\n"
if f"chiral-ftp-ps5-{version}.zip" in present:
    notes += f"""
**PS5**

1. Download `chiral-ftp-ps5-{version}.zip` below and unpack it: it contains
   the folder `PPSA96024`.
2. Copy that folder to `/data/homebrew/PPSA96024` on the PS5.
3. Make sure the folder and every file in it are 777 (the zip stores them
   that way, but some FTP clients don't keep it), then start Chiral-FTP from
   the dashboard.

Updating: close chiral-ftp first, then replace the folder. Your settings and
saved sites stay in `/data/chiral-ftp`.
"""
if "chiral-ftp.nro" in present:
    notes += """
**Nintendo Switch**

1. Download `chiral-ftp.nro` below.
2. Copy it into `/switch` on your SD card.
3. Start it from hbmenu. For the whole console's memory, hold **R** while
   launching a game and pick chiral-ftp from the menu that opens; starting it
   from the album works too, with less memory.

Updating: replace the `.nro`. Your settings and saved sites stay in
`/switch/chiral-ftp`.
"""
apks = [f for f in present if f.endswith(".apk")]
if apks:
    notes += f"""
**Android** (Android 8 or later: phones, tablets, foldables and TVs)

1. Download `{apks[0]}` below, on the device or to copy over to it.
2. Open it and install it, allowing installs from that source if Android asks.

Updating: install the new APK over the old one. Your settings stay.
"""
if "chiral-ftp.app/Contents/MacOS/chiral-ftp" in present or "chiral-ftp.app.zip" in present:
    zipped = "chiral-ftp.app.zip" in present
    notes += f"""
**Mac** ({"Apple silicon" if zipped else "macOS 27 or later, Apple silicon"})

1. Download `{"chiral-ftp.app.zip" if zipped else "chiral-ftp.app"}` below{", and unzip it" if zipped else ""}.
2. Drag `chiral-ftp.app` to Applications.
3. The app isn't notarized by Apple, so the first time macOS says it can't
   check it: open **System Settings > Privacy & Security** and choose
   **Open Anyway** (or run `xattr -dr com.apple.quarantine
   /Applications/chiral-ftp.app` once).

Updating: replace the app. Your settings stay in
`~/Library/Application Support/chiral-ftp`.
"""
appimages = sorted(f for f in present if f.endswith(".AppImage"))
if appimages:
    # x86_64 for most PCs, aarch64 for ARM ones: say which is which when both are there.
    def kind(f):
        return "most PCs" if "x86_64" in f else "ARM computers, such as a Raspberry Pi 4 or 5 on 64-bit" if "aarch64" in f else ""
    pick = (" or ".join(f"`{f}` ({kind(f)})" for f in appimages) if len(appimages) > 1 else f"`{appimages[0]}`")
    notes += f"""
**Linux** (Ubuntu 22.04, Debian 12, Fedora 36 or newer, and others like them)

1. Download {pick} below.
2. Make it executable (`chmod +x chiral-ftp-*.AppImage`, or Properties >
   Allow executing as a program) and run it.

Updating: replace the file. Your settings stay in `~/.local/share/chiral-ftp`.
"""
exes = [f for f in present if f.endswith(".exe")]
if exes:
    names = " or ".join(f"`{f}`" for f in exes)
    # With both, say which is which; with x64 alone, that it covers ARM PCs too.
    which = ("(arm64 for a Windows on ARM PC, x64 for any other)" if len(exes) > 1
             else "(it runs on Windows on ARM PCs too, through Windows' own x64 emulation)" if "x64" in exes[0]
             else "")
    notes += f"""
**Windows** (Windows 10 or 11)

1. Download {names} below {which}.
2. Run it; nothing needs installing. The program isn't signed, so the first
   time Windows SmartScreen says it doesn't recognise it: choose
   **More info > Run anyway**. When Windows Firewall asks, allow private
   networks, so that a console or phone can reach the server.

Updating: replace the `.exe`. Your settings stay in `%APPDATA%\\chiral-ftp`.
To keep everything on a USB stick instead, settings included, make a folder
named `data` next to the `.exe`: chiral-ftp then keeps its settings there.
"""
notes += f"""
### Checksums

```
{digests}
```

Made by Herbrax (Simo Hakim). Bugs and pull requests are welcome:
https://github.com/Herbrax/chiral-ftp/issues
"""
repo = "Herbrax/chiral-ftp"
badges = (f"[![Downloads](https://img.shields.io/github/downloads/{repo}/v{version}/total?color=D9B65E&label=downloads)]"
          f"(https://github.com/{repo}/releases/tag/v{version}) "
          f"[![Stars](https://img.shields.io/github/stars/{repo}?color=D9B65E&label=stars)]"
          f"(https://github.com/{repo}/stargazers)")
open(out, "w", encoding="utf-8").write(f"# chiral-ftp {version}\n\n{badges}\n\n" + notes)
