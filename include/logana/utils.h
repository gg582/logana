#ifndef LOGANA_UTILS_H
#define LOGANA_UTILS_H

#include "logana/types.h"

void logana_default_config(logana_config_t *config);
int logana_config_load(const char *path, logana_config_t *config);
const char *logana_status_name(logana_job_status_t status);
const char *logana_algorithm_name(logana_algorithm_t algorithm);
logana_algorithm_t logana_parse_algorithm(const char *name);

#endif
