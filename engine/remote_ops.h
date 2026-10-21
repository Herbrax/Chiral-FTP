#pragma once

#include "ftp.h"

/* Deletes a remote folder and everything in it (list, delete children, RMD). */
int remote_delete_tree(ftp_conn_t *c, const char *path);

/* chmod a remote folder and everything in it, the way localfs_chmod_tree
   does locally: folders get localfs_dir_mode(mode), links are left alone.
   Keeps going past refusals while the connection lasts; -1 if anything
   failed, with the last reason in c->error. */
int remote_chmod_tree(ftp_conn_t *c, const char *path, unsigned int mode);
