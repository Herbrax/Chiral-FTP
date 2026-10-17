#pragma once

/* Copying and moving files on the PS5 itself (the file manager, on the TV and
   in the web app). One worker thread runs the jobs in order. */

typedef struct fsjob_stats {
  int running;            // 1 while a job runs
  int queued;             // jobs waiting after it
  int move;               // the running job moves instead of copies
  char current[256];      // file being copied
  char target[1024];      // folder it goes to
  long long done, total;  // bytes of the running job
  double speed;           // bytes/s
  unsigned long finished; // count of finished jobs: changes when the panes should refresh
  int failed;             // finished jobs that failed
  char error[300];        // why the last one failed
} fsjob_stats_t;

/* Copies (or moves) src into the folder dst_dir, keeping its name; when that
   name is taken, the copy becomes "name (2).ext". 0, or -1 if not queued. */
int fsjob_add(int move, const char *src, const char *dst_dir);
void fsjob_cancel_all(void);
void fsjob_get_stats(fsjob_stats_t *out);
