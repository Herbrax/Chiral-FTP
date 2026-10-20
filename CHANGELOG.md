# Changelog

## 1.0.1

chiral-ftp is now cross-platform. The same app runs on the **PS5**, the
**Nintendo Switch** (new) and on **PC and Mac**, from one codebase. Support
for Android, PS4 and other platforms is coming soon.

### New: Nintendo Switch

The first release for the Switch, as homebrew (`.nro`) started from hbmenu.
Everything the PS5 version does is there:

- **FTP server**, **web app**, **file manager** (the SD card) and
  **FTP / SFTP client** with saved sites, history, resume and up to 30
  transfers at once.
- Controls follow the PS5 layout by position: **A** opens, **B** goes back,
  **Y** marks, **X** transfers, **R** switches sides, **Plus** opens the
  menu and **Minus** shows the transfer queue. The bottom bar shows the
  Switch's own buttons.
- **The touch screen works**: tap a row, a card, a menu item or a setting,
  drag a list to scroll, tap outside a menu to close it.
- Text is typed with the system keyboard.
- Sleep is held off and the HOME menu can't close the app while it runs,
  so a transfer isn't cut short.
- The start screen says whether the app got the console's full memory
  (launched over a game, holding **R**) or the album's smaller share.
- English, Spanish, French, Italian and Russian.
- Settings, saved sites and history are kept in `/switch/chiral-ftp`.

### New: PC and Mac

The same app in a window on Windows, macOS or Linux, with the keyboard or
a game controller. There's no download yet: build it from source with
`make PLATFORM=sdl` (see the README).

### PS5

- **Creating, renaming, moving, deleting and uploading files no longer fail
  on folders the app isn't allowed to change** (755 or 766 ones, for
  example): chiral-ftp sets the folder to 777 and tries again. This works in
  the file manager, the web app, the FTP server and the client.
- **Change permissions works again.**
- **New folder and rename work in the two-pane file manager**: the PS5
  keyboard is loaded before the app asks for full storage access. If the
  keyboard still can't open, the app says so instead of doing nothing.
- **Hold Circle** to go back to the start screen from anywhere.
- **Faster drawing**: menus and lists respond more quickly to the
  controller.
- The FTP server notices when its connection died while the console slept,
  instead of claiming to still be running.
- In the client, **Disconnect** is on the server pane while connected.

### Everywhere

- The About screen says which platforms chiral-ftp runs on, and thanks the
  projects each version is built with.

### Downloads

- `chiral-ftp-ps5-1.0.1.zip`: the PS5 title (`PPSA96024`). Close chiral-ftp
  before you replace it; your settings in `/data/chiral-ftp` are kept.
- `chiral-ftp.nro`: the Switch homebrew. Copy it into `/switch` on the SD
  card; to update, replace it.

## 1.0

The first release of chiral-ftp: the ultimate file manager and FTP interface
for PS5. It runs as a native homebrew title from the dashboard (via
ShadowMountPlus) and is used with the controller on the TV.

### What's in it

- **FTP server.** Other devices connect to the PS5 with FileZilla, a file
  manager or any FTP app. The screen shows the address and login; change the
  username, password or port, or allow anonymous logins. Live activity:
  connected clients, transfers and speed.
- **Web app.** Open the address it shows in a browser on your PC or phone:
  browse the PS5's files, download them, upload from the browser (drag and
  drop works), copy, move, rename and delete. The FTP client is there too.
- **File manager.** Two panes for copying and moving files on the PS5:
  internal storage, Homebrew, USB drives and extended storage. Copies run in
  the background with progress; a folder is deleted with everything inside.
- **FTP / SFTP client.** Connect to a laptop, PC or NAS and transfer both
  ways: saved sites and recent connections, resume, and up to 30 transfers
  at once (5 by default).
- **Full storage access.** At start, chiral-ftp asks the jailbreak for access
  to the console's real storage (OnionHEN, a resident Lapy, or the packaged
  Lapy helper sent to elfldr on port 9021), the way ProsperoEden does.
- **Every file chiral-ftp writes to the PS5 is 777**, so homebrew you copy
  starts. Files it uploads to a server are set to 777 there too when the
  server allows it.
- **Nine languages:** English, Spanish, French, Italian, Japanese, Korean,
  Chinese, Russian and Arabic, with a language selector in Settings
  ("Automatic" follows the PS5's system language). Japanese, Korean, Chinese
  and Arabic use the PS5's own system fonts.
- Servers and transfers keep running while you use another mode or go to the
  PS5 home screen.

### Requirements

- A jailbroken PS5 with ShadowMountPlus, and an ELF loader (elfldr) on port
  9021 when chiral-ftp starts.
- Tested on PS5 firmware 13.60.

### Known limitations

- Closing chiral-ftp stops its servers and transfers.
- There is no SFTP server yet (the FTP server is plain FTP).
- The web app has no login: anyone on your network can use it while it runs.
- The technical message log and the web pages are in English.
- Arabic text reads right to left, but the screen layout stays left to right,
  and Arabic file names aren't joined.
- Free space on drives isn't shown.
