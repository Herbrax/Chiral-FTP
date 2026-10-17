#pragma once

/* Shows a PS5 system notification; a no-op on host builds. */
void notify_user(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
