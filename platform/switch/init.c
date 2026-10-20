/* Horizon services, brought up before main() and torn down after it.
   libnx calls userAppInit/userAppExit from its own startup code, which is
   where an .nro is expected to do this (the same place ftpd does it).

   The PS5 build had no equivalent: a native title inherits its services from
   the system. Here we have to ask for each one, and get to pick how much
   memory the socket driver may use -- which decides how fast transfers go. */

#include <switch.h>

#include <stdio.h>

#include "switch_init.h"

/* The socket driver's buffers. The defaults (32 KB send / 64 KB receive, 3
   sessions) are sized for a game that talks to a matchmaking server, not for
   a file server saturating the link; these are the same generous numbers
   mtheall's ftpd uses, which is what makes its transfers fast.

   sb_efficiency multiplies the per-socket buffer count, so the memory the
   driver reserves is roughly (tx_max + rx_max) * sb_efficiency. */
static const SocketInitConfig s_socket_config = {
    .tcp_tx_buf_size     = 1 * 1024 * 1024,
    .tcp_rx_buf_size     = 1 * 1024 * 1024,
    .tcp_tx_buf_max_size = 4 * 1024 * 1024,
    .tcp_rx_buf_max_size = 4 * 1024 * 1024,

    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,

    .sb_efficiency = 8,

    .num_bsd_sessions = 3,
    .bsd_service_type = BsdServiceType_User,
};

/* Filesystem sessions. chiral-ftp reads and writes from many threads at once
   (up to 30 transfer slots, plus the FTP server and the web app), and every
   session is a queue requests line up in; ftpd's single session would make
   them wait on each other. */
u32 __nx_fs_num_sessions = 8;

/* userAppInit runs before main, and before the app has picked its data
   folder, so it can't write the startup log yet. Each service's Result is
   kept here instead and traced once the log is usable -- otherwise a failed
   socketInitialize shows up only as "the FTP server won't start", with
   nothing saying why. */
static chiral_service_result_t s_results[4];
static int s_result_count;

static void
record(const char *name, Result rc) {
  if(s_result_count < (int)(sizeof(s_results) / sizeof(s_results[0]))) {
    s_results[s_result_count].name = name;
    s_results[s_result_count].rc = rc;
    s_result_count++;
  }
}

const chiral_service_result_t *
chiral_service_results(int *count) {
  *count = s_result_count;
  return s_results;
}

void
userAppInit(void) {
  /* The app decides when it exits (main returns), not the system: without
     this, closing from the HOME menu can kill us mid-transfer. */
  appletLockExit();
  /* An FTP server that falls asleep is not a server. */
  appletSetAutoSleepDisabled(true);

  /* pl:u, the system's shared fonts. Nothing uses them yet -- they arrive
     BFTTF-obfuscated and the renderer can't read them (docs/PORTING.md) --
     but the service is cheap and decoding them is the next step. */
  record("pl:u", plInitialize(PlServiceType_User));
  /* set:sys, for the console's language (app/i18n.cpp). */
  record("set:sys", setInitialize());
  /* nifm, for the console's IP address; gethostid() needs it. */
  record("nifm", nifmInitialize(NifmServiceType_User));
  /* The socket driver. Everything that listens or connects -- the FTP
     server, the web app, the client -- is dead in the water without it. */
  record("bsd (sockets)", socketInitialize(&s_socket_config));
}

void
userAppExit(void) {
  socketExit();
  nifmExit();
  setExit();
  plExit();
  appletSetAutoSleepDisabled(false);
  appletUnlockExit();
}
