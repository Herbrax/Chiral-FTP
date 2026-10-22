#pragma once

#include <stddef.h>

#include "ftp.h"

/* The browsing connection (log id 0) used by the remote pane. Transfers use
   their own connections from the queue. */

int session_connect(const ftp_server_t *server, char *home, size_t home_size,
                    char *err, size_t err_size);
void session_disconnect(void);
/* Copies the current server; returns 0 when not connected. */
int session_server(ftp_server_t *out);
/* How many connections have been given up so far: the server stopped
   answering and couldn't be reached again for 30 seconds. The UI watches
   for it to change. */
unsigned session_drops(void);

typedef int (*session_fn)(ftp_conn_t *c, void *ctx);
/* Runs fn on the browsing connection, reconnecting once if it was dropped.
   On failure the error text is copied to err. */
int session_run(session_fn fn, void *ctx, char *err, size_t err_size);

void session_start_keepalive(void);
/* Ends the keepalive thread and waits for it. Call before returning from
   main: static destructors run on the way out, and a thread still looking
   at this module's state while that happens is a crash on exit. */
void session_stop_keepalive(void);
