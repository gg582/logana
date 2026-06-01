#include "logana/pipeline.h"
#include <stdlib.h>
#include <string.h>

static int eval_init(void *state, const logana_config_t *config) {
    (void)state; (void)config;
    return 0;
}

static int eval_process(void *state, logana_pipeline_context_t *ctx) {
    (void)state;
    logana_job_t *job = ctx->job;

    /* Commit result into job */
    job->result = ctx->result;
    job->summary = ctx->summary;

    /* Also back-fill legacy matrix.labels so legacy render paths don't crash */
    if (job->matrix.labels) { free(job->matrix.labels); job->matrix.labels = NULL; }
    if (job->result.row_count > 0) {
        job->matrix.labels = calloc(job->result.row_count, sizeof(int));
        if (job->matrix.labels) {
            for (size_t i = 0; i < job->result.row_count; ++i) {
                job->matrix.labels[i] = job->result.labels[i];
            }
        }
    }

    /* Compute cluster balance */
    if (job->result.cluster_count > 1) {
        size_t *counts = calloc(job->result.cluster_count, sizeof(size_t));
        if (counts) {
            for (size_t i = 0; i < job->result.row_count; ++i) {
                int lb = job->result.labels[i];
                if (lb >= 0 && (size_t)lb < job->result.cluster_count) counts[lb]++;
            }
            size_t max_c = 0, min_c = job->result.row_count;
            for (size_t c = 0; c < job->result.cluster_count; ++c) {
                if (counts[c] > max_c) max_c = counts[c];
                if (counts[c] < min_c) min_c = counts[c];
            }
            job->summary.cluster_balance = max_c > 0 ? (double)min_c / (double)max_c : 0.0;
            free(counts);
        }
    } else {
        job->summary.cluster_balance = job->result.cluster_count == 1 ? 1.0 : 0.0;
    }

    return 0;
}

static void eval_cleanup(void *state) {
    (void)state;
}

static const logana_pipeline_stage_vtable_t eval_vtable = {
    .name    = "evaluation",
    .init    = eval_init,
    .process = eval_process,
    .cleanup = eval_cleanup,
};

logana_pipeline_stage_t logana_eval_stage(void) {
    return (logana_pipeline_stage_t){ &eval_vtable, NULL };
}
