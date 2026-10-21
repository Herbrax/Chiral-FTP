#pragma once

/* Shared between the FTP engine (ftp.c) and its SFTP backend (sftp.c). */

#include "ftp.h"

/* read_reply() result when the server greets with an SSH banner. */
#define FTP_CODE_SSH -2

void ftp_set_error(ftp_conn_t *c, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));
void ftp_sock_timeouts(int fd, int seconds);
/* Resolves c->server.host and connects c->fd; sets c->peer. */
int ftp_connect_socket(ftp_conn_t *c, unsigned short default_port);

/* The SFTP client (sftp.c). "sftpc_", not "sftp_": libssh, which the
   computer build links for its SFTP server (sftpd.c), exports sftp_open,
   sftp_close and others under the plain names. */
int sftpc_open(ftp_conn_t *c);
void sftpc_close(ftp_conn_t *c);
/* Frees the SSH session without a polite disconnect (after errors). */
void sftpc_drop(ftp_conn_t *c);
int sftpc_pwd(ftp_conn_t *c, char *out, size_t size);
int sftpc_cwd(ftp_conn_t *c, const char *path);
int sftpc_list(ftp_conn_t *c, const char *path, ftp_listing_t *out,
              char *resolved, size_t resolved_size);
int sftpc_mkdir(ftp_conn_t *c, const char *path);
int sftpc_rmdir(ftp_conn_t *c, const char *path);
int sftpc_delete(ftp_conn_t *c, const char *path);
int sftpc_rename(ftp_conn_t *c, const char *from, const char *to);
int sftpc_chmod(ftp_conn_t *c, const char *path, unsigned int mode);
int sftpc_ping(ftp_conn_t *c, int timeout_ms);  // ftp_ping
int sftpc_size(ftp_conn_t *c, const char *path, long long *size);
int sftpc_set_mtime(ftp_conn_t *c, const char *path, long long mtime);
int sftpc_retrieve(ftp_conn_t *c, const char *path, long long offset,
                  ftp_sink_cb sink, void *ctx);
int sftpc_store(ftp_conn_t *c, const char *path, long long offset,
               ftp_source_cb source, void *ctx);
int sftpc_noop(ftp_conn_t *c);
