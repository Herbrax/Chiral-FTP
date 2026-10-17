/* Remote operations shared by the web API and the native app. */

#include "remote_ops.h"

#include <limits.h>
#include <stdio.h>

#include "localfs.h"

/* Depth-first delete, as FileZilla does: list, delete children, RMD. */
static int
delete_tree(ftp_conn_t *c, const char *path, int depth) {
  ftp_listing_t l;
  char resolved[PATH_MAX];
  int ret = 0;

  if(depth > 64) return -1;
  if(ftp_list(c, path, &l, resolved, sizeof(resolved))) return -1;
  for(size_t i = 0; i < l.count && !ret; i++) {
    char child[PATH_MAX];

    if(!path_is_safe_name(l.entries[i].name)) continue;
    path_join(child, sizeof(child), path, l.entries[i].name);
    if(l.entries[i].is_dir && !l.entries[i].is_link) {
      ret = delete_tree(c, child, depth + 1);
    } else {
      ret = ftp_delete(c, child);
    }
  }
  ftp_listing_free(&l);
  if(ret) return ret;
  /* Leave the folder before removing it; some servers refuse otherwise. */
  {
    char parent[PATH_MAX];
    path_parent(parent, sizeof(parent), path);
    ftp_cwd(c, parent);
  }
  return ftp_rmdir(c, path);
}

int
remote_delete_tree(ftp_conn_t *c, const char *path) {
  return delete_tree(c, path, 0);
}
