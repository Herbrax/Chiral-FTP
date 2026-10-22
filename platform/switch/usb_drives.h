#pragma once

/* USB drives on the Switch (usb_drives.c): libusbhsfs mounts each one as
   "ums0:", "ums1:"...; the app sees them as /usb0, /usb1... like any other
   folder, so every path stays unprefixed (docs/AGENT-NOTES.md, 1). */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* From backend::early_init(), before anything opens a file: starts
   libusbhsfs and puts the router in front of the SD card. Never fatal. */
void usb_drives_start(void);
/* From backend::shutdown(), after every thread that could touch a file is
   gone. The router stays: a path under /usbN just stops existing. */
void usb_drives_stop(void);

/* The drives mounted now, for the drive list: "/usb0" and what to call it. */
typedef struct usb_drive {
  char path[16];
  char label[96];
} usb_drive_t;
int usb_drives_list(usb_drive_t *out, int max);

/* 0..N for a path under /usbN, -1 for anything else (the SD card). */
int usb_drives_index(const char *path);

#ifdef __cplusplus
}
#endif
