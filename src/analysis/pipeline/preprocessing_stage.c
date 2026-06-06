#include "logana/pipeline.h"
#include "logana/coercion.h"
#include <string.h>

static int preprocessing_init(void *state, const logana_config_t *config) {
    (void)state; (void)config;
    return 0;
}

static int preprocessing_process(void *state, logana_pipeline_context_t *ctx) {
    (void)state;
    logana_engine_t *engine = ctx->engine;
    logana_job_t *job = ctx->job;

    /* Polymorphic coercion pipeline (Rules 1-5) */
    logana_coercion_context_t coerced;
    if (logana_coercion_init(&coerced) != 0) return -1;
    if (logana_coercion_parse_payload(engine, job, &coerced) != 0) {
        logana_coercion_destroy(&coerced);
        return -1;
    }
    if (logana_coercion_export_matrix(&coerced, job, engine) != 0) {
        logana_coercion_destroy(&coerced);
        return -1;
    }
    logana_coercion_destroy(&coerced);

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
