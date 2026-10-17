# Changelog

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
