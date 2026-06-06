#include "logana/pipeline.h"

#include <stdlib.h>
#include <string.h>

static void logana_pipeline_free_matrix(logana_feature_matrix_t *m) {
    if (!m) return;
    free(m->values);
    free(m->valid_mask);
    free(m->timestamps);
    free(m->categories);
    free(m->formats);
    free(m->labels);
    free(m->outlier_pressure);
    memset(m, 0, sizeof(*m));
}

static void logana_pipeline_free_result(logana_cluster_result_t *r) {
    if (!r) return;
    free(r->labels);
    free(r->is_noise);
    memset(r, 0, sizeof(*r));
}

int logana_pipeline_execute(logana_pipeline_stage_t *stages, size_t stage_count,
                            logana_pipeline_context_t *ctx) {
    for (size_t i = 0; i < stage_count; ++i) {
        logana_pipeline_stage_t *stage = &stages[i];
        if (stage->vtable->init && stage->vtable->init(stage->state, ctx->config) != 0) {
            if (ctx->working_matrix_owned) logana_pipeline_free_matrix(&ctx->working_matrix);
            logana_pipeline_free_result(&ctx->result);
            return -1;
        }
        if (stage->vtable->process && stage->vtable->process(stage->state, ctx) != 0) {
            if (stage->vtable->cleanup) stage->vtable->cleanup(stage->state);
            if (ctx->working_matrix_owned) logana_pipeline_free_matrix(&ctx->working_matrix);
            logana_pipeline_free_result(&ctx->result);
            return -1;
        }
        if (stage->vtable->cleanup) stage->vtable->cleanup(stage->state);
    }
    if (ctx->working_matrix_owned) logana_pipeline_free_matrix(&ctx->working_matrix);
    return 0;
}
