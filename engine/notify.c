#include "notify.h"

#include <stdarg.h>
#include <stdio.h>

#include "system.h"

/* Where "outside the app" means depends on the console -- a system
   notification on the PS5, the in-app log where there is no such thing.
   platform/<name>/system.c decides; this just formats. */
void
notify_user(const char *fmt, ...) {
  char message[512];
  va_list args;

  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  system_notify(message);
}
