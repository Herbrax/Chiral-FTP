/* The web app, left out. Built instead of websrv.c, api.c and asset.c when
   the Makefile is run with WEB=0, which is the escape hatch for a toolchain
   where libmicrohttpd won't cross-compile (tools/build-switch-deps.sh).

   Everything else -- the FTP server, the file manager, the FTP/SFTP client
   -- is unaffected. The web app's screen reports that it can't start. */

#include "websrv.h"

#include <errno.h>

int
websrv_start(unsigned short port) {
  (void)port;
  errno = ENOSYS;
  return -1;
}

void
websrv_stop(void) {
}

int
websrv_running(void) {
  return 0;
}

unsigned short
websrv_port(void) {
  return 0;
}
