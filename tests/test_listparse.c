/* Unit tests for the directory listing parser: make test */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ftp.h"

static int g_failed;

#define CHECK(cond, ...) do {                       \
    if(!(cond)) {                                   \
      g_failed++;                                   \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
      printf(__VA_ARGS__);                          \
      printf("\n");                                 \
    }                                               \
  } while(0)

/* 2026-10-15 12:00:00 UTC */
#define NOW 1792065600LL

static ftp_listing_t
parse(const char *text, int mlsd) {
  ftp_listing_t l = {0};
  char *copy = strdup(text);

  ftp_parse_listing(&l, copy, strlen(copy), mlsd, (time_t)NOW);
  free(copy);
  return l;
}

static const ftp_entry_t *
find(const ftp_listing_t *l, const char *name) {
  for(size_t i = 0; i < l->count; i++) {
    if(!strcmp(l->entries[i].name, name)) return &l->entries[i];
  }
  return NULL;
}

static void
test_ftpsrv(void) {
  /* Exactly what ps5-payload-dev/ftpsrv emits. */
  ftp_listing_t l = parse(
    "drwxr-xr-x 2 0 0 512 Oct 15 11:00 .\r\n"
    "drwxr-xr-x 2 0 0 512 Oct 15 11:00 ..\r\n"
    "drwxrwxrwx 12 0 0 512 Oct 15 11:00 data\r\n"
    "-rw-r--r-- 1 0 0 1048576 Dec 24 08:30 eboot.bin\r\n", 0);
  const ftp_entry_t *e;

  CHECK(l.count == 2, "ftpsrv LIST count %zu", l.count);
  CHECK((e = find(&l, "data")) && e->is_dir, "ftpsrv dir");
  CHECK((e = find(&l, "eboot.bin")) && !e->is_dir && e->size == 1048576, "ftpsrv file");
  /* Dec 24 is in the future for NOW, so it must be last year. */
  CHECK(e && e->mtime == 1766565000LL, "ftpsrv year inference %lld", e ? e->mtime : 0);
  CHECK(e && !strcmp(e->owner, "0") && !strcmp(e->group, "0"), "ftpsrv owner/group");
  ftp_listing_free(&l);

  l = parse("type=dir;size=512;modify=20261015110000; .\r\n"
            "type=dir;size=512;modify=20261015110000; ..\r\n"
            "type=dir;size=512;modify=20261015110000; data\r\n"
            "type=file;size=42;modify=20260101000000; my file.txt\r\n", 1);
  CHECK(l.count == 2, "ftpsrv MLSD count %zu", l.count);
  CHECK((e = find(&l, "my file.txt")) && e->size == 42 && e->mtime == 1767225600LL,
        "ftpsrv MLSD file");
  ftp_listing_free(&l);
}

static void
test_unix_variants(void) {
  ftp_listing_t l = parse(
    "total 12\n"
    "-rw-r--r--    1 1000     1000      1048576 Jan 01  2023 big file.bin\n"
    "-rw-r--r-- 1 owner 123 Feb  3 10:00 nogroup\n"
    "lrwxrwxrwx 1 root root 7 Mar  1  2020 lib -> usr/lib\n"
    "-rw-r--r-- 1 ftp ftp      123456 Oct 10  2024 movie.mkv\n"
    "-rw-r--r-- 1 u g 10 2024-05-01 13:45 iso.txt\n"
    "drwxr-xr-x+ 3 user staff 96 Sep  9 09:09 acl dir\n"
    "-rw-r--r-- 1 user group 5 Jun  6 06:06  leading space\n", 0);
  const ftp_entry_t *e;

  CHECK(l.count == 7, "unix count %zu", l.count);
  CHECK((e = find(&l, "big file.bin")) && e->size == 1048576 &&
        e->mtime == 1672531200LL, "vsftpd year format");
  CHECK((e = find(&l, "nogroup")) && e->size == 123 && !strcmp(e->owner, "owner"),
        "no group column");
  CHECK((e = find(&l, "lib")) && e->is_link && !strcmp(e->target, "usr/lib"), "symlink");
  CHECK((e = find(&l, "movie.mkv")) && e->size == 123456, "FileZilla Server LIST");
  CHECK((e = find(&l, "iso.txt")) && e->mtime == 1714571100LL, "ISO date");
  CHECK((e = find(&l, "acl dir")) && e->is_dir, "ACL marker and spaces");
  CHECK(find(&l, " leading space"), "name with leading space");
  ftp_listing_free(&l);
}

static void
test_dos(void) {
  ftp_listing_t l = parse(
    "01-31-24  09:15PM       <DIR>          My Folder\r\n"
    "02-01-2024  10:00AM            12345 file.txt\r\n"
    "12-25-99  12:00AM               1 old.txt\r\n", 0);
  const ftp_entry_t *e;

  CHECK(l.count == 3, "dos count %zu", l.count);
  CHECK((e = find(&l, "My Folder")) && e->is_dir && e->mtime == 1706735700LL, "IIS dir");
  CHECK((e = find(&l, "file.txt")) && e->size == 12345, "IIS file 4-digit year");
  CHECK((e = find(&l, "old.txt")) && e->mtime == 946080000LL, "IIS 12AM, 1999");
  ftp_listing_free(&l);
}

static void
test_mlsd_facts(void) {
  ftp_listing_t l = parse(
    "type=cdir;modify=20240101000000; /home\n"
    "type=file;modify=20241010120000;size=123456; movie.mkv\n"
    "type=OS.unix=slink:/etc/x;modify=20240101000000;UNIX.mode=0777; link\n"
    "Type=Dir;Modify=20240101000000;UNIX.mode=0755;UNIX.owner=1000; Folder\n"
    "type=file;size=6;modify=20240101000000;unix.mode=0o640;unix.uid=501; py.txt\n", 1);
  const ftp_entry_t *e;

  CHECK(l.count == 4, "mlsd count %zu", l.count);
  CHECK((e = find(&l, "py.txt")) && !strcmp(e->perm, "0640") && !strcmp(e->owner, "501"),
        "pyftpdlib 0o mode");
  CHECK((e = find(&l, "link")) && e->is_link && !strcmp(e->target, "/etc/x") &&
        !strcmp(e->perm, "0777"), "MLSD symlink");
  CHECK((e = find(&l, "Folder")) && e->is_dir && !strcmp(e->owner, "1000"),
        "MLSD case-insensitive facts");
  ftp_listing_free(&l);
}

int
main(void) {
  test_ftpsrv();
  test_unix_variants();
  test_dos();
  test_mlsd_facts();
  if(g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("all listing parser tests passed\n");
  return 0;
}
