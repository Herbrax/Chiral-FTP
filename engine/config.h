#pragma once

#define APP_NAME "chiral-ftp"

/* Files chiral-ftp writes to PS5 storage (uploads, downloads, copies) are
   open to all: the console only starts homebrew whose files are 777. */
#define PS5_FILE_MODE 0777

/* Transfer slots; each one logs in with its own control connection. */
#define MAX_TRANSFER_CONNECTIONS     30
#define DEFAULT_TRANSFER_CONNECTIONS 5

#define MAX_HISTORY 30

/* Where sites, history, settings, host keys and logs live. On the PS5 this is
   /data/chiral-ftp when ShadowMountPlus gives the app /data, otherwise the
   app's own /download0 storage. Chosen once by data_dir_init(). */
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
