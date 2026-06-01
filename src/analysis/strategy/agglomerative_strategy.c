#include "logana/cluster_strategy.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static size_t run_agglomerative(const logana_feature_matrix_t *matrix, size_t target_clusters,
                                logana_distance_fn_t dist_fn,
                                const logana_analysis_summary_t *summary,
                                int *labels) {
    size_t rows = matrix->row_count;
    size_t dims = matrix->dimensions;
    if (rows > 1024) rows = 1024;
    int active[1024];
    for (size_t i = 0; i < rows; ++i) active[i] = (int)i;
    size_t clusters = rows;
    while (clusters > target_clusters && clusters > 1) {
        double best = 1e18;
        size_t best_i = 0, best_j = 1;
        for (size_t i = 0; i < rows; ++i) {
            if (active[i] < 0) continue;
            for (size_t j = i + 1; j < rows; ++j) {
                if (active[j] < 0) continue;
                double d = dist_fn(matrix->values + i * dims, matrix->values + j * dims,
                                   matrix->valid_mask + i * dims, matrix->valid_mask + j * dims, dims, summary);
                if (d < best) { best = d; best_i = i; best_j = j; }
            }
        }
        active[best_j] = active[best_i];
        clusters--;
    }
    int label_map[1024];
    memset(label_map, -1, sizeof(label_map));
    int next_label = 0;
    for (size_t i = 0; i < rows; ++i) {
        int root = active[i];
        if (root < 0) root = (int)i;
        if (label_map[root] < 0) label_map[root] = next_label++;
        labels[i] = label_map[root];
    }
    return (size_t)next_label;
}

static int agglomerative_fit(const logana_cluster_strategy_vtable_t *self,
                             const logana_feature_matrix_t *matrix,
                             const logana_analysis_summary_t *summary,
                             logana_cluster_result_t *out) {
    (void)self;
    size_t rows = matrix->row_count;
    if (!rows) return -1;
    int *labels = calloc(rows, sizeof(int));
    bool *is_noise = calloc(rows, sizeof(bool));
    if (!labels || !is_noise) { free(labels); free(is_noise); return -1; }

    size_t target = 3;
    if (target > rows) target = rows;
    run_agglomerative(matrix, target, logana_distance_euclidean_sq, summary, labels);

    out->labels = labels;
    out->is_noise = is_noise;
    out->row_count = rows;
    out->cluster_count = target;
    out->noise_count = 0;
    out->silhouette_score = -1.0;
    out->davies_bouldin_index = INFINITY;
    return 0;
}

static const logana_cluster_strategy_vtable_t agglomerative_vtable = {
    .name = "agglomerative",
    .fit  = agglomerative_fit,
};

const logana_cluster_strategy_vtable_t *logana_strategy_agglomerative(void) {
    return &agglomerative_vtable;
}
