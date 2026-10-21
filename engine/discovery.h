#pragma once

/* Quick connect: finding the other copies of chiral-ftp on the network, and
   asking one of them to share.

   While the app is open it says hello to the network every few seconds
   (UDP broadcast, DISCOVERY_PORT) and listens for the others'. One device
   can then ask another to share: the other shows the request with a short
   code, and when the person there allows it, answers with a login that
   only works from the asking device's address. Every message is one small
   datagram of "key=value" lines; one thread owns the socket.

   Never fatal. Where the platform refuses any of it -- the socket, the
   port, broadcast -- discovery_start() says so (startup log) and the rest
   of the app carries on without quick connect. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISCOVERY_PORT 47121
#define DISCOVERY_MAX_PEERS 16
#define DISCOVERY_ASK_SECONDS 30  // how long a request waits for an answer

typedef struct discovery_peer {
  char id[17];     // random per run, to tell devices apart
  char name[64];   // "PS5", "Nintendo Switch", "Simo's MacBook Pro"
  char kind[16];   // "ps5", "switch", "mac", "windows", "linux"
  char version[24];
  char ip[48];
  unsigned short port;  // where its hellos come from: where to ask it
} discovery_peer_t;

/* A request from another device, waiting for the person here. */
typedef struct discovery_request {
  char nonce[17];
  char name[64];
  char kind[16];
  char ip[48];
  char code[5];  // shown on both screens, to be compared
  int seconds_left;
} discovery_request_t;

enum {
  DISCOVERY_IDLE,
  DISCOVERY_ASKING,
  DISCOVERY_ALLOWED,
  DISCOVERY_DENIED,
  DISCOVERY_BUSY,     // it was already answering someone else
  DISCOVERY_TIMEOUT,  // no answer in time
  DISCOVERY_FAILED,   // allowed, but its server wouldn't start
};

/* Where our own request stands. */
typedef struct discovery_outcome {
  int state;
  char code[5];
  char name[64];
  char ip[48];
  int seconds_left;
  /* On DISCOVERY_ALLOWED: how to log in. */
  char protocol[8];  // "ftp" or "sftp"
  unsigned short port;
  char user[40];
  char pass[48];
} discovery_outcome_t;

/* name and kind are how this device shows up on the others. 0, or -1:
   while a stop is still finishing (try again shortly), or with the reason
   in discovery_problem() and, the first time that reason comes up, in the
   startup log. Meant to be started and stopped often -- whenever quick
   connect is wanted and not -- by one caller, the UI.

   The thread also ends by itself when the socket stops working (an error
   from it, or sends failing over and over: the network gone, the console
   asleep and back). Starting again opens a fresh one. */
int discovery_start(const char *name, const char *kind);
void discovery_stop(void);  // asks; discovery_running() turns 0 when done
int discovery_running(void);
// Why the last start failed, in English for the log; "" when it didn't.
const char *discovery_problem(void);

// The devices heard from lately, by name.
int discovery_peers(discovery_peer_t *out, int max);
// Asks everyone to say hello now, rather than within a few seconds.
void discovery_probe(void);

/* Asking: one request at a time. The code to show is in the outcome. */
int discovery_ask(const discovery_peer_t *peer);
void discovery_get_outcome(discovery_outcome_t *out);
void discovery_cancel(void);  // tells the other device too

/* Being asked: 1 with the request while one waits for the person here.
   It goes away on its own when it is answered, withdrawn or too old.
   allow: 1 yes, 0 no, -1 yes but the server here couldn't start. */
int discovery_pending(discovery_request_t *out);
void discovery_answer(const char *nonce, int allow, const char *protocol, unsigned short port,
                      const char *user, const char *pass);

/* Random letters and digits, for a login handed out by an answer. */
void discovery_token(char *out, size_t size);

#ifdef __cplusplus
}
#endif
