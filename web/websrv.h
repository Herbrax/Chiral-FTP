#pragma once

#include <microhttpd.h>

/* The web UI server, on its own thread inside the app. The web UI and the
   app share one engine: the same connection, queue and settings. */
int websrv_start(unsigned short port);  // 0, or -1 with errno
void websrv_stop(void);                 // asks; websrv_running() turns 0 when done
int websrv_running(void);
unsigned short websrv_port(void);

enum MHD_Result websrv_queue_response(struct MHD_Connection *conn,
                                      unsigned int status,
                                      struct MHD_Response *resp);
