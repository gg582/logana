#include "logana/cluster_strategy.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fallback_fit(const logana_cluster_strategy_vtable_t *self,
                        const logana_feature_matrix_t *matrix,
                        const logana_analysis_summary_t *summary,
                        logana_cluster_result_t *out) {
    if (!self || !matrix || !out) return -1;
    (void)summary; /* fallback does not require summary statistics */
    size_t rows = matrix->row_count;
    int *labels = NULL;
    bool *is_noise = NULL;
    if (rows > 0) {
        labels = calloc(rows, sizeof(int));
        is_noise = calloc(rows, sizeof(bool));
        if (!labels || !is_noise) { free(labels); free(is_noise); return -1; }
        for (size_t i = 0; i < rows; ++i) {
            labels[i] = -1;
            is_noise[i] = true;
        }
    }
    out->labels = labels;
    out->is_noise = is_noise;
    out->row_count = rows;
    out->cluster_count = 0;
    out->noise_count = rows;
    out->silhouette_score = -1.0;
    out->davies_bouldin_index = INFINITY;
    return 0;
}

static const logana_cluster_strategy_vtable_t fallback_vtable = {
    .name = "fallback_scatterplot",
    .fit  = fallback_fit,
};

const logana_cluster_strategy_vtable_t *logana_strategy_fallback(void) {
    return &fallback_vtable;
}
