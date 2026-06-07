#include "logana/cluster_strategy.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <float.h>

#define LOGANA_GMM_MAX_COMPONENTS_CAP 8

static double gmm_get_sigma_floor(const logana_analysis_summary_t *summary, size_t dim) {
    if (!summary) return 1.0;
    double range = summary->max[dim] - summary->min[dim];
    if (!isfinite(range) || range <= 0.0) range = 1.0;
    double fraction = summary->cluster_options.has_min_stddev_fraction
                          ? summary->cluster_options.min_stddev_fraction
                          : 1e-4;
    double floor = range * fraction;
    return floor > 0.0 ? floor : 1.0;
}

static size_t run_gmm(const logana_feature_matrix_t *matrix, size_t k,
                      const logana_analysis_summary_t *summary,
                      int *labels, size_t em_iters) {
    if (!matrix || !matrix->values || !labels) return 0;
    size_t rows = matrix->row_count;
    size_t dims = matrix->dimensions;
    if (k > LOGANA_GMM_MAX_COMPONENTS_CAP) k = LOGANA_GMM_MAX_COMPONENTS_CAP;
    if (k == 0) k = 1;

    double *means = calloc(k * dims, sizeof(double));
    double *variances = calloc(k * dims, sizeof(double));
    double *weights = malloc(k * sizeof(double));
    double *resp_sum = calloc(k, sizeof(double));
    double *mean_accum = calloc(k * dims, sizeof(double));
    double *probs = malloc(k * sizeof(double));
    if (!means || !variances || !weights || !resp_sum || !mean_accum || !probs) {
        free(means); free(variances); free(weights); free(resp_sum); free(mean_accum); free(probs);
        return 0;
    }
    for (size_t c = 0; c < k; ++c) weights[c] = 1.0 / (double)k;
    for (size_t c = 0; c < k; ++c) {
        memcpy(means + c * dims, matrix->values + (c % rows) * dims, dims * sizeof(double));
        for (size_t d = 0; d < dims; ++d) variances[c * dims + d] = 1.0;
    }

    for (size_t iter = 0; iter < em_iters; ++iter) {
        memset(resp_sum, 0, k * sizeof(double));
        memset(mean_accum, 0, k * dims * sizeof(double));
        for (size_t r = 0; r < rows; ++r) {
            double norm = 0.0;
            for (size_t c = 0; c < k; ++c) {
                double dist = 0.0;
                size_t valid_d = 0;
                for (size_t d = 0; d < dims; ++d) {
                    if (matrix->valid_mask && !matrix->valid_mask[r * dims + d]) continue;
                    double sigma = gmm_get_sigma_floor(summary, d);
                    if (summary && summary->stddev[d] > sigma) sigma = summary->stddev[d];
                    double delta = (matrix->values[r * dims + d] - means[c * dims + d]) / sigma;
                    dist += (delta * delta) / variances[c * dims + d];
                    ++valid_d;
                }
                if (valid_d > 0) dist *= (double)dims / (double)valid_d;
                probs[c] = weights[c] * exp(-0.5 * dist);
                if (!isfinite(probs[c])) probs[c] = 0.0;
                norm += probs[c];
            }
            if (norm <= 0.0 || !isfinite(norm)) norm = 1.0;
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
            if (weights[c] < 1e-12) weights[c] = 1e-12;
            for (size_t d = 0; d < dims; ++d) means[c * dims + d] = mean_accum[c * dims + d] / denom;
        }
    }

    free(means);
    free(variances);
    free(weights);
    free(resp_sum);
    free(mean_accum);
    free(probs);
    return k;
}

static int gmm_fit(const logana_cluster_strategy_vtable_t *self,
                   const logana_feature_matrix_t *matrix,
                   const logana_analysis_summary_t *summary,
                   logana_cluster_result_t *out) {
    if (!self || !matrix || !matrix->values || !out) return -1;
    size_t rows = matrix->row_count;
    if (!rows) return -1;
    int *labels = calloc(rows, sizeof(int));
    bool *is_noise = calloc(rows, sizeof(bool));
    if (!labels || !is_noise) { free(labels); free(is_noise); return -1; }

    size_t k = 3;
    if (summary && summary->cluster_options.has_gmm_max_components && summary->cluster_options.gmm_max_components > 0) {
        k = summary->cluster_options.gmm_max_components;
    }
    if (rows < k) k = rows;
    if (k > LOGANA_GMM_MAX_COMPONENTS_CAP) k = LOGANA_GMM_MAX_COMPONENTS_CAP;
    size_t em_iters = 8;
    if (summary && summary->cluster_options.has_gmm_em_iterations && summary->cluster_options.gmm_em_iterations > 0) {
        em_iters = summary->cluster_options.gmm_em_iterations;
    }
    run_gmm(matrix, k, summary, labels, em_iters);

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
