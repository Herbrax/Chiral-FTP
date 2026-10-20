#pragma once

/* PS5 native build: name lookup through sceNetResolver (see ps5_net.c). */
#ifdef __PROSPERO__
#include <netdb.h>

int chiral_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                       struct addrinfo **res);
void chiral_freeaddrinfo(struct addrinfo *entry);
const char *chiral_gai_strerror(int err);

#define getaddrinfo chiral_getaddrinfo
#define freeaddrinfo chiral_freeaddrinfo
#define gai_strerror chiral_gai_strerror
#endif
