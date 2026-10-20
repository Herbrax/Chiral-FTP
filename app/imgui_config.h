#pragma once

/* Dear ImGui settings for chiral-ftp (IMGUI_USER_CONFIG, set by the builds).

   A failed ImGui check is written to the startup log and the app carries on,
   instead of the console killing it with "abort is called" and nothing to
   show why. */

#ifdef __cplusplus
extern "C"
#endif
    void chiral_imgui_check_failed(const char *expr, const char *file, int line);

#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : chiral_imgui_check_failed(#_EXPR, __FILE__, __LINE__))

/* ImGui's default "open this link/path in the shell" helper calls execvp()
   and waitpid(). Horizon has neither, and nothing in chiral-ftp opens a
   shell, so drop the helper rather than stub the process API. */
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
