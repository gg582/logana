#include "logana/cluster_strategy.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define LOGANA_MEAN_SHIFT_MAX_MODES 64
#define LOGANA_MEAN_SHIFT_ROW_CAP 10000

static size_t assign_cluster_modes(const double *modes, size_t mode_count, size_t dims,
                                   const double *candidate, logana_distance_fn_t dist_fn,
                                   const logana_analysis_summary_t *summary,
                                   double merge_threshold_sq) {
    if (!modes || !candidate || !dist_fn) return mode_count;
    for (size_t i = 0; i < mode_count; ++i) {
        if (dist_fn(modes + i * dims, candidate, NULL, NULL, dims, summary) < merge_threshold_sq)
            return i;
    }
    return mode_count;
}

static double mean_shift_estimate_bandwidth(const logana_feature_matrix_t *matrix,
                                            const logana_analysis_summary_t *summary) {
    if (summary) {
        double ssum = 0.0;
        size_t scnt = 0;
        for (size_t d = 0; d < matrix->dimensions && d < LOGANA_MAX_DIMENSIONS; ++d) {
            if (isfinite(summary->stddev[d]) && summary->stddev[d] > 0.0) {
                ssum += summary->stddev[d];
                scnt++;
            }
        }
        if (scnt > 0) {
            double bw = (ssum / (double)scnt) * 2.0;
            return bw > 0.0 && isfinite(bw) ? bw : 1.0;
        }
    }
    /* Fallback: median pairwise distance among a small sample */
    size_t sample = matrix->row_count > 256 ? 256 : matrix->row_count;
    if (sample < 2) return 1.0;
    double *pd = malloc((sample * (sample - 1) / 2) * sizeof(double));
    if (!pd) return 1.0;
    size_t pidx = 0;
    for (size_t i = 0; i < sample; ++i) {
        for (size_t j = i + 1; j < sample; ++j) {
            double d = logana_distance_euclidean_sq(
                matrix->values + i * matrix->dimensions,
                matrix->values + j * matrix->dimensions,
                matrix->valid_mask ? matrix->valid_mask + i * matrix->dimensions : NULL,
                matrix->valid_mask ? matrix->valid_mask + j * matrix->dimensions : NULL,
                matrix->dimensions, summary);
            if (isfinite(d)) pd[pidx++] = sqrt(d);
        }
    }
    double bw = 1.0;
    if (pidx > 0) {
        /* simple select-sort for median to avoid qsort overhead on small set */
        for (size_t i = 0; i < pidx; ++i) {
            size_t min_idx = i;
            for (size_t j = i + 1; j < pidx; ++j) {
                if (pd[j] < pd[min_idx]) min_idx = j;
            }
            if (min_idx != i) { double t = pd[i]; pd[i] = pd[min_idx]; pd[min_idx] = t; }
        }
        bw = pd[pidx / 2];
        if (bw <= 0.0 || !isfinite(bw)) bw = 1.0;
    }
    free(pd);
    return bw;
}

static size_t run_mean_shift(const logana_feature_matrix_t *matrix, double bandwidth,
                             logana_distance_fn_t dist_fn,
                             const logana_analysis_summary_t *summary,
                             int *labels, size_t ms_iters) {
    if (!matrix || !matrix->values || !labels || !dist_fn) return 0;
    size_t rows = matrix->row_count;
    size_t dims = matrix->dimensions;
    size_t step = 1;
    if (rows > LOGANA_MEAN_SHIFT_ROW_CAP) { step = rows / LOGANA_MEAN_SHIFT_ROW_CAP; if (step < 1) step = 1; }

    double *modes = calloc(LOGANA_MEAN_SHIFT_MAX_MODES * dims, sizeof(double));
    double *point = malloc(dims * sizeof(double));
    double *accum = calloc(dims, sizeof(double));
    if (!modes || !point || !accum) {
        free(modes); free(point); free(accum);
        return 0;
    }

    double merge_threshold_sq = bandwidth * bandwidth * 0.04;
    if (summary && summary->cluster_options.has_mean_shift_merge_threshold) {
        merge_threshold_sq = summary->cluster_options.mean_shift_merge_threshold;
    }

    size_t mode_count = 0;
    for (size_t r = 0; r < rows; r += step) {
        memcpy(point, matrix->values + r * dims, dims * sizeof(double));
        for (size_t iter = 0; iter < ms_iters; ++iter) {
            memset(accum, 0, dims * sizeof(double));
            size_t count = 0;
            for (size_t j = 0; j < rows; ++j) {
                double dist = dist_fn(point, matrix->values + j * dims,
                                   NULL,
                                   matrix->valid_mask ? matrix->valid_mask + j * dims : NULL,
                                   dims, summary);
                if (dist <= bandwidth) {
                    for (size_t dim_idx = 0; dim_idx < dims; ++dim_idx) {
                        if (matrix->valid_mask && !matrix->valid_mask[j * dims + dim_idx]) continue;
                        accum[dim_idx] += matrix->values[j * dims + dim_idx];
                    }
                    count++;
                }
            }
            if (!count) break;
            for (size_t d = 0; d < dims; ++d) point[d] = accum[d] / (double)count;
        }
        size_t label = assign_cluster_modes(modes, mode_count, dims, point, dist_fn, summary, merge_threshold_sq);
        if (label == mode_count && mode_count < LOGANA_MEAN_SHIFT_MAX_MODES) {
            memcpy(modes + mode_count * dims, point, dims * sizeof(double));
            mode_count++;
        }
        labels[r] = (int)label;
    }
    free(modes);
    free(point);
    free(accum);
    return mode_count;
}

static int mean_shift_fit(const logana_cluster_strategy_vtable_t *self,
                          const logana_feature_matrix_t *matrix,
                          const logana_analysis_summary_t *summary,
                          logana_cluster_result_t *out) {
    if (!self || !matrix || !matrix->values || !out) return -1;
    size_t rows = matrix->row_count;
    if (!rows) return -1;
    int *labels = calloc(rows, sizeof(int));
    bool *is_noise = calloc(rows, sizeof(bool));
    if (!labels || !is_noise) { free(labels); free(is_noise); return -1; }

    double bandwidth = mean_shift_estimate_bandwidth(matrix, summary);
    if (summary && summary->cluster_options.has_mean_shift_bandwidth && summary->cluster_options.mean_shift_bandwidth > 0.0) {
        bandwidth = summary->cluster_options.mean_shift_bandwidth;
    }
    size_t ms_iters = 6;
    if (summary && summary->cluster_options.has_mean_shift_iterations && summary->cluster_options.mean_shift_iterations > 0) {
        ms_iters = summary->cluster_options.mean_shift_iterations;
    }
    size_t clusters = run_mean_shift(matrix, bandwidth, logana_distance_euclidean_sq, summary, labels, ms_iters);
    size_t step = 1;
    if (rows > LOGANA_MEAN_SHIFT_ROW_CAP) { step = rows / LOGANA_MEAN_SHIFT_ROW_CAP; if (step < 1) step = 1; }
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
