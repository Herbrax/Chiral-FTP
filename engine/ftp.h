#pragma once

#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>

#define FTP_PASV_AUTO 0
#define FTP_PASV_ONLY 1
#define FTP_EPSV_ONLY 2

#define FTP_PROTO_FTP  0
#define FTP_PROTO_SFTP 1

/* Login details for one server, as entered in Quick Connect or a saved site. */
typedef struct ftp_server {
  int protocol;     /* FTP_PROTO_FTP or FTP_PROTO_SFTP */
  char host[256];
  unsigned short port;
  char user[128];   /* empty means anonymous */
  char pass[256];
  int pasv_mode;
  int show_hidden;  /* use LIST -a when MLSD is unavailable */
  char keyfile[256]; /* SFTP private key on the PS5; pass is its passphrase */
} ftp_server_t;

typedef struct ftp_entry {
  char name[768];
  char target[768];  /* symlink target, when the server reports it */
  char perm[16];
  char owner[64];
  char group[64];
  int is_dir;
  int is_link;
  long long size;    /* -1 when unknown */
  long long mtime;   /* UTC seconds, -1 when unknown */
} ftp_entry_t;

typedef struct ftp_listing {
  ftp_entry_t *entries;
  size_t count;
  size_t cap;
} ftp_listing_t;

/* Receives downloaded bytes; a nonzero return aborts the transfer. */
typedef int (*ftp_sink_cb)(void *ctx, const char *data, size_t size);
/* Supplies upload bytes; returns 0 at end of file and -1 on error. */
typedef ssize_t (*ftp_source_cb)(void *ctx, char *buf, size_t size);

typedef struct ftp_conn {
  int fd;
  int log_id;
  ftp_server_t server;
  struct sockaddr_storage peer;
  socklen_t peer_len;

  char rbuf[16384];
  size_t rlen;
  int code;
  char reply[4096];
  char error[512];

  int feat_mlsd;
  int feat_utf8;
  int feat_mfmt;
  int no_chmod;  /* the server refused a chmod: don't ask again on this connection */
  char mlst_facts[256];  /* fact list from the FEAT MLST line */
  int no_size;
  int no_mdtm;
  int epsv_failed;
  int pasv_failed;
  int list_a_failed;
  int binary;

  const volatile int *cancel;
  time_t last_activity;

  /* SFTP state (libssh2 session and channel) */
  void *ssh;
  void *sftp;
  char cwd[1024];
} ftp_conn_t;

void ftp_init(ftp_conn_t *c, int log_id);
/* Connects and logs in. Returns 0 on success. */
int ftp_open(ftp_conn_t *c, const ftp_server_t *server);
void ftp_close(ftp_conn_t *c);
int ftp_is_open(const ftp_conn_t *c);
/* Same protocol, host, port and user, so an open connection can be reused. */
int ftp_same_server(const ftp_server_t *a, const ftp_server_t *b);

/* Sends one command and reads the reply. Returns the code, or -1 on I/O error. */
int ftp_command(ftp_conn_t *c, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

int ftp_pwd(ftp_conn_t *c, char *out, size_t size);
int ftp_cwd(ftp_conn_t *c, const char *path);
/* Lists path and stores the server's canonical path in resolved. */
int ftp_list(ftp_conn_t *c, const char *path, ftp_listing_t *out,
             char *resolved, size_t resolved_size);
int ftp_mkdir(ftp_conn_t *c, const char *path);
int ftp_rmdir(ftp_conn_t *c, const char *path);
int ftp_delete(ftp_conn_t *c, const char *path);
int ftp_rename(ftp_conn_t *c, const char *from, const char *to);
int ftp_chmod(ftp_conn_t *c, const char *path, unsigned int mode);
/* Returns 0 and sets *size, 1 if the file does not exist, -1 on error. */
int ftp_size(ftp_conn_t *c, const char *path, long long *size);
int ftp_set_mtime(ftp_conn_t *c, const char *path, long long mtime);
int ftp_retrieve(ftp_conn_t *c, const char *path, long long offset,
                 ftp_sink_cb sink, void *ctx);
/* offset > 0 appends to the existing remote file (resume). */
int ftp_store(ftp_conn_t *c, const char *path, long long offset,
              ftp_source_cb source, void *ctx);
int ftp_noop(ftp_conn_t *c);

/* Forgets every remembered SFTP host key. */
int ftp_forget_host_keys(void);

void ftp_listing_free(ftp_listing_t *l);
int ftp_listing_add(ftp_listing_t *l, const ftp_entry_t *e);
/* Parses a raw MLSD or LIST payload into l. */
void ftp_parse_listing(ftp_listing_t *l, char *data, size_t size, int mlsd,
                       time_t now);
