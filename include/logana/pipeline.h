#ifndef LOGANA_PIPELINE_H
#define LOGANA_PIPELINE_H

#include "logana/types.h"
#include "logana/math.h"
#include "logana/cluster_strategy.h"

typedef struct {
    logana_job_t              *job;
    logana_feature_matrix_t    working_matrix;
    logana_analysis_summary_t  summary;
    logana_cluster_result_t    result;
    const logana_config_t     *config;
    logana_engine_t           *engine;
    bool                       working_matrix_owned;
} logana_pipeline_context_t;

typedef struct logana_pipeline_stage_vtable {
    const char *name;
    int (*init)(void *state, const logana_config_t *config);
    int (*process)(void *state, logana_pipeline_context_t *ctx);
    void (*cleanup)(void *state);
} logana_pipeline_stage_vtable_t;

typedef struct {
    const logana_pipeline_stage_vtable_t *vtable;
    void *state;
} logana_pipeline_stage_t;

/* Built-in stage constructors */
logana_pipeline_stage_t logana_preprocessing_stage(void);
logana_pipeline_stage_t logana_feature_stage(void);
logana_pipeline_stage_t logana_model_stage(void);
logana_pipeline_stage_t logana_eval_stage(void);

/* Orchestrator */
int logana_pipeline_execute(logana_pipeline_stage_t *stages, size_t stage_count,
                            logana_pipeline_context_t *ctx);

#endif
