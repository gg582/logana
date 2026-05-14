#ifndef LOGANA_RENDER_H
#define LOGANA_RENDER_H

#include "logana/types.h"

void *logana_render_dispatcher_main(void *arg);
int logana_render_job(logana_engine_t *engine, logana_job_t *job);
char *logana_job_status_json(logana_job_t *job);
char *logana_job_result_json(logana_job_t *job);
char *logana_job_report_page(logana_job_t *job);

#endif
