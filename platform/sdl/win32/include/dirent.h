/* Folders on Windows, in place of MinGW's <dirent.h>: UTF-8 names of any
   length, d_type, and "/" listing the drives (win32/posix.c). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10

struct dirent {
  unsigned char d_type;
  char d_name[1024]; /* 255 UTF-16 units at most, 3 bytes each in UTF-8 */
};

typedef struct chiral_dir DIR;

DIR *chiral_opendir(const char *path);
struct dirent *chiral_readdir(DIR *dir);
int chiral_closedir(DIR *dir);

#define opendir(...) chiral_opendir(__VA_ARGS__)
#define readdir(...) chiral_readdir(__VA_ARGS__)
#define closedir(...) chiral_closedir(__VA_ARGS__)

#ifdef __cplusplus
}
#endif
