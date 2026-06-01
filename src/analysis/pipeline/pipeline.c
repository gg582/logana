#include "logana/pipeline.h"

int logana_pipeline_execute(logana_pipeline_stage_t *stages, size_t stage_count,
                            logana_pipeline_context_t *ctx) {
    for (size_t i = 0; i < stage_count; ++i) {
        logana_pipeline_stage_t *stage = &stages[i];
        if (stage->vtable->init && stage->vtable->init(stage->state, ctx->config) != 0) {
            return -1;
        }
        if (stage->vtable->process && stage->vtable->process(stage->state, ctx) != 0) {
            if (stage->vtable->cleanup) stage->vtable->cleanup(stage->state);
            return -1;
        }
        if (stage->vtable->cleanup) stage->vtable->cleanup(stage->state);
    }
    return 0;
}
