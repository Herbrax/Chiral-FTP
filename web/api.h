#pragma once

#include <stddef.h>
#include <microhttpd.h>

enum MHD_Result api_request(struct MHD_Connection *conn, const char *url,
                            const char *body, size_t size);
enum MHD_Result api_error(struct MHD_Connection *conn, unsigned int status,
                          const char *message);
