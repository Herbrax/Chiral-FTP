/* Network and libc gaps for the PS5 native build.

   Name lookup: In a ShadowMountPlus title the libc
   getaddrinfo isn't usable, so like ps5-native-app-boilerplate's console_curl.c
   (GPL-3.0-or-later) this resolves IPv4 names with sceNetResolver and parses
   numeric addresses directly. One address per name. */

#ifdef __PROSPERO__

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

extern int sceNetPoolCreate(const char *name, int size, int flags);
extern int sceNetPoolDestroy(int pool);
extern int sceNetResolverCreate(const char *name, int pool, int flags);
extern int sceNetResolverStartNtoa(int resolver, const char *hostname, struct in_addr *address,
                                   int timeout, int retries, int flags);
extern int sceNetResolverStartAton(int resolver, const struct in_addr *address, char *hostname,
                                   int len, int timeout, int retries, int flags);
extern int sceNetResolverDestroy(int resolver);

static int
lookup(const char *name, struct in_addr *address) {
  int pool, resolver, result = EAI_FAIL;

  if(inet_pton(AF_INET, name, address) == 1) return 0;
  if((pool = sceNetPoolCreate("chiral_dns", 16 * 1024, 0)) < 0) return EAI_MEMORY;
  if((resolver = sceNetResolverCreate("chiral_dns", pool, 0)) >= 0) {
    /* 5 s per try, 2 retries */
    result = sceNetResolverStartNtoa(resolver, name, address, 5000000, 2, 0) < 0 ? EAI_NONAME : 0;
    sceNetResolverDestroy(resolver);
  }
  sceNetPoolDestroy(pool);
  return result;
}

int
chiral_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
            struct addrinfo **res) {
  struct in_addr address;
  struct addrinfo *entry;
  struct sockaddr_in *in;
  int family = hints ? hints->ai_family : AF_UNSPEC;
  int err;

  *res = NULL;
  if(family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;
  address.s_addr = htonl(INADDR_LOOPBACK);
  if(node && (err = lookup(node, &address))) return err;
  if(!(entry = calloc(1, sizeof(*entry) + sizeof(*in)))) return EAI_MEMORY;
  in = (struct sockaddr_in *)(entry + 1);
  in->sin_len = sizeof(*in);
  in->sin_family = AF_INET;
  in->sin_port = htons((unsigned short)(service ? atoi(service) : 0));
  in->sin_addr = address;
  entry->ai_family = AF_INET;
  entry->ai_socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
  entry->ai_protocol = IPPROTO_TCP;
  entry->ai_addrlen = sizeof(*in);
  entry->ai_addr = (struct sockaddr *)in;
  *res = entry;
  return 0;
}

void
chiral_freeaddrinfo(struct addrinfo *entry) {
  while(entry) {
    struct addrinfo *next = entry->ai_next;
    free(entry);
    entry = next;
  }
}

/* The name a device has on the network (from the router's DNS), for the FTP
   server's activity list. 0 on success. */
int
chiral_peer_name(const struct in_addr *address, char *out, int size) {
  int pool, resolver, result = -1;

  if((pool = sceNetPoolCreate("chiral_rdns", 16 * 1024, 0)) < 0) return -1;
  if((resolver = sceNetResolverCreate("chiral_rdns", pool, 0)) >= 0) {
    /* One short try: a router without names answers at once, and the
       greeting waits for this. */
    result = sceNetResolverStartAton(resolver, address, out, size, 1500000, 0, 0) < 0 ? -1 : 0;
    sceNetResolverDestroy(resolver);
  }
  sceNetPoolDestroy(pool);
  return result;
}

/* libssh2 wipes key material with explicit_bzero, which the console's libc
   lacks. Volatile writes keep the compiler from dropping the wipe. */
void
explicit_bzero(void *buf, size_t len) {
  volatile unsigned char *p = buf;
  while(len--) *p++ = 0;
}

const char *
chiral_gai_strerror(int err) {
  switch(err) {
  case EAI_NONAME: return "host name not found";
  case EAI_MEMORY: return "out of memory";
  case EAI_FAMILY: return "address family not supported";
  default: return "name lookup failed";
  }
}

#endif
