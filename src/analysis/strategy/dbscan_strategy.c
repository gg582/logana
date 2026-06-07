#include "logana/cluster_strategy.h"
#include "logana/math.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static size_t run_dbscan(const logana_feature_matrix_t *matrix, double eps, size_t min_samples,
                         logana_distance_fn_t dist_fn,
                         const logana_analysis_summary_t *summary,
                         int *labels) {
    size_t rows = matrix->row_count;
    if (rows > 65536) rows = 65536;
    size_t dims = matrix->dimensions;
    for (size_t i = 0; i < rows; ++i) labels[i] = -2;
    size_t cluster_id = 0;
    for (size_t i = 0; i < rows; ++i) {
        if (labels[i] != -2) continue;
        size_t count = 0;
        for (size_t j = 0; j < rows; ++j) {
            if (dist_fn(matrix->values + i * dims, matrix->values + j * dims,
                        matrix->valid_mask + i * dims, matrix->valid_mask + j * dims, dims, summary) <= eps)
                count++;
        }
        if (count < min_samples) { labels[i] = -1; continue; }
        labels[i] = (int)cluster_id;
        for (size_t j = 0; j < rows; ++j) {
            if (labels[j] == -2 &&
                dist_fn(matrix->values + i * dims, matrix->values + j * dims,
                        matrix->valid_mask + i * dims, matrix->valid_mask + j * dims, dims, summary) <= eps) {
                labels[j] = (int)cluster_id;
            }
        }
        cluster_id++;
    }
    return cluster_id;
}

static int dbscan_fit(const logana_cluster_strategy_vtable_t *self,
                      const logana_feature_matrix_t *matrix,
                      const logana_analysis_summary_t *summary,
                      logana_cluster_result_t *out) {
    (void)self;
    size_t rows = matrix->row_count;
    if (!rows) return -1;
    int *labels = calloc(rows, sizeof(int));
    bool *is_noise = calloc(rows, sizeof(bool));
    if (!labels || !is_noise) { free(labels); free(is_noise); return -1; }

    logana_distance_fn_t dist_fn = logana_distance_euclidean_sq;
    size_t min_samples = 3;
    if (rows > 100) min_samples = 5;
    double eps = logana_knee_detect_eps(matrix, min_samples, dist_fn, summary);
    /* Scale eps slightly to be permissive for log data */
    eps *= 1.05;

    size_t clusters = run_dbscan(matrix, eps, min_samples, dist_fn, summary, labels);

    size_t noise = 0;
    for (size_t i = 0; i < rows; ++i) {
        if (labels[i] < 0) { is_noise[i] = true; noise++; }
    }

    out->labels = labels;
    out->is_noise = is_noise;
    out->row_count = rows;
    out->cluster_count = clusters;
    out->noise_count = noise;
    out->silhouette_score = -1.0;
    out->davies_bouldin_index = INFINITY;
    return 0;
}

static const logana_cluster_strategy_vtable_t dbscan_vtable = {
    .name = "dbscan",
    .fit  = dbscan_fit,
};

const logana_cluster_strategy_vtable_t *logana_strategy_dbscan(void) {
    return &dbscan_vtable;
}
