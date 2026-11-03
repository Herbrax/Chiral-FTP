// ProsperoRadio curl guide - libc entry points PacBrew's libcurl/OpenSSL/zstd need.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// In chiral-ftp: the boilerplate's examples/pacbrew-curl/compat.c, as
// ProsperoStore ships it, for the updater's libcurl (platform/ps5/update.cpp).
// pipe2 is left out, being runtime.c's, and gmtime_r, being the SDK libc.a's.
//
// PacBrew's archives are built for the payload SDK's libc. A native title
// links against the console's libc import stubs, which do not export these.
// Each one is the smallest stand-in that keeps the library working. The set
// was found with tools in find_missing_symbols.sh; yours may differ with
// other package versions.

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#ifndef CURL_COMPAT_TIME_TEST

/* libcurl looks up the user's home folder (.netrc and the like). There is no
 * user database: report "no such user". */
struct passwd;
int getpwuid_r(unsigned user, struct passwd *entry, char *buffer, size_t size,
               struct passwd **result)
{
    (void)user;
    (void)entry;
    (void)buffer;
    (void)size;
    *result = NULL;
    return ENOENT;
}

/* The archives call _setjmp/_longjmp; the console libc exports setjmp/longjmp.
 * Plain jumps keep the caller's frame intact (a C wrapper would not:
 * setjmp must not return through an extra frame). x86-64 only. */
__asm__(".text\n"
        ".globl _setjmp\n"
        "_setjmp:\n"
        "    jmp *setjmp@GOTPCREL(%rip)\n"
        ".globl _longjmp\n"
        "_longjmp:\n"
        "    jmp *longjmp@GOTPCREL(%rip)\n");

/* syslog is not used here. */
__attribute__((weak)) void openlog(const char *identifier, int option, int facility)
{
    (void)identifier;
    (void)option;
    (void)facility;
}

void closelog(void)
{
}

/* Referenced by OpenSSL's module-loading code; "not found" is fine. */
int dladdr(const void *address, void *info)
{
    (void)address;
    (void)info;
    return 0;
}

/* Only for IPv6 scope ids in URLs. */
unsigned if_nametoindex(const char *name)
{
    (void)name;
    return 0;
}

/* pipe2, which libcurl creates its internal pipes with, is platform/ps5/runtime.c's
 * (libmicrohttpd needs it too). */

/* Only QUIC/HTTP3 uses these, and curl falls back when they fail. */
int recvmmsg(int socket, void *messages, size_t count, int flags, const void *timeout)
{
    (void)socket;
    (void)messages;
    (void)count;
    (void)flags;
    (void)timeout;
    errno = ENOSYS;
    return -1;
}

int sendmmsg(int socket, void *messages, size_t count, int flags)
{
    (void)socket;
    (void)messages;
    (void)count;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

/* Referenced by the archives, never needed by an HTTP client: fail. */
__attribute__((weak)) FILE *popen(const char *command, const char *mode)
{
    (void)command;
    (void)mode;
    errno = ENOSYS;
    return NULL;
}

__attribute__((weak)) int pclose(FILE *stream)
{
    (void)stream;
    errno = ENOSYS;
    return -1;
}

/* zstd's optional tracing hooks, referenced by the archive. A begin that returns 0 means "not
 * traced". */
unsigned long long ZSTD_trace_compress_begin(const void *context)
{
    (void)context;
    return 0;
}

void ZSTD_trace_compress_end(unsigned long long trace, const void *record)
{
    (void)trace;
    (void)record;
}

unsigned long long ZSTD_trace_decompress_begin(const void *context)
{
    (void)context;
    return 0;
}

void ZSTD_trace_decompress_end(unsigned long long trace, const void *record)
{
    (void)trace;
    (void)record;
}

#endif

/* gmtime_r, which OpenSSL checks certificate dates with, is the payload SDK's
 * libc.a one: chiral-ftp links that archive (tools/build-ps5.sh), and two
 * definitions don't link. It is a full one, not a stub. */
