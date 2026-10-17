#pragma once

#include "ftp.h"

/* Deletes a remote folder and everything in it (list, delete children, RMD). */
int remote_delete_tree(ftp_conn_t *c, const char *path);
