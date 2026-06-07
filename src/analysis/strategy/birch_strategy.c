#include "logana/cluster_strategy.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define LOGANA_BIRCH_MAX_CLUSTERS 64

static double birch_estimate_threshold(const logana_feature_matrix_t *matrix,
                                       const logana_analysis_summary_t *summary) {
    if (!matrix || !matrix->values) return 1.0;
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
            double base = (ssum / (double)scnt) * 2.0;
            double mult = (summary->cluster_options.has_birch_auto_threshold)
                              ? summary->cluster_options.birch_auto_threshold
                              : 1.0;
            double thr = base * mult;
            return thr > 0.0 && isfinite(thr) ? thr : 1.0;
        }
    }
    /* Fallback: small fixed default when no summary available */
    return 1.0;
}

static size_t run_birch(const logana_feature_matrix_t *matrix, double threshold,
                        logana_distance_fn_t dist_fn,
                        const logana_analysis_summary_t *summary,
                        int *labels) {
    if (!matrix || !matrix->values || !labels || !dist_fn) return 0;
    size_t rows = matrix->row_count;
    size_t dims = matrix->dimensions;
    double *centroids = calloc(LOGANA_BIRCH_MAX_CLUSTERS * dims, sizeof(double));
    size_t *counts = calloc(LOGANA_BIRCH_MAX_CLUSTERS, sizeof(size_t));
    if (!centroids || !counts) {
        free(centroids); free(counts);
        return 0;
    }
    size_t cluster_count = 0;
    for (size_t r = 0; r < rows; ++r) {
        size_t best = 0;
        double best_dist = DBL_MAX;
        for (size_t c = 0; c < cluster_count; ++c) {
            double d = dist_fn(matrix->values + r * dims, centroids + c * dims,
                               matrix->valid_mask ? matrix->valid_mask + r * dims : NULL,
                               NULL, dims, summary);
            if (d < best_dist) { best_dist = d; best = c; }
        }
        if (cluster_count == 0 || best_dist > threshold) {
            if (cluster_count < LOGANA_BIRCH_MAX_CLUSTERS) {
                best = cluster_count++;
                memcpy(centroids + best * dims, matrix->values + r * dims, dims * sizeof(double));
            } else {
                best = LOGANA_BIRCH_MAX_CLUSTERS - 1;
            }
        }
        labels[r] = (int)best;
        counts[best]++;
        for (size_t d = 0; d < dims; ++d) {
            if (matrix->valid_mask && !matrix->valid_mask[r * dims + d]) continue;
            centroids[best * dims + d] = (centroids[best * dims + d] * (double)(counts[best] - 1) +
                                          matrix->values[r * dims + d]) / (double)counts[best];
        }
    }
    free(centroids);
    free(counts);
    return cluster_count;
}

static int birch_fit(const logana_cluster_strategy_vtable_t *self,
                     const logana_feature_matrix_t *matrix,
                     const logana_analysis_summary_t *summary,
                     logana_cluster_result_t *out) {
    if (!self || !matrix || !matrix->values || !out) return -1;
    size_t rows = matrix->row_count;
    if (!rows) return -1;
    int *labels = calloc(rows, sizeof(int));
    bool *is_noise = calloc(rows, sizeof(bool));
    if (!labels || !is_noise) { free(labels); free(is_noise); return -1; }

    double threshold = birch_estimate_threshold(matrix, summary);
    if (summary && summary->cluster_options.has_birch_threshold && summary->cluster_options.birch_threshold > 0.0) {
        threshold = summary->cluster_options.birch_threshold;
    }
    size_t clusters = run_birch(matrix, threshold, logana_distance_euclidean_sq, summary, labels);

    out->labels = labels;
    out->is_noise = is_noise;
    out->row_count = rows;
    out->cluster_count = clusters;
    out->noise_count = 0;
    out->silhouette_score = -1.0;
    out->davies_bouldin_index = INFINITY;
    return 0;
}

static const logana_cluster_strategy_vtable_t birch_vtable = {
    .name = "birch",
    .fit  = birch_fit,
};

const logana_cluster_strategy_vtable_t *logana_strategy_birch(void) {
    return &birch_vtable;
}
