#pragma once

/* What app/switch_init.c's userAppInit() managed to bring up. Reported to
   the startup log by main() once the log exists, because userAppInit runs
   before the data folder has been chosen. */

#include <switch.h>

typedef struct chiral_service_result {
  const char *name;
  Result rc;
} chiral_service_result_t;

#ifdef __cplusplus
extern "C"
#endif
    const chiral_service_result_t *chiral_service_results(int *count);
