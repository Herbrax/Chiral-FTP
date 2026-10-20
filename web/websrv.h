#pragma once

/* The web UI server, on its own thread inside the app. The web UI and the
   app share one engine: the same connection, queue and settings.

   This header is the part the UI uses, and deliberately says nothing about
   libmicrohttpd: CHIRAL_NO_WEB builds (web/websrv_stub.c) satisfy it without
   the library present. web/websrv_mhd.h has the rest. */

#ifdef __cplusplus
extern "C" {
#endif

int websrv_start(unsigned short port);  /* 0, or -1 with errno */
void websrv_stop(void);                 /* asks; websrv_running() turns 0 when done */
int websrv_running(void);
unsigned short websrv_port(void);

#ifdef __cplusplus
}
#endif
