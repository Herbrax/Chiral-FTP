#include "session.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

#define KEEPALIVE_IDLE_S 30

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_keepalive;
static int g_keepalive_running;
static volatile int g_keepalive_stop;
static ftp_conn_t g_conn;
static int g_connected;  /* the user wants to be connected */
static int g_initialized;

static void
ensure_init(void) {
  if(!g_initialized) {
    ftp_init(&g_conn, 0);
    g_initialized = 1;
  }
}

int
session_connect(const ftp_server_t *server, char *home, size_t home_size,
                char *err, size_t err_size) {
  int ret = 0;

  pthread_mutex_lock(&g_lock);
  ensure_init();
  ftp_close(&g_conn);
  g_connected = 0;
  if(ftp_open(&g_conn, server) || ftp_pwd(&g_conn, home, home_size)) {
    snprintf(err, err_size, "%s", g_conn.error);
    ftp_close(&g_conn);
    ret = -1;
  } else {
    g_connected = 1;
    log_msg(0, LOG_STATUS, "Connected to %s", server->host);
  }
  pthread_mutex_unlock(&g_lock);
  return ret;
}

void
session_disconnect(void) {
  pthread_mutex_lock(&g_lock);
  ensure_init();
  g_connected = 0;
  ftp_close(&g_conn);
  pthread_mutex_unlock(&g_lock);
}

int
session_server(ftp_server_t *out) {
  int connected;

  pthread_mutex_lock(&g_lock);
  connected = g_connected;
  if(connected) *out = g_conn.server;
  pthread_mutex_unlock(&g_lock);
  return connected;
}

int
session_run(session_fn fn, void *ctx, char *err, size_t err_size) {
  int ret = -1;

  pthread_mutex_lock(&g_lock);
  ensure_init();
  if(!g_connected) {
    snprintf(err, err_size, "Not connected");
    pthread_mutex_unlock(&g_lock);
    return -1;
  }
  for(int attempt = 0; attempt < 2; attempt++) {
    if(!ftp_is_open(&g_conn)) {
      ftp_server_t server = g_conn.server;

      log_msg(0, LOG_STATUS, "Connection lost, reconnecting...");
      if(ftp_open(&g_conn, &server)) break;
    }
    g_conn.error[0] = 0;
    if(!(ret = fn(&g_conn, ctx))) break;
    /* Retry only when the connection dropped, not when the server said no. */
    if(ftp_is_open(&g_conn)) break;
  }
  if(ret) snprintf(err, err_size, "%s", g_conn.error[0] ? g_conn.error : "Operation failed");
  pthread_mutex_unlock(&g_lock);
  return ret;
}

static void *
keepalive_main(void *arg) {
  (void)arg;
  while(!g_keepalive_stop) {
    /* Five seconds, in short naps: a plain sleep(5) would keep the exit
       waiting that long for this thread to notice it should finish. */
    for(int i = 0; i < 50 && !g_keepalive_stop; i++) usleep(100 * 1000);
    if(g_keepalive_stop) break;
    if(pthread_mutex_trylock(&g_lock)) continue;
    if(g_initialized && g_connected && ftp_is_open(&g_conn) &&
       time(NULL) - g_conn.last_activity >= KEEPALIVE_IDLE_S) {
      ftp_noop(&g_conn);
    }
    pthread_mutex_unlock(&g_lock);
  }
  return NULL;
}

void
session_start_keepalive(void) {
  /* Joinable, not detached: stopping it means waiting for it. */
  if(!pthread_create(&g_keepalive, NULL, keepalive_main, NULL)) {
    g_keepalive_running = 1;
  }
}

void
session_stop_keepalive(void) {
  if(!g_keepalive_running) return;
  g_keepalive_stop = 1;
  pthread_join(g_keepalive, NULL);
  g_keepalive_running = 0;
}
