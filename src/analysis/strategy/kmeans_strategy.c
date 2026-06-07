#include "logana/cluster_strategy.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint64_t lcg_next(uint64_t *state) {
    *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
    return *state;
}

static double lcg_double01(uint64_t *state) {
    return (double)(lcg_next(state) >> 11) * (1.0 / (double)(1ULL << 53));
}

static double kmeans_inertia(const logana_feature_matrix_t *matrix, size_t k,
                             const double *centers, logana_distance_fn_t dist_fn,
                             const logana_analysis_summary_t *summary) {
    size_t rows = matrix->row_count;
    size_t dims = matrix->dimensions;
    double inertia = 0.0;
    for (size_t r = 0; r < rows; ++r) {
        double nearest = 1e18;
        for (size_t c = 0; c < k; ++c) {
            double d = dist_fn(matrix->values + r * dims, centers + c * dims,
                               matrix->valid_mask + r * dims, NULL, dims, summary);
            if (d < nearest) nearest = d;
        }
        inertia += nearest;
    }
    return inertia;
}

static void kmeans_pp_init(const logana_feature_matrix_t *matrix, size_t k, uint64_t seed,
                           double *centers, logana_distance_fn_t dist_fn,
                           const logana_analysis_summary_t *summary) {
    size_t rows = matrix->row_count;
    size_t dims = matrix->dimensions;
    if (!rows || !k) return;
    uint64_t rng = seed;
    size_t first_idx = (size_t)(lcg_double01(&rng) * (double)rows);
    if (first_idx >= rows) first_idx = rows - 1;
    memcpy(centers, matrix->values + first_idx * dims, dims * sizeof(double));
    if (k == 1) return;
    double *dists = malloc(rows * sizeof(double));
    if (!dists) {
        for (size_t c = 1; c < k; ++c) {
            size_t idx = (rows * c) / k;
            memcpy(centers + c * dims, matrix->values + idx * dims, dims * sizeof(double));
        }
        return;
    }
    for (size_t c = 1; c < k; ++c) {
        double dist_sum = 0.0;
        for (size_t r = 0; r < rows; ++r) {
            double nearest = 1e18;
            for (size_t j = 0; j < c; ++j) {
                double d = dist_fn(matrix->values + r * dims, centers + j * dims,
                                   matrix->valid_mask + r * dims, NULL, dims, summary);
                if (d < nearest) nearest = d;
            }
            dists[r] = nearest * nearest;
            dist_sum += dists[r];
        }
        if (dist_sum <= 0.0) {
            for (size_t j = c; j < k; ++j) memcpy(centers + j * dims, centers, dims * sizeof(double));
            break;
        }
        double pick = lcg_double01(&rng) * dist_sum;
        double accum = 0.0;
        size_t chosen = 0;
        for (size_t r = 0; r < rows; ++r) {
            accum += dists[r];
            if (accum >= pick) { chosen = r; break; }
        }
        memcpy(centers + c * dims, matrix->values + chosen * dims, dims * sizeof(double));
    }
    free(dists);
}

static size_t run_kmeans(const logana_feature_matrix_t *matrix, size_t k, size_t seed_offset,
                         logana_distance_fn_t dist_fn,
                         const logana_analysis_summary_t *summary,
                         int *out_labels) {
    size_t rows = matrix->row_count;
    size_t dims = matrix->dimensions;
    if (!rows) return 0;
    if (k > rows) k = rows;
    if (k == 0) k = 1;
    if (summary->cluster_options.has_kmeans_max_k && summary->cluster_options.kmeans_max_k > 0) {
        if (k > summary->cluster_options.kmeans_max_k) k = summary->cluster_options.kmeans_max_k;
    } else {
        if (k > 8) k = 8;
    }

    double centers[LOGANA_MAX_DIMENSIONS * 8] = {0};
    double best_centers[LOGANA_MAX_DIMENSIONS * 8] = {0};
    int *tmp_labels = calloc(rows, sizeof(int));
    if (!tmp_labels) return 0;

    size_t n_init = (rows > 5000) ? 3 : 5;
    if (summary->cluster_options.has_kmeans_n_init && summary->cluster_options.kmeans_n_init > 0) {
        n_init = summary->cluster_options.kmeans_n_init;
    }
    double best_inertia = 1e300;

    for (size_t trial = 0; trial < n_init; ++trial) {
        uint64_t seed = (uint64_t)(seed_offset + trial * 31 + 0x9e3779b9);
        kmeans_pp_init(matrix, k, seed, centers, dist_fn, summary);
        size_t max_iter = 30;
        if (summary->cluster_options.has_kmeans_iterations && summary->cluster_options.kmeans_iterations > 0) {
            max_iter = summary->cluster_options.kmeans_iterations;
        }
        for (size_t iter = 0; iter < max_iter; ++iter) {
            double accum[LOGANA_MAX_DIMENSIONS * 8] = {0};
            size_t counts[8] = {0};
            for (size_t r = 0; r < rows; ++r) {
                double best_dist = 1e18;
                int best = 0;
                for (size_t c = 0; c < k; ++c) {
                    double d = dist_fn(matrix->values + r * dims, centers + c * dims,
                                       matrix->valid_mask + r * dims, NULL, dims, summary);
                    if (d < best_dist) { best_dist = d; best = (int)c; }
                }
                tmp_labels[r] = best;
                counts[best]++;
                for (size_t d = 0; d < dims; ++d) {
                    if (matrix->valid_mask && !matrix->valid_mask[r * dims + d]) continue;
                    accum[best * dims + d] += matrix->values[r * dims + d];
                }
            }
            for (size_t c = 0; c < k; ++c) {
                if (!counts[c]) continue;
                for (size_t d = 0; d < dims; ++d) {
                    if (matrix->valid_mask) {
                        size_t valid_cnt = 0;
                        for (size_t r = 0; r < rows; ++r) {
                            if (tmp_labels[r] == (int)c && matrix->valid_mask[r * dims + d]) valid_cnt++;
                        }
                        if (valid_cnt) centers[c * dims + d] = accum[c * dims + d] / (double)valid_cnt;
                    } else {
                        centers[c * dims + d] = accum[c * dims + d] / (double)counts[c];
                    }
                }
            }
        }
        double inertia = kmeans_inertia(matrix, k, centers, dist_fn, summary);
        if (inertia < best_inertia) {
            best_inertia = inertia;
            memcpy(best_centers, centers, sizeof(centers));
            memcpy(out_labels, tmp_labels, rows * sizeof(int));
        }
    }

    memcpy(centers, best_centers, sizeof(best_centers));
    for (size_t r = 0; r < rows; ++r) {
        double best_dist = 1e18;
        int best = 0;
        for (size_t c = 0; c < k; ++c) {
            double d = dist_fn(matrix->values + r * dims, centers + c * dims,
                               matrix->valid_mask + r * dims, NULL, dims, summary);
            if (d < best_dist) { best_dist = d; best = (int)c; }
        }
        out_labels[r] = best;
    }
    free(tmp_labels);
    return k;
}

