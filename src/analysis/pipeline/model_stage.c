#include "logana/pipeline.h"
#include <string.h>

static int model_init(void *state, const logana_config_t *config) {
    (void)state; (void)config;
    return 0;
}

static const logana_cluster_strategy_vtable_t *model_select_strategy(logana_algorithm_t algo) {
    switch (algo) {
        case LOGANA_ALGO_KMEANS_PP:       return logana_strategy_kmeans();
        case LOGANA_ALGO_DBSCAN:          return logana_strategy_dbscan();
        case LOGANA_ALGO_BIRCH:           return logana_strategy_birch();
        case LOGANA_ALGO_MEAN_SHIFT:      return logana_strategy_mean_shift();
        case LOGANA_ALGO_OPTICS:          return logana_strategy_optics();
        case LOGANA_ALGO_GMM:             return logana_strategy_gmm();
        case LOGANA_ALGO_AGGLOMERATIVE:   return logana_strategy_agglomerative();
        case LOGANA_ALGO_FALLBACK_SCATTERPLOT: return logana_strategy_fallback();
        case LOGANA_ALGO_AUTO:            return logana_strategy_auto();
    }
    return logana_strategy_fallback();
}

static int model_process(void *state, logana_pipeline_context_t *ctx) {
    (void)state;
    logana_job_t *job = ctx->job;
    const logana_cluster_strategy_vtable_t *strategy = model_select_strategy(job->algorithm);
    ctx->summary.cluster_options = job->cluster_options;

    logana_cluster_result_t out = {0};
    int rc = logana_strategy_fit(strategy, &ctx->working_matrix, &ctx->summary, &out);
    if (rc != 0) return -1;

    if (job->algorithm == LOGANA_ALGO_AUTO) {
        if (out.algorithm == LOGANA_ALGO_AUTO) {
            out.algorithm = LOGANA_ALGO_FALLBACK_SCATTERPLOT;
        }
    } else {
        out.algorithm = job->algorithm;
    }

    ctx->result = out;
    ctx->summary.cluster_count = out.cluster_count;
    return 0;
}

static void model_cleanup(void *state) {
    (void)state;
}

static const logana_pipeline_stage_vtable_t model_vtable = {
    .name    = "model_selection",
    .init    = model_init,
    .process = model_process,
    .cleanup = model_cleanup,
};

logana_pipeline_stage_t logana_model_stage(void) {
    return (logana_pipeline_stage_t){ &model_vtable, NULL };
}
