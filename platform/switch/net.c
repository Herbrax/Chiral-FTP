/* Network gaps for the Switch build. See net.h for why this exists. */

#ifdef __SWITCH__

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdint.h>

/* One attempt at a connected pair: listen on bind_addr, connect to
   connect_addr (both in host byte order). 0 on success.

   Horizon's socket calls don't always set errno when they fail, so every
   step that goes wrong gets a plausible one here -- a caller that reports
   "failed, errno 0 (Success)" helps nobody. */
static int
pair_via(unsigned long bind_addr, unsigned long connect_addr, int sv[2]) {
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);
  int listener = -1, client = -1, server = -1, on = 1, saved;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = 0; /* any free port */
  addr.sin_addr.s_addr = htonl((uint32_t)bind_addr);

  errno = 0;
  if((listener = socket(AF_INET, SOCK_STREAM, 0)) < 0) goto fail;
  if(bind(listener, (struct sockaddr *)&addr, sizeof(addr))) goto fail;
  if(listen(listener, 1)) goto fail;
  /* The port the kernel picked. */
  if(getsockname(listener, (struct sockaddr *)&addr, &len)) goto fail;

  addr.sin_addr.s_addr = htonl((uint32_t)connect_addr);
  if((client = socket(AF_INET, SOCK_STREAM, 0)) < 0) goto fail;
  if(connect(client, (struct sockaddr *)&addr, sizeof(addr))) goto fail;
  if((server = accept(listener, NULL, NULL)) < 0) goto fail;
  close(listener);

  /* The point of the pair is an immediate wakeup, so don't let Nagle sit on
     the one byte that carries it. */
  (void)setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  (void)setsockopt(server, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

  sv[0] = client;
  sv[1] = server;
  return 0;

fail:
  saved = errno ? errno : ECONNREFUSED;
  if(listener >= 0) close(listener);
  if(client >= 0) close(client);
  if(server >= 0) close(server);
  errno = saved;
  return -1;
}

/* A connected pair of sockets, because Horizon has no AF_UNIX and libnx's
   socketpair() is a stub that sets ENOSYS.

   Loopback is the obvious way to build one, and it does not work: on
   hardware the 127.0.0.1 route fails outright (see the stage 8 build test
   in the port notes). So the console's own address is tried as well, which
   needs the network to be up.

   libmicrohttpd calls this during MHD_start_daemon whatever flags it is
   given -- its polling thread has no other way to be woken -- so the web
   app lives or dies by it.

   Returns 0 with both descriptors in sv, or -1 with errno set. */
int
chiral_socketpair(int domain, int type, int protocol, int sv[2]) {
  unsigned long host;
  int base, extra;

  (void)protocol;
  /* Callers ask for AF_UNIX; we answer with the closest thing available. */
  if(domain != AF_UNIX && domain != AF_INET) {
    errno = EAFNOSUPPORT;
    return -1;
  }

  /* The type argument carries flags as well as a type: libmicrohttpd asks
     for SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK. Those flag bits are
     high ones, so mask them off before deciding what kind of socket is
     wanted -- comparing the whole argument against SOCK_STREAM rejected
     MHD's request outright, with EPROTOTYPE ("protocol wrong type for
     socket"), and took the web app down with it. */
  base = type & 0x0f;
  extra = type & ~0x0f;
  if(base != SOCK_STREAM) {
    errno = EPROTOTYPE;
    return -1;
  }

  if(pair_via(INADDR_LOOPBACK, INADDR_LOOPBACK, sv)) {
    /* Out through the real interface and back in. Only possible with a
       network connection, and gethostid() is how libnx reports one. */
    host = (unsigned long)ntohl((uint32_t)gethostid());
    if(!host || host == INADDR_LOOPBACK || pair_via(INADDR_ANY, host, sv)) {
      if(!errno) errno = ENOSYS;
      return -1;
    }
  }

  /* Any flag at all means the caller wanted the non-blocking, close-on-exec
     pair. Horizon has no exec, so close-on-exec is moot; non-blocking is
     not -- a caller expecting it will hang on a blocking read. */
  if(extra) {
    for(int i = 0; i < 2; i++) {
      int fl = fcntl(sv[i], F_GETFL, 0);
      if(fl >= 0) (void)fcntl(sv[i], F_SETFL, fl | O_NONBLOCK);
    }
  }
  return 0;
}

#else

/* Not the Switch: libnx's gap doesn't exist here, and an empty translation
   unit isn't valid C. */
typedef int chiral_nx_net_unused;

#endif /* __SWITCH__ */
