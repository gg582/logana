#include "logana/pipeline.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

static int feature_init(void *state, const logana_config_t *config) {
    (void)state; (void)config;
    return 0;
}

static void feature_free_owned_matrix(logana_feature_matrix_t *m) {
    if (!m) return;
    free(m->values);
    free(m->valid_mask);
    free(m->timestamps);
    free(m->categories);
    free(m->formats);
    free(m->labels);
    memset(m, 0, sizeof(*m));
}

/**
 * @brief Stateless variance-threshold transformer.
 *        Drops dimensions whose variance (over valid-masked entries) is near zero.
 */
static int feature_process(void *state, logana_pipeline_context_t *ctx) {
    (void)state;
    logana_feature_matrix_t *src = &ctx->working_matrix;
    size_t rows = src->row_count;
    size_t dims = src->dimensions;
    if (rows == 0 || dims == 0) return 0;

    bool keep[LOGANA_MAX_DIMENSIONS] = {false};
    size_t new_dims = 0;
    const double VARIANCE_EPS = 1e-12;

    for (size_t d = 0; d < dims; ++d) {
        double sum = 0.0;
        size_t n = 0;
        for (size_t r = 0; r < rows; ++r) {
            if (src->valid_mask && !src->valid_mask[r * dims + d]) continue;
            sum += src->values[r * dims + d];
            ++n;
        }
        if (n == 0) continue;
        double mean = sum / (double)n;
        double var = 0.0;
        for (size_t r = 0; r < rows; ++r) {
            if (src->valid_mask && !src->valid_mask[r * dims + d]) continue;
            double delta = src->values[r * dims + d] - mean;
            var += delta * delta;
        }
        var /= (double)n;
        if (var > VARIANCE_EPS) {
            keep[d] = true;
            ++new_dims;
        }
    }

    /* Fallback: if everything was dropped, keep a synthetic primary dimension */
    if (new_dims == 0) {
        if (dims > 0) { keep[0] = true; new_dims = 1; }
        else return -1;
    }

    if (new_dims == dims) return 0; /* nothing to do */

    /* Allocate new dense matrix */
    double *nvalues = malloc(rows * new_dims * sizeof(double));
    uint8_t *nmask = malloc(rows * new_dims * sizeof(uint8_t));
    if (!nvalues || !nmask) { free(nvalues); free(nmask); return -1; }

    for (size_t r = 0; r < rows; ++r) {
        size_t nd = 0;
        for (size_t d = 0; d < dims; ++d) {
            if (!keep[d]) continue;
            nvalues[r * new_dims + nd] = src->values[r * dims + d];
            nmask[r * new_dims + nd] = src->valid_mask ? src->valid_mask[r * dims + d] : 1;
            ++nd;
        }
    }

    /* Take ownership of timestamps/categories/formats if we didn't already */
    uint64_t *nts = src->timestamps;
    uint64_t *ncats = src->categories;
    uint8_t  *nfmts = src->formats;

    /* If the source matrix was already owned by a previous stage,
       we steal the pointers; otherwise we duplicate them. */
    if (!ctx->working_matrix_owned) {
        if (!src->timestamps || !src->categories || !src->formats) {
            free(nvalues); free(nmask);
            return -1;
        }
        nts = malloc(rows * sizeof(uint64_t));
        ncats = malloc(rows * sizeof(uint64_t));
        nfmts = malloc(rows * sizeof(uint8_t));
        if (!nts || !ncats || !nfmts) {
            free(nvalues); free(nmask); free(nts); free(ncats); free(nfmts);
            return -1;
        }
        memcpy(nts, src->timestamps, rows * sizeof(uint64_t));
        memcpy(ncats, src->categories, rows * sizeof(uint64_t));
        memcpy(nfmts, src->formats, rows * sizeof(uint8_t));
    }

    /* Clean up previous owned matrix if any */
    if (ctx->working_matrix_owned) {
        feature_free_owned_matrix(src);
    }

    src->values = nvalues;
    src->valid_mask = nmask;
    src->timestamps = nts;
    src->categories = ncats;
    src->formats = nfmts;
    src->dimensions = new_dims;
    src->labels = NULL;
    ctx->working_matrix_owned = true;
    return 0;
}

static void feature_cleanup(void *state) {
    (void)state;
}

static const logana_pipeline_stage_vtable_t feature_vtable = {
    .name    = "feature_engineering",
    .init    = feature_init,
    .process = feature_process,
    .cleanup = feature_cleanup,
};

logana_pipeline_stage_t logana_feature_stage(void) {
    return (logana_pipeline_stage_t){ &feature_vtable, NULL };
}
