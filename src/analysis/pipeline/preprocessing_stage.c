#include "logana/pipeline.h"
#include <string.h>

static int preprocessing_init(void *state, const logana_config_t *config) {
    (void)state; (void)config;
    return 0;
}

static int preprocessing_process(void *state, logana_pipeline_context_t *ctx) {
    (void)state;
    logana_engine_t *engine = ctx->engine;
    logana_job_t *job = ctx->job;

    size_t rows = logana_parse_matrix(engine, job);
    if (!rows) return -1;

    logana_compute_summary(job);

    /* Initialize working_matrix as a shallow view of job->matrix */
    ctx->working_matrix = job->matrix;
    ctx->working_matrix_owned = false;
    ctx->summary = job->summary;
    return 0;
}

static void preprocessing_cleanup(void *state) {
    (void)state;
}

static const logana_pipeline_stage_vtable_t preprocessing_vtable = {
    .name    = "preprocessing",
    .init    = preprocessing_init,
    .process = preprocessing_process,
    .cleanup = preprocessing_cleanup,
};

logana_pipeline_stage_t logana_preprocessing_stage(void) {
    return (logana_pipeline_stage_t){ &preprocessing_vtable, NULL };
}
