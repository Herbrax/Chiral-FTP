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

int sftp_open(ftp_conn_t *c);
void sftp_close(ftp_conn_t *c);
/* Frees the SSH session without a polite disconnect (after errors). */
void sftp_drop(ftp_conn_t *c);
int sftp_pwd(ftp_conn_t *c, char *out, size_t size);
int sftp_cwd(ftp_conn_t *c, const char *path);
int sftp_list(ftp_conn_t *c, const char *path, ftp_listing_t *out,
              char *resolved, size_t resolved_size);
int sftp_mkdir(ftp_conn_t *c, const char *path);
int sftp_rmdir(ftp_conn_t *c, const char *path);
int sftp_delete(ftp_conn_t *c, const char *path);
int sftp_rename(ftp_conn_t *c, const char *from, const char *to);
int sftp_chmod(ftp_conn_t *c, const char *path, unsigned int mode);
int sftp_size(ftp_conn_t *c, const char *path, long long *size);
int sftp_set_mtime(ftp_conn_t *c, const char *path, long long mtime);
int sftp_retrieve(ftp_conn_t *c, const char *path, long long offset,
                  ftp_sink_cb sink, void *ctx);
int sftp_store(ftp_conn_t *c, const char *path, long long offset,
               ftp_source_cb source, void *ctx);
int sftp_noop(ftp_conn_t *c);
