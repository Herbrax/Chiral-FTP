#pragma once

/* SFTP server: the FTP server's login and shared folder, over SSH. Only
   where the platform has it (CHIRAL_HAS_SFTPD, a computer for now), and
   only one of the two runs at a time -- the server screen picks which.
   Same shape as ftpd.h, so the screen shows either the same way. */

#include <stddef.h>

#include "ftpd.h"

#define SFTPD_DEFAULT_PORT 2222

/* c->port is the SFTP port. c->anonymous is ignored: SSH has no anonymous
   login. 0, or -1 with errno. */
int sftpd_start(const ftpd_config_t *c);
void sftpd_stop(void);  // asks; sftpd_running() turns 0 when done
int sftpd_running(void);
// New logins use the new credentials; sessions already logged in stay.
void sftpd_set_config(const ftpd_config_t *c);
void sftpd_get_stats(ftpd_stats_t *out);
int sftpd_recent(char lines[][160], int max);

/* The server key's fingerprint, "SHA256:...": what an SFTP app shows the
   first time it connects, asking whether to trust it. Empty until the
   server has started once. */
void sftpd_fingerprint(char *buf, size_t size);

/* As ftpd_add_guest(): a login quick connect hands out, good only from ip. */
void sftpd_add_guest(const char *user, const char *pass, const char *ip);
