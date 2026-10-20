#pragma once

/* Network gaps for the Switch build (see net.c).

   libnx gives us the real thing for almost everything the engine needs:
   getaddrinfo, getnameinfo and gethostid all work once socketInitialize has
   run (app/switch_init.c), so unlike the PS5 build there is nothing to
   redirect here. What is missing is socketpair(), which libnx declares but
   answers with ENOSYS -- libmicrohttpd needs one to wake its polling thread.
   The web app's copy of libmicrohttpd is compiled with
   -Dsocketpair=chiral_socketpair so it picks up the version below instead
   (see tools/build-switch-deps.sh). */

#ifdef __SWITCH__
int chiral_socketpair(int domain, int type, int protocol, int sv[2]);
#endif
