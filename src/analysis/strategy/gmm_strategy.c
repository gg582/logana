#include "logana/cluster_strategy.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

static size_t run_gmm(const logana_feature_matrix_t *matrix, size_t k,
                      const logana_analysis_summary_t *summary,
                      int *labels) {
    size_t rows = matrix->row_count;
    size_t dims = matrix->dimensions;
    if (k > 3) k = 3;
    double means[LOGANA_MAX_DIMENSIONS * 3] = {0};
    double variances[LOGANA_MAX_DIMENSIONS * 3] = {0};
    double weights[3] = {0.34, 0.33, 0.33};
    for (size_t c = 0; c < k; ++c) {
        memcpy(means + c * dims, matrix->values + (c % rows) * dims, dims * sizeof(double));
        for (size_t d = 0; d < dims; ++d) variances[c * dims + d] = 1.0;
    }
    for (size_t iter = 0; iter < 8; ++iter) {
        double resp_sum[3] = {0};
        double mean_accum[LOGANA_MAX_DIMENSIONS * 3] = {0};
        for (size_t r = 0; r < rows; ++r) {
            double probs[3] = {0};
            double norm = 0.0;
            for (size_t c = 0; c < k; ++c) {
                double dist = 0.0;
                size_t valid_d = 0;
                for (size_t d = 0; d < dims; ++d) {
                    if (matrix->valid_mask && !matrix->valid_mask[r * dims + d]) continue;
                    double sigma = summary && summary->stddev[d] > 0.0001 ? summary->stddev[d] : 1.0;
                    double delta = (matrix->values[r * dims + d] - means[c * dims + d]) / sigma;
                    dist += (delta * delta) / variances[c * dims + d];
                    ++valid_d;
                }
                if (valid_d > 0) dist *= (double)dims / (double)valid_d;
                probs[c] = weights[c] * exp(-0.5 * dist);
                norm += probs[c];
            }
            if (norm == 0.0) norm = 1.0;
            int best = 0;
            double best_resp = -1.0;
            for (size_t c = 0; c < k; ++c) {
                double resp = probs[c] / norm;
                resp_sum[c] += resp;
                if (resp > best_resp) { best_resp = resp; best = (int)c; }
                for (size_t d = 0; d < dims; ++d) {
                    if (matrix->valid_mask && !matrix->valid_mask[r * dims + d]) continue;
                    mean_accum[c * dims + d] += resp * matrix->values[r * dims + d];
                }
            }
            labels[r] = best;
        }
        for (size_t c = 0; c < k; ++c) {
            double denom = resp_sum[c] > 0.0 ? resp_sum[c] : 1.0;
            weights[c] = resp_sum[c] / (double)rows;
            for (size_t d = 0; d < dims; ++d) means[c * dims + d] = mean_accum[c * dims + d] / denom;
        }
    }
    return k;
}

static int gmm_fit(const logana_cluster_strategy_vtable_t *self,
                   const logana_feature_matrix_t *matrix,
                   const logana_analysis_summary_t *summary,
                   logana_cluster_result_t *out) {
    (void)self;
    size_t rows = matrix->row_count;
    if (!rows) return -1;
    int *labels = calloc(rows, sizeof(int));
    bool *is_noise = calloc(rows, sizeof(bool));
    if (!labels || !is_noise) { free(labels); free(is_noise); return -1; }

    size_t k = 3;
    if (rows < k) k = rows;
    run_gmm(matrix, k, summary, labels);

    out->labels = labels;
    out->is_noise = is_noise;
    out->row_count = rows;
    out->cluster_count = k;
    out->noise_count = 0;
    out->silhouette_score = -1.0;
    out->davies_bouldin_index = INFINITY;
    return 0;
}

static const logana_cluster_strategy_vtable_t gmm_vtable = {
    .name = "gmm",
    .fit  = gmm_fit,
};

const logana_cluster_strategy_vtable_t *logana_strategy_gmm(void) {
    return &gmm_vtable;
}
