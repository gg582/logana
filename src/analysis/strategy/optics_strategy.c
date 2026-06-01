#include "logana/cluster_strategy.h"
#include "logana/math.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    size_t idx;
    double dist;
} neighbor_t;

static size_t run_optics(const logana_feature_matrix_t *matrix, double eps, size_t min_samples,
                         logana_distance_fn_t dist_fn,
                         const logana_analysis_summary_t *summary,
                         int *labels) {
    size_t rows = matrix->row_count;
    size_t dims = matrix->dimensions;
    if (rows > 4096) rows = 4096;
    neighbor_t *neighbors = malloc(rows * sizeof(neighbor_t));
    if (!neighbors) return 0;
    for (size_t i = 0; i < rows; ++i) {
        size_t count = 0;
        for (size_t j = 0; j < rows; ++j) {
            double d = dist_fn(matrix->values + i * dims, matrix->values + j * dims,
                               matrix->valid_mask + i * dims, matrix->valid_mask + j * dims, dims, summary);
            if (d <= eps) count++;
        }
        neighbors[i].idx = i;
        neighbors[i].dist = -(double)count;
    }
    for (size_t i = 0; i < rows; ++i) {
        for (size_t j = i + 1; j < rows; ++j) {
            if (neighbors[j].dist < neighbors[i].dist) {
                neighbor_t tmp = neighbors[i];
                neighbors[i] = neighbors[j];
                neighbors[j] = tmp;
            }
        }
    }
    size_t cluster = 0;
    for (size_t rank = 0; rank < rows; ++rank) {
        size_t idx = neighbors[rank].idx;
        labels[idx] = (rank < min_samples) ? 0 : (int)cluster;
        if ((rank + 1) % min_samples == 0) cluster++;
    }
    free(neighbors);
    return cluster + 1;
}

static int optics_fit(const logana_cluster_strategy_vtable_t *self,
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
    eps *= 1.2;

    size_t clusters = run_optics(matrix, eps, min_samples, dist_fn, summary, labels);

    out->labels = labels;
    out->is_noise = is_noise;
    out->row_count = rows;
    out->cluster_count = clusters;
    out->noise_count = 0;
    out->silhouette_score = -1.0;
    out->davies_bouldin_index = INFINITY;
    return 0;
}

static const logana_cluster_strategy_vtable_t optics_vtable = {
    .name = "optics",
    .fit  = optics_fit,
};

const logana_cluster_strategy_vtable_t *logana_strategy_optics(void) {
    return &optics_vtable;
}
