/*
 * chiral-ftp's self-updater: what the app and the helper say to each other.
 * The boilerplate's self_update_protocol.h (ps5-native-app-boilerplate,
 * GPL-3.0-or-later), without the SHA-256: chiral-ftp learns of an update from
 * its own update API, which gives the release's size, not a digest.
 *
 * The app sends the helper program to the console's payload loader; the same
 * connection then carries this conversation. Lines end with '\n'.
 *
 *   app     CHU1, update, <title ID>, <installed content version>,
 *           <new content version>, <size in bytes>, <app name>,
 *           <version name>                         (eight lines)
 *   helper  ready                                  or  fail <reason>
 *   app     the archive in pieces: a four-byte little-endian length, then
 *           that many bytes (at most CHIRAL_UPDATE_PIECE); a length of zero ends
 *   helper  p <bytes unpacked> <bytes to unpack>   five times a second
 *   helper  staged                                 or  fail <reason>
 *   app     apply                                  or  cancel
 *   helper  applying
 *
 * After "applying" the app closes itself. The helper waits until the app is
 * gone, puts the new files in place, deletes the old ones and posts a
 * notification. A connection that ends before "apply", or "cancel" at any time
 * before it, makes the helper remove everything it wrote and leave the app
 * untouched.
 */
#ifndef CHIRAL_UPDATE_PROTOCOL_H
#define CHIRAL_UPDATE_PROTOCOL_H

#define CHIRAL_UPDATE_MAGIC "CHU1"
#define CHIRAL_UPDATE_LINE 512u                 /* longest line either side accepts */
#define CHIRAL_UPDATE_PIECE (1u << 20)          /* largest piece of the archive */
#define CHIRAL_UPDATE_MAX_ARCHIVE 0x40000000ull /* 1 GiB: far more than a release */
#define CHIRAL_UPDATE_LOADER_PORT 9021

#endif
