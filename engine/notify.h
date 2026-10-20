#pragma once

/* Tells the user something worth noticing, by whatever means the console
   has (engine/system.h). */
void notify_user(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
