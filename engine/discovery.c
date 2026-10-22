/* Quick connect (discovery.h): one UDP socket, one thread.

   Messages are text, one datagram each, at most MSG_MAX bytes: a first
   line "CHIRAL1 <TYPE>" and then "key=value" lines.

     HELLO    broadcast every HELLO_EVERY_MS, and to whoever sent a PROBE:
              id, name, kind, version
     PROBE    broadcast: "say hello now", with the hello's own fields so
              whoever hears it lists the sender too -- sent on every start,
              so the list fills in at once
     BYE      broadcast when quick connect stops here: id -- the others
              drop this device now rather than when its hellos stop
     REQUEST  to one device: nonce, code, name, kind -- resent until an
              ANSWER arrives, so a lost datagram costs a second, not the ask
     CANCEL   to the same device: nonce -- the person gave up waiting
     ANSWER   back to the asker: nonce, result=allow|deny|busy|failed, and on allow
              protocol, port, user, pass

   A reply goes back to the address and port its message came from, and a
   request to the port a device's hellos come from. Between two copies of
   the app that is DISCOVERY_PORT both ways; it also lets a test stand in
   for the other device on the same machine.

   Only system calls the app already makes elsewhere on every platform, plus
   sendto/recvfrom and SO_BROADCAST, which is the part to prove on a console:
   every step that can fail is reported once to the startup log, and a
   failure leaves quick connect off rather than the app down. */

#include "discovery.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.h"
#include "log.h"

#define MSG_MAX 512
#define HELLO_EVERY_MS 3000
#define PEER_FORGET_MS 12000  // four hellos missed
#define RESEND_EVERY_MS 1500
#define ANSWER_KEEP_MS 60000
#define OUTBOX_MAX 8

#define SEND_FAILURES_MAX 3  // hellos in a row: about nine seconds

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_stop;
static int g_running;
static char g_problem[160];
static int g_announced;  // the startup log has had its "visible as" line
static int g_bcast_err;   // why a broadcast didn't go out, until logged (once a launch)
static int g_bcast_logged;
static char g_id[17], g_name[64], g_kind[16];
static unsigned long long g_rng;

typedef struct {
  discovery_peer_t peer;
  long long seen;
} peer_slot_t;
static peer_slot_t g_peers[DISCOVERY_MAX_PEERS];

/* What we asked, and what came back. */
static struct {
  discovery_outcome_t out;
  char nonce[17];
  long long started, sent;
} g_ask;

/* What we are being asked, and what we last said back (sent again if the
   same request arrives again: our answer may be the datagram that got lost). */
static struct {
  int active;
  discovery_request_t req;
  long long arrived;
} g_incoming;
static struct {
  char nonce[17];
  char ip[48];
  unsigned short port;
  char msg[MSG_MAX];
  long long at;
} g_last_answer;
static unsigned short g_incoming_port, g_ask_port;

/* Messages the UI thread wants sent; the network thread sends them. */
static struct {
  char msg[MSG_MAX];
  char ip[48];  // "" for broadcast
  unsigned short port;
} g_outbox[OUTBOX_MAX];
static int g_outbox_n;

/* ---------- small things ---------- */

static long long
now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Not a cryptographic generator: a login it makes also needs the person on
   the other device to allow it, and works only from the asker's address.
   Stirred again with the time of every datagram that arrives. Lock held. */
static unsigned
rnd(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return (unsigned)(g_rng >> 32);
}

static void
stir(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  g_rng ^= ((unsigned long long)tv.tv_usec << 32) ^ (unsigned long long)tv.tv_sec ^ (uintptr_t)&tv;
  if(!g_rng) g_rng = 0x9E3779B97F4A7C15ull;
  rnd();
}

static void
hex(char *out, int digits) {
  static const char x[] = "0123456789abcdef";
  for(int i = 0; i < digits; i++) out[i] = x[rnd() & 15];
  out[digits] = 0;
}