static double collapse_score(size_t rows, const int *labels, size_t k) {
    if (rows == 0 || k == 0) return 1.0;
    size_t *counts = calloc(k, sizeof(size_t));
    if (!counts) return 1.0;
    for (size_t i = 0; i < rows; ++i) {
        int lb = labels[i];
        if (lb >= 0 && (size_t)lb < k) counts[lb]++;
    }
    size_t max_count = 0, empty = 0;
    for (size_t c = 0; c < k; ++c) {
        if (counts[c] > max_count) max_count = counts[c];
        if (counts[c] == 0) empty++;
    }
    free(counts);
    return ((double)max_count / (double)rows) * 0.6 + ((double)empty / (double)k) * 0.4;
}

static size_t run_kmeans_auto(const logana_feature_matrix_t *matrix,
                              logana_distance_fn_t dist_fn,
                              const logana_analysis_summary_t *summary,
                              int *out_labels) {
    size_t rows = matrix->row_count;
    if (!rows) return 0;
    if (rows == 1) { out_labels[0] = 0; return 1; }
    /* For large matrices, avoid the expensive auto sweep and just run once */
    if (rows > 100000) {
        return run_kmeans(matrix, 3, 0, dist_fn, summary, out_labels);
    }
    int *tmp_labels = calloc(rows, sizeof(int));
    int *best_labels = calloc(rows, sizeof(int));
    if (!tmp_labels || !best_labels) { free(tmp_labels); free(best_labels); return run_kmeans(matrix, 2, 0, dist_fn, summary, out_labels); }
    double best_score = 2.0;
    size_t best_actual_k = 1;
    for (size_t k = 2; k <= 4; ++k) {
        if (k > rows) break;
        for (size_t seed = 0; seed < 3; ++seed) {
            memset(tmp_labels, 0, rows * sizeof(int));
            logana_feature_matrix_t trial = *matrix;
            trial.labels = tmp_labels;
            size_t actual_k = run_kmeans(&trial, k, seed * 7, dist_fn, summary, tmp_labels);
            double score = collapse_score(rows, tmp_labels, actual_k);
            if (score < best_score) {
                best_score = score;
                best_actual_k = actual_k;
                memcpy(best_labels, tmp_labels, rows * sizeof(int));
            }
        }
    }
    memcpy(out_labels, best_labels, rows * sizeof(int));
    free(tmp_labels);
    free(best_labels);
    return best_actual_k;
}

static int kmeans_fit(const logana_cluster_strategy_vtable_t *self,
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
    size_t k = run_kmeans_auto(matrix, dist_fn, summary, labels);

    out->labels = labels;
    out->is_noise = is_noise;
    out->row_count = rows;
    out->cluster_count = k;
    out->noise_count = 0;
    out->silhouette_score = -1.0;
    out->davies_bouldin_index = INFINITY;
    return 0;
}

static const logana_cluster_strategy_vtable_t kmeans_vtable = {
    .name = "kmeans++",
    .fit  = kmeans_fit,
};

const logana_cluster_strategy_vtable_t *logana_strategy_kmeans(void) {
    return &kmeans_vtable;
}
