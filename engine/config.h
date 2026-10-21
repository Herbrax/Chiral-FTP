#pragma once

#define APP_NAME "chiral-ftp"

/* CHIRAL_FILE_MODE, and the other traits that differ per console, come from
   platform/<name>/platform_config.h. */
#include "platform_config.h"

/* Uploads into a PS5 homebrew folder -- any remote path with /data/homebrew
   in it, whichever server is on the other end (chiral-ftp, ftpsrv, ...) --
   end up 0777, folders as well as files: the console won't start a title
   whose folder or files aren't. Every build, whatever it runs on; set to 0
   here to build without it. */
#ifndef CHIRAL_HOMEBREW_777
#define CHIRAL_HOMEBREW_777 1
#endif

/* Transfer slots; each one logs in with its own control connection. */
#define MAX_TRANSFER_CONNECTIONS     30
#define DEFAULT_TRANSFER_CONNECTIONS 5

#define MAX_HISTORY 30

/* Where sites, history, settings, host keys and logs live: the first of the
   platform's candidates (system_data_dir_candidates) that can actually be
   written to and listed. Chosen once by data_dir_init(). */
const char *data_dir(void);
/* Full path of a file in the data folder (static buffer per call site). */
const char *data_path(char *buf, unsigned long size, const char *name);
int data_dir_init(void);

#define SITES_NAME     "sites.json"
#define HISTORY_NAME   "history.json"
#define SETTINGS_NAME  "settings.json"
#define PREFS_NAME     "prefs.json"
#define SERVERS_NAME   "servers.json"  /* FTP server and web app settings */

#define WEB_DEFAULT_PORT 8080
#define STARTUP_NAME   "startup.log"
#define KNOWN_HOSTS_NAME "known_hosts"
