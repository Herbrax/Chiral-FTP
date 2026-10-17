#pragma once

/* FTP server (option 1 on the start screen), on threads inside the app. */

#include <limits.h>

#define FTPD_DEFAULT_PORT 2121
#define FTPD_DEFAULT_USER "ps5"
#define FTPD_DEFAULT_PASS "chiral"
#define FTPD_MAX_CLIENTS 30

typedef struct ftpd_config {
  unsigned short port;
  int anonymous;  // also accept "anonymous"/"ftp" with any password
  char user[64];
  char pass[128];
  char root[PATH_MAX];  // what clients see as "/" ("/" on the PS5)
} ftpd_config_t;

typedef struct ftpd_stats {
  int clients;    // logged-in control connections
  int transfers;  // data transfers in progress
  long long bytes_up, bytes_down;  // to / from the PS5 since the start
  double speed;   // bytes/s over the last seconds, both directions
} ftpd_stats_t;

void ftpd_config_default(ftpd_config_t *c);
int ftpd_start(const ftpd_config_t *c);  // 0, or -1 with errno
void ftpd_stop(void);                    // asks; ftpd_running() turns 0 when done
int ftpd_running(void);
// New logins use the new credentials; sessions already logged in stay.
void ftpd_set_config(const ftpd_config_t *c);
void ftpd_get_stats(ftpd_stats_t *out);

/* Recent server events for the TV screen, oldest first. Returns how many
   were copied (at most max), each line NUL-terminated in 160 bytes. */
int ftpd_recent(char lines[][160], int max);

/* The address other devices use to reach the PS5 (or this computer). */
void net_local_ip(char *buf, unsigned long size);
