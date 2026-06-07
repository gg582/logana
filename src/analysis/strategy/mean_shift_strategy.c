#include "logana/cluster_strategy.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static size_t assign_cluster_modes(const double *modes, size_t mode_count, size_t dims,
                                   double *candidate, logana_distance_fn_t dist_fn,
                                   const logana_analysis_summary_t *summary) {
    for (size_t i = 0; i < mode_count; ++i) {
        if (dist_fn(modes + i * dims, candidate, NULL, NULL, dims, summary) < 0.04) return i;
    }
    if (mode_count < 16) {
        memcpy((double *)(modes + mode_count * dims), candidate, dims * sizeof(double));
    }
    return mode_count;
}

static size_t run_mean_shift(const logana_feature_matrix_t *matrix, double bandwidth,
                             logana_distance_fn_t dist_fn,
                             const logana_analysis_summary_t *summary,
                             int *labels, size_t ms_iters) {
    size_t rows = matrix->row_count;
    size_t dims = matrix->dimensions;
    /* Hard cap: mean-shift is O(n^2); beyond 10k rows it becomes a denial-of-service */
    size_t step = 1;
    if (rows > 10000) { step = rows / 10000; if (step < 1) step = 1; }
    double modes[LOGANA_MAX_DIMENSIONS * 16] = {0};
    size_t mode_count = 0;
    for (size_t r = 0; r < rows; r += step) {
        double point[LOGANA_MAX_DIMENSIONS];
        memcpy(point, matrix->values + r * dims, dims * sizeof(double));
        for (size_t iter = 0; iter < ms_iters; ++iter) {
            double accum[LOGANA_MAX_DIMENSIONS] = {0};
            size_t count = 0;
            for (size_t j = 0; j < rows; ++j) {
                if (dist_fn(point, matrix->values + j * dims,
                            NULL, matrix->valid_mask + j * dims, dims, summary) <= bandwidth) {
                    for (size_t d = 0; d < dims; ++d) {
                        if (matrix->valid_mask && !matrix->valid_mask[j * dims + d]) continue;
                        accum[d] += matrix->values[j * dims + d];
                    }
                    count++;
                }
            }
            if (!count) break;
            for (size_t d = 0; d < dims; ++d) point[d] = accum[d] / (double)count;
        }
        size_t label = assign_cluster_modes(modes, mode_count, dims, point, dist_fn, summary);
        if (label == mode_count && mode_count < 16) mode_count++;
        labels[r] = (int)label;
    }
    return mode_count;
}

static int mean_shift_fit(const logana_cluster_strategy_vtable_t *self,
                          const logana_feature_matrix_t *matrix,
                          const logana_analysis_summary_t *summary,
                          logana_cluster_result_t *out) {
    (void)self;
    size_t rows = matrix->row_count;
    if (!rows) return -1;
    int *labels = calloc(rows, sizeof(int));
    bool *is_noise = calloc(rows, sizeof(bool));
    if (!labels || !is_noise) { free(labels); free(is_noise); return -1; }

    double bandwidth = 2.0;
    if (summary->cluster_options.has_mean_shift_bandwidth && summary->cluster_options.mean_shift_bandwidth > 0.0) {
        bandwidth = summary->cluster_options.mean_shift_bandwidth;
    }
    size_t ms_iters = 6;
    if (summary->cluster_options.has_mean_shift_iterations && summary->cluster_options.mean_shift_iterations > 0) {
        ms_iters = summary->cluster_options.mean_shift_iterations;
    }
    size_t clusters = run_mean_shift(matrix, bandwidth, logana_distance_euclidean_sq, summary, labels, ms_iters);
    /* Fill skipped rows with nearest sampled label */
    size_t step = 1;
    if (rows > 10000) { step = rows / 10000; if (step < 1) step = 1; }
    if (step > 1) {
        for (size_t r = 0; r < rows; ++r) {
            size_t src = (r / step) * step;
            if (src >= rows) src = rows - 1;
            labels[r] = labels[src];
        }
    }

    out->labels = labels;
    out->is_noise = is_noise;
    out->row_count = rows;
    out->cluster_count = clusters;
    out->noise_count = 0;
    out->silhouette_score = -1.0;
    out->davies_bouldin_index = INFINITY;
    return 0;
}

static const logana_cluster_strategy_vtable_t mean_shift_vtable = {
    .name = "mean_shift",
    .fit  = mean_shift_fit,
};

const logana_cluster_strategy_vtable_t *logana_strategy_mean_shift(void) {
    return &mean_shift_vtable;
}