void
discovery_token(char *out, size_t size) {
  static const char set[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  pthread_mutex_lock(&g_lock);
  stir();
  for(size_t i = 0; i + 1 < size; i++) out[i] = set[rnd() % (sizeof(set) - 1)];
  if(size) out[size - 1] = 0;
  pthread_mutex_unlock(&g_lock);
}

/* A value for a "key=value" line: no line breaks. */
static void
put(char *msg, size_t size, const char *key, const char *value) {
  size_t n = strlen(msg);
  if(n + strlen(key) + 3 >= size) return;
  n += (size_t)snprintf(msg + n, size - n, "%s=", key);
  for(const char *p = value ? value : ""; *p && n + 2 < size; p++) msg[n++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
  msg[n++] = '\n';
  msg[n] = 0;
}

static void
get(const char *msg, const char *key, char *out, size_t size) {
  const size_t k = strlen(key);
  out[0] = 0;
  for(const char *line = msg; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
    if(!strncmp(line, key, k) && line[k] == '=') {
      const char *v = line + k + 1;
      size_t n = 0;
      while(v[n] && v[n] != '\n' && n + 1 < size) n++;
      memcpy(out, v, n);
      out[n] = 0;
      return;
    }
  }
}

static void
begin(char *msg, const char *type) {
  snprintf(msg, MSG_MAX, "CHIRAL1 %s\n", type);
}

// Lock held. port 0: DISCOVERY_PORT.
static void
queue_msg(const char *msg, const char *ip, unsigned short port) {
  if(g_outbox_n >= OUTBOX_MAX) return;
  snprintf(g_outbox[g_outbox_n].msg, MSG_MAX, "%s", msg);
  snprintf(g_outbox[g_outbox_n].ip, sizeof(g_outbox[0].ip), "%s", ip ? ip : "");
  g_outbox[g_outbox_n].port = port;
  g_outbox_n++;
}

static void
hello(char *msg) {
  begin(msg, "HELLO");
  put(msg, MSG_MAX, "id", g_id);
  put(msg, MSG_MAX, "name", g_name);
  put(msg, MSG_MAX, "kind", g_kind);
  put(msg, MSG_MAX, "version", VERSION_TAG);
}

// A probe: "say hello", and here is who is asking.
static void
probe(char *msg) {
  begin(msg, "PROBE");
  put(msg, MSG_MAX, "id", g_id);
  put(msg, MSG_MAX, "name", g_name);
  put(msg, MSG_MAX, "kind", g_kind);
  put(msg, MSG_MAX, "version", VERSION_TAG);
}

/* ---------- the network thread ---------- */

// 0, or -1 when the datagram couldn't be sent.
static int
send_to(int fd, const char *msg, const char *ip, unsigned short port) {
  struct sockaddr_in to;

  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons(port ? port : DISCOVERY_PORT);
  if(!ip || !*ip) to.sin_addr.s_addr = htonl(0xFFFFFFFFu);  // everyone on this network
  else if(inet_pton(AF_INET, ip, &to.sin_addr) != 1) return 0;
  return sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&to, sizeof(to)) < 0 ? -1 : 0;
}

static void
answer_msg(char *msg, const char *nonce, const char *result) {
  begin(msg, "ANSWER");
  put(msg, MSG_MAX, "nonce", nonce);
  put(msg, MSG_MAX, "result", result);
}

/* A device heard from: its hello, or a probe carrying the same fields.
   Lock held. */
static void
seen_peer(const char *msg, const char *id, const char *ip, unsigned short port, long long t) {
  int slot = -1, oldest = 0;
  discovery_peer_t *p;

  if(!id[0]) return;
  for(int i = 0; i < DISCOVERY_MAX_PEERS; i++) {
    if(g_peers[i].seen && !strcmp(g_peers[i].peer.id, id)) slot = i;
    if(g_peers[i].seen < g_peers[oldest].seen) oldest = i;
  }
  if(slot < 0) slot = oldest;
  p = &g_peers[slot].peer;
  snprintf(p->id, sizeof(p->id), "%s", id);
  get(msg, "name", p->name, sizeof(p->name));
  get(msg, "kind", p->kind, sizeof(p->kind));
  get(msg, "version", p->version, sizeof(p->version));
  snprintf(p->ip, sizeof(p->ip), "%s", ip);
  p->port = port;
  if(!p->name[0]) snprintf(p->name, sizeof(p->name), "%s", ip);
  g_peers[slot].seen = t;
}

// One datagram that arrived, from ip:port. Lock held.
static void
handle(int fd, const char *msg, const char *ip, unsigned short port) {
  char type[16] = "", id[17], nonce[17], buf[MSG_MAX];
  const long long t = now_ms();

  sscanf(msg, "CHIRAL1 %15s", type);
  get(msg, "id", id, sizeof(id));
  if(id[0] && !strcmp(id, g_id)) return;  // our own broadcast, heard back

  if(!strcmp(type, "HELLO")) {
    seen_peer(msg, id, ip, port, t);
  } else if(!strcmp(type, "BYE")) {
    for(int i = 0; i < DISCOVERY_MAX_PEERS; i++) {
      if(g_peers[i].seen && !strcmp(g_peers[i].peer.id, id)) g_peers[i].seen = 0;
    }
    // Waiting on it: it will not answer now.
    if(g_ask.out.state == DISCOVERY_ASKING && !strcmp(g_ask.out.ip, ip)) g_ask.out.state = DISCOVERY_TIMEOUT;
  } else if(!strcmp(type, "PROBE")) {
    if(strstr(msg, "\nname=")) seen_peer(msg, id, ip, port, t);  // older builds' probes have only the id
    hello(buf);
    send_to(fd, buf, ip, port);
  } else if(!strcmp(type, "REQUEST")) {
    get(msg, "nonce", nonce, sizeof(nonce));
    if(!nonce[0]) return;
    if(!strcmp(nonce, g_last_answer.nonce) && t - g_last_answer.at < ANSWER_KEEP_MS) {
      send_to(fd, g_last_answer.msg, ip, port);  // asked again: our answer was lost
    } else if(g_incoming.active && strcmp(nonce, g_incoming.req.nonce)) {
      answer_msg(buf, nonce, "busy");  // someone else's request is on the screen
      send_to(fd, buf, ip, port);
    } else if(!g_incoming.active) {
      discovery_request_t *r = &g_incoming.req;
      memset(r, 0, sizeof(*r));
      snprintf(r->nonce, sizeof(r->nonce), "%s", nonce);
      get(msg, "name", r->name, sizeof(r->name));
      get(msg, "kind", r->kind, sizeof(r->kind));
      get(msg, "code", r->code, sizeof(r->code));
      snprintf(r->ip, sizeof(r->ip), "%s", ip);
      if(!r->name[0]) snprintf(r->name, sizeof(r->name), "%s", ip);
      g_incoming.active = 1;
      g_incoming.arrived = t;
      g_incoming_port = port;
    }
  } else if(!strcmp(type, "CANCEL")) {
    get(msg, "nonce", nonce, sizeof(nonce));
    if(g_incoming.active && !strcmp(nonce, g_incoming.req.nonce)) g_incoming.active = 0;
  } else if(!strcmp(type, "ANSWER")) {
    char result[16];
    get(msg, "nonce", nonce, sizeof(nonce));
    if(g_ask.out.state != DISCOVERY_ASKING || strcmp(nonce, g_ask.nonce)) return;
    get(msg, "result", result, sizeof(result));
    if(!strcmp(result, "allow")) {
      char port[8];
      get(msg, "protocol", g_ask.out.protocol, sizeof(g_ask.out.protocol));
      get(msg, "port", port, sizeof(port));
      get(msg, "user", g_ask.out.user, sizeof(g_ask.out.user));
      get(msg, "pass", g_ask.out.pass, sizeof(g_ask.out.pass));
      g_ask.out.port = (unsigned short)atoi(port);
      g_ask.out.state = g_ask.out.port ? DISCOVERY_ALLOWED : DISCOVERY_DENIED;
    } else {
      g_ask.out.state = !strcmp(result, "busy") ? DISCOVERY_BUSY : !strcmp(result, "failed") ? DISCOVERY_FAILED : DISCOVERY_DENIED;
    }
  }
}

/* The clock's part: hellos, resent requests, things that went stale.
   Returns 0 when the socket looks dead -- nothing at all has gone out for
   SEND_FAILURES_MAX hellos in a row -- and the thread should end (the UI
   starts a fresh one). Lock held. */
static int
tick(int fd, long long *last_hello, int *failures) {
  const long long t = now_ms();
  char buf[MSG_MAX];

  if(t - *last_hello >= HELLO_EVERY_MS) {
    int sent;
    hello(buf);
    sent = !send_to(fd, buf, NULL, 0);
    if(!sent && !g_bcast_err) g_bcast_err = errno ? errno : EIO;  // reported once, by the thread
    /* And straight to each device already known. Wi-Fi drops broadcasts
       far more often than it does datagrams to one address -- a console
       saving power hears few of them -- and a console may not send them at
       all, so this keeps both sides on each other's lists as long as
       either heard the other once. */
    for(int i = 0; i < DISCOVERY_MAX_PEERS; i++) {
      if(g_peers[i].seen && t - g_peers[i].seen <= PEER_FORGET_MS &&
         !send_to(fd, buf, g_peers[i].peer.ip, g_peers[i].peer.port)) {
        sent = 1;
      }
    }
    /* A refused broadcast alone isn't a dead socket while datagrams still
       reach the devices we know: restarting would only empty the list. */
    *failures = sent ? 0 : *failures + 1;
    *last_hello = t;
    if(*failures >= SEND_FAILURES_MAX) return 0;
  }
  for(int i = 0; i < DISCOVERY_MAX_PEERS; i++) {
    if(g_peers[i].seen && t - g_peers[i].seen > PEER_FORGET_MS) g_peers[i].seen = 0;
  }
  if(g_ask.out.state == DISCOVERY_ASKING) {
    if(t - g_ask.started > DISCOVERY_ASK_SECONDS * 1000LL) {
      g_ask.out.state = DISCOVERY_TIMEOUT;
    } else if(t - g_ask.sent >= RESEND_EVERY_MS) {
      begin(buf, "REQUEST");
      put(buf, MSG_MAX, "id", g_id);
      put(buf, MSG_MAX, "nonce", g_ask.nonce);
      put(buf, MSG_MAX, "code", g_ask.out.code);
      put(buf, MSG_MAX, "name", g_name);
      put(buf, MSG_MAX, "kind", g_kind);
      send_to(fd, buf, g_ask.out.ip, g_ask_port);
      g_ask.sent = t;
    }
  }
  /* A request nobody answered: refused, as the screen says it will be. */
  if(g_incoming.active && t - g_incoming.arrived > DISCOVERY_ASK_SECONDS * 1000LL) {
    answer_msg(buf, g_incoming.req.nonce, "deny");
    send_to(fd, buf, g_incoming.req.ip, g_incoming_port);
    g_incoming.active = 0;
  }
  for(int i = 0; i < g_outbox_n; i++) send_to(fd, g_outbox[i].msg, g_outbox[i].ip, g_outbox[i].port);
  g_outbox_n = 0;
  return 1;
}

static void *
discovery_main(void *arg) {
  const int fd = (int)(long)arg;
  long long last_hello = 0;
  int failures = 0, alive = 1;
  char buf[MSG_MAX + 1];

  while(!g_stop && alive) {
    struct pollfd pfd = {fd, POLLIN, 0};
    const int ready = poll(&pfd, 1, 100);
    if(ready > 0 && (pfd.revents & POLLIN)) {
      struct sockaddr_in from;
      socklen_t len = sizeof(from);
      const ssize_t n = recvfrom(fd, buf, MSG_MAX, 0, (struct sockaddr *)&from, &len);
      if(n > 8 && !strncmp(buf, "CHIRAL1 ", 8)) {
        char ip[48];
        buf[n] = 0;
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        pthread_mutex_lock(&g_lock);
        stir();
        handle(fd, buf, ip, ntohs(from.sin_port));
        pthread_mutex_unlock(&g_lock);
      }
    } else if(ready > 0 && (pfd.revents & (POLLERR | POLLNVAL))) {
      break;  // the socket died (a console asleep and back, say)
    }
    pthread_mutex_lock(&g_lock);
    alive = tick(fd, &last_hello, &failures);
    const int bcast_err = g_bcast_logged ? 0 : g_bcast_err;
    if(bcast_err) g_bcast_logged = 1;
    pthread_mutex_unlock(&g_lock);
    if(bcast_err) startup_trace("Quick connect: broadcasts refused (%s); reaching known devices directly", strerror(bcast_err));
  }
  if(g_stop) {  // stopped on purpose: tell the others (a dead socket can't)
    begin(buf, "BYE");
    put(buf, MSG_MAX, "id", g_id);
    send_to(fd, buf, NULL, 0);
  }
  close(fd);
  pthread_mutex_lock(&g_lock);
  /* Nothing heard from here on can be trusted to still be there: the list
     empties, a request on the screen goes (its asker will hear nothing and
     give up), and our own ask, which can no longer get its answer, ends as
     one that wasn't answered. */
  memset(g_peers, 0, sizeof(g_peers));
  g_incoming.active = 0;
  if(g_ask.out.state == DISCOVERY_ASKING) g_ask.out.state = DISCOVERY_TIMEOUT;
  g_outbox_n = 0;
  g_running = 0;
  pthread_mutex_unlock(&g_lock);
  return NULL;
}

/* The reason a start failed: kept for discovery_problem(), and written to
   the startup log only when it is new, since the UI tries again every few
   seconds and the message log is the user's. */
static int
problem(const char *fmt, ...) {
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  pthread_mutex_lock(&g_lock);
  const int fresh = strcmp(line, g_problem) != 0;
  snprintf(g_problem, sizeof(g_problem), "%s", line);
  pthread_mutex_unlock(&g_lock);
  if(fresh) startup_trace("%s", line);
  return -1;
}

const char *
discovery_problem(void) {
  return g_problem;
}

/* ---------- the UI's side ---------- */

int
discovery_start(const char *name, const char *kind) {
  struct sockaddr_in addr;
  pthread_t t;
  int fd, err;

  pthread_mutex_lock(&g_lock);
  if(g_running) {
    const int stopping = g_stop;
    pthread_mutex_unlock(&g_lock);
    if(stopping) {  // the last thread is still on its way out
      errno = EBUSY;
      return -1;
    }
    return 0;
  }
  stir();
  if(!g_id[0]) hex(g_id, 16);  // one id per launch: a restart is the same device
  snprintf(g_name, sizeof(g_name), "%s", name);
  snprintf(g_kind, sizeof(g_kind), "%s", kind);
  pthread_mutex_unlock(&g_lock);

  /* Each step on its own line in the startup log: on a console, the one
     that failed is the thing to know. */
  if((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) return problem("Quick connect off: no UDP socket (%s)", strerror(errno));
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
  if(setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &(int){1}, sizeof(int))) {
    const int err = errno;
    close(fd);
    return problem("Quick connect off: broadcast refused (%s)", strerror(err));
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(DISCOVERY_PORT);
  if(bind(fd, (struct sockaddr *)&addr, sizeof(addr))) {
    const int err = errno;
    close(fd);
    return problem("Quick connect off: UDP port %d unavailable (%s)", DISCOVERY_PORT, strerror(err));
  }
  pthread_mutex_lock(&g_lock);
  g_stop = 0;
  if((err = pthread_create(&t, NULL, discovery_main, (void *)(long)fd))) {
    pthread_mutex_unlock(&g_lock);
    close(fd);
    return problem("Quick connect off: no thread (%s)", strerror(err));
  }
  pthread_detach(t);
  g_running = 1;
  g_problem[0] = 0;
  {
    char msg[MSG_MAX];
    probe(msg);
    queue_msg(msg, NULL, 0);
  }
  const int first = !g_announced;
  g_announced = 1;
  pthread_mutex_unlock(&g_lock);
  if(first) startup_trace("Quick connect: visible as \"%s\" on UDP %d", name, DISCOVERY_PORT);
  return 0;
}

void
discovery_stop(void) {
  g_stop = 1;
}

int
discovery_running(void) {
  int r;
  pthread_mutex_lock(&g_lock);
  r = g_running;
  pthread_mutex_unlock(&g_lock);
  return r;
}

int
discovery_peers(discovery_peer_t *out, int max) {
  int n = 0;
  pthread_mutex_lock(&g_lock);
  /* By name, so the list stays put while it fills in. */
  for(int pass = 0; pass < DISCOVERY_MAX_PEERS && n < max; pass++) {
    int best = -1;
    for(int i = 0; i < DISCOVERY_MAX_PEERS; i++) {
      if(!g_peers[i].seen) continue;
      int taken = 0;
      for(int k = 0; k < n; k++) if(!strcmp(out[k].id, g_peers[i].peer.id)) taken = 1;
      if(!taken && (best < 0 || strcmp(g_peers[i].peer.name, g_peers[best].peer.name) < 0)) best = i;
    }
    if(best < 0) break;
    out[n++] = g_peers[best].peer;
  }
  pthread_mutex_unlock(&g_lock);
  return n;
}

void
discovery_probe(void) {
  char msg[MSG_MAX];
  pthread_mutex_lock(&g_lock);
  if(g_running) {
    probe(msg);
    queue_msg(msg, NULL, 0);
  }
  pthread_mutex_unlock(&g_lock);
}

int
discovery_ask(const discovery_peer_t *peer) {
  pthread_mutex_lock(&g_lock);
  if(!g_running) {
    pthread_mutex_unlock(&g_lock);
    return -1;
  }
  stir();
  memset(&g_ask, 0, sizeof(g_ask));
  hex(g_ask.nonce, 16);
  snprintf(g_ask.out.code, sizeof(g_ask.out.code), "%04u", rnd() % 10000);
  snprintf(g_ask.out.name, sizeof(g_ask.out.name), "%s", peer->name);
  snprintf(g_ask.out.ip, sizeof(g_ask.out.ip), "%s", peer->ip);
  g_ask_port = peer->port;
  g_ask.out.state = DISCOVERY_ASKING;
  g_ask.started = now_ms();
  g_ask.sent = 0;  // the network thread sends it straight away
  pthread_mutex_unlock(&g_lock);
  return 0;
}

void
discovery_get_outcome(discovery_outcome_t *out) {
  pthread_mutex_lock(&g_lock);
  *out = g_ask.out;
  if(out->state == DISCOVERY_ASKING) {
    const long long left = DISCOVERY_ASK_SECONDS * 1000LL - (now_ms() - g_ask.started);
    out->seconds_left = left > 0 ? (int)((left + 999) / 1000) : 0;
  }
  pthread_mutex_unlock(&g_lock);
}

void
discovery_cancel(void) {
  char msg[MSG_MAX];
  pthread_mutex_lock(&g_lock);
  if(g_ask.out.state == DISCOVERY_ASKING) {
    begin(msg, "CANCEL");
    put(msg, MSG_MAX, "id", g_id);
    put(msg, MSG_MAX, "nonce", g_ask.nonce);
    queue_msg(msg, g_ask.out.ip, g_ask_port);
  }
  memset(&g_ask, 0, sizeof(g_ask));
  pthread_mutex_unlock(&g_lock);
}

int
discovery_pending(discovery_request_t *out) {
  int active;
  pthread_mutex_lock(&g_lock);
  active = g_incoming.active;
  if(active) {
    *out = g_incoming.req;
    const long long left = DISCOVERY_ASK_SECONDS * 1000LL - (now_ms() - g_incoming.arrived);
    out->seconds_left = left > 0 ? (int)((left + 999) / 1000) : 0;
  }
  pthread_mutex_unlock(&g_lock);
  return active;
}

void
discovery_answer(const char *nonce, int allow, const char *protocol, unsigned short port, const char *user,
                 const char *pass) {
  char msg[MSG_MAX], num[8];
  pthread_mutex_lock(&g_lock);
  if(!g_incoming.active || strcmp(nonce, g_incoming.req.nonce)) {
    pthread_mutex_unlock(&g_lock);
    return;  // withdrawn, or already timed out
  }
  answer_msg(msg, nonce, allow > 0 ? "allow" : allow < 0 ? "failed" : "deny");
  if(allow > 0) {
    snprintf(num, sizeof(num), "%u", port);
    put(msg, MSG_MAX, "protocol", protocol);
    put(msg, MSG_MAX, "port", num);
    put(msg, MSG_MAX, "user", user);
    put(msg, MSG_MAX, "pass", pass);
  }
  queue_msg(msg, g_incoming.req.ip, g_incoming_port);
  snprintf(g_last_answer.nonce, sizeof(g_last_answer.nonce), "%s", nonce);
  snprintf(g_last_answer.ip, sizeof(g_last_answer.ip), "%s", g_incoming.req.ip);
  g_last_answer.port = g_incoming_port;
  snprintf(g_last_answer.msg, sizeof(g_last_answer.msg), "%s", msg);
  g_last_answer.at = now_ms();
  g_incoming.active = 0;
  pthread_mutex_unlock(&g_lock);
}
