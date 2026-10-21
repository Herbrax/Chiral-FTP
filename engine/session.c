#include "session.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

#define PING_IDLE_S    10  /* this quiet: check the server is still there */
#define PING_TIMEOUT_S 5
#define LOST_AFTER_S   30  /* out of touch this long: give the connection up */

/* g_lock is the connection, held for as long as a network operation takes.
   g_state is what the UI reads -- connected or not, to what -- so asking
   never waits on a server that has stopped answering. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_state = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_keepalive;
static int g_keepalive_running;
static volatile int g_keepalive_stop;
/* Cuts short a reconnect watch() has under way (ftp_open checks it), so
   disconnecting or leaving the app doesn't wait out a connect timeout to a
   device that has gone. */
static volatile int g_abort;
static ftp_conn_t g_conn;
static int g_connected;  /* the user wants to be connected (g_state) */
static ftp_server_t g_server;  /* to what (g_state) */
static unsigned g_drops;  /* connections given up (g_state) */
static time_t g_last_ok;  /* last time the server answered (g_lock) */
static int g_initialized;

static void
set_connected(int on, const ftp_server_t *server, int dropped) {
  pthread_mutex_lock(&g_state);
  g_connected = on;
  if(on) g_server = *server;
  if(dropped) g_drops++;
  pthread_mutex_unlock(&g_state);
}

static int
connected(void) {
  int on;
  pthread_mutex_lock(&g_state);
  on = g_connected;
  pthread_mutex_unlock(&g_state);
  return on;
}

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
  set_connected(0, NULL, 0);
  if(ftp_open(&g_conn, server) || ftp_pwd(&g_conn, home, home_size)) {
    snprintf(err, err_size, "%s", g_conn.error);
    ftp_close(&g_conn);
    ret = -1;
  } else {
    g_last_ok = time(NULL);
    set_connected(1, &g_conn.server, 0);
    log_msg(0, LOG_STATUS, "Connected to %s", server->host);
  }
  pthread_mutex_unlock(&g_lock);
  return ret;
}

void
session_disconnect(void) {
  set_connected(0, NULL, 0);  // at once, whatever the connection is doing
  g_abort = 1;
  pthread_mutex_lock(&g_lock);
  ensure_init();
  ftp_close(&g_conn);
  g_abort = g_keepalive_stop;
  pthread_mutex_unlock(&g_lock);
}

int
session_server(ftp_server_t *out) {
  int on;

  pthread_mutex_lock(&g_state);
  on = g_connected;
  if(on) *out = g_server;
  pthread_mutex_unlock(&g_state);
  return on;
}

unsigned
session_drops(void) {
  unsigned n;
  pthread_mutex_lock(&g_state);
  n = g_drops;
  pthread_mutex_unlock(&g_state);
  return n;
}

int
session_run(session_fn fn, void *ctx, char *err, size_t err_size) {
  int ret = -1;

  pthread_mutex_lock(&g_lock);
  ensure_init();
  if(!connected()) {
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
    if(!(ret = fn(&g_conn, ctx))) {
      g_last_ok = time(NULL);
      break;
    }
    /* Retry only when the connection dropped, not when the server said no. */
    if(ftp_is_open(&g_conn)) break;
  }
  if(ret) snprintf(err, err_size, "%s", g_conn.error[0] ? g_conn.error : "Operation failed");
  pthread_mutex_unlock(&g_lock);
  return ret;
}

/* Every five seconds, with g_lock held: a quiet connection is pinged, a
   dropped one is reconnected, and one that hasn't been heard from for
   LOST_AFTER_S is given up -- session_server() then says so, and
   session_drops() goes up for the UI to notice. */
static void
watch(void) {
  const time_t t = time(NULL);

  if(ftp_is_open(&g_conn) && t - g_conn.last_activity >= PING_IDLE_S) {
    if(!ftp_ping(&g_conn, PING_TIMEOUT_S)) g_last_ok = time(NULL);
  }
  if(ftp_is_open(&g_conn)) return;
  if(time(NULL) - g_last_ok < LOST_AFTER_S) {
    ftp_server_t server = g_conn.server;
    int failed;
    g_conn.cancel = &g_abort;
    failed = ftp_open(&g_conn, &server);
    g_conn.cancel = NULL;
    if(g_abort) return;  // being disconnected anyway
    if(!failed) {
      g_last_ok = time(NULL);
      return;
    }
  }
  if(time(NULL) - g_last_ok >= LOST_AFTER_S) {
    ftp_close(&g_conn);
    set_connected(0, NULL, 1);
    log_msg(0, LOG_ERROR, "Lost the connection to %s", g_conn.server.host);
  }
}

static void *
keepalive_main(void *arg) {
  (void)arg;
  while(!g_keepalive_stop) {
    /* Five seconds, in short naps: a plain sleep(5) would keep the exit
       waiting that long for this thread to notice it should finish. */
    for(int i = 0; i < 50 && !g_keepalive_stop; i++) usleep(100 * 1000);
    if(g_keepalive_stop) break;
    if(pthread_mutex_trylock(&g_lock)) continue;  // in use: it is being heard from
    if(g_initialized && connected()) watch();
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
  g_abort = 1;
  pthread_join(g_keepalive, NULL);
  g_keepalive_running = 0;
}
