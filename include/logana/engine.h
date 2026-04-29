#ifndef LOGANA_ENGINE_H
#define LOGANA_ENGINE_H

#include "logana/types.h"

uint64_t logana_now_ms(void);
uint64_t logana_hash64(const void *data, size_t len);

int logana_engine_init(logana_engine_t *engine, const logana_config_t *config);
void logana_engine_shutdown(logana_engine_t *engine);
logana_job_t *logana_engine_submit(logana_engine_t *engine, const char *payload, size_t payload_size, logana_algorithm_t algorithm);
logana_job_t *logana_engine_find_job(logana_engine_t *engine, uint64_t job_id);
int logana_analyze_job(logana_engine_t *engine, logana_job_t *job);
void logana_job_destroy(logana_job_t *job);

#endif
