#pragma once

/* The libmicrohttpd half of the web server, used by websrv.c, api.c and
   asset.c. Kept out of websrv.h so app/ui.cpp needn't see the library. */

#include <microhttpd.h>

#include "websrv.h"

enum MHD_Result websrv_queue_response(struct MHD_Connection *conn,
                                      unsigned int status,
                                      struct MHD_Response *resp);
