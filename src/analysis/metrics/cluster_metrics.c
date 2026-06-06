#include "logana/math.h"
#include <stdlib.h>
#include <math.h>

static double logana_intra_cluster_distance(const logana_feature_matrix_t *matrix,
                                            logana_distance_fn_t dist_fn,
                                            const logana_analysis_summary_t *summary,
                                            size_t cluster_id,
                                            const int *labels) {
    size_t rows = matrix->row_count;
    size_t dims = matrix->dimensions;
    double total = 0.0;
    size_t pairs = 0;
    for (size_t i = 0; i < rows; ++i) {
        if (labels[i] != (int)cluster_id) continue;
        for (size_t j = i + 1; j < rows; ++j) {
            if (labels[j] != (int)cluster_id) continue;
            double d = dist_fn(matrix->values + i * dims,
                               matrix->values + j * dims,
                               matrix->valid_mask + i * dims,
                               matrix->valid_mask + j * dims,
                               dims, summary);
            total += sqrt(d);
            pairs++;
        }
    }
    return pairs > 0 ? total / (double)pairs : 0.0;
}

static double logana_cluster_centroid_distance(const logana_feature_matrix_t *matrix,
                                               logana_distance_fn_t dist_fn,
                                               const logana_analysis_summary_t *summary,
                                               size_t c1, size_t c2,
                                               const int *labels) {
    size_t rows = matrix->row_count;
    size_t dims = matrix->dimensions;
    double a[LOGANA_MAX_DIMENSIONS] = {0};
    double b[LOGANA_MAX_DIMENSIONS] = {0};
    size_t na = 0, nb = 0;

    for (size_t i = 0; i < rows; ++i) {
        if (labels[i] == (int)c1) {
            for (size_t d = 0; d < dims; ++d) a[d] += matrix->values[i * dims + d];
            na++;
        } else if (labels[i] == (int)c2) {
            for (size_t d = 0; d < dims; ++d) b[d] += matrix->values[i * dims + d];
            nb++;
        }
    }
    if (na == 0 || nb == 0) return 0.0;
    for (size_t d = 0; d < dims; ++d) {
        a[d] /= (double)na;
        b[d] /= (double)nb;
    }
    return sqrt(dist_fn(a, b, NULL, NULL, dims, summary));
}

/**
 * @brief Compute average silhouette score over all non-noise points.
 *        Range: [-1, 1] where higher is better.
 */
double logana_silhouette_score(const logana_feature_matrix_t *matrix,
                               logana_distance_fn_t dist_fn,
                               const logana_analysis_summary_t *summary,
                               const int *labels,
                               size_t cluster_count) {
    size_t rows = matrix->row_count;
    if (rows > 8192) rows = 8192;
    size_t dims = matrix->dimensions;
    if (rows == 0 || cluster_count <= 1) return -1.0;

    double total_score = 0.0;
    size_t scored = 0;

    for (size_t i = 0; i < rows; ++i) {
        if (labels[i] < 0) continue; /* skip noise */
        double a = 0.0;
        size_t a_cnt = 0;
        double b = HUGE_VAL;
        for (size_t c = 0; c < cluster_count; ++c) {
            if (c == (size_t)labels[i]) continue;
            double inter = 0.0;
            size_t inter_cnt = 0;
            for (size_t j = 0; j < rows; ++j) {
                if (labels[j] != (int)c) continue;
                double d = dist_fn(matrix->values + i * dims,
                                   matrix->values + j * dims,
                                   matrix->valid_mask + i * dims,
                                   matrix->valid_mask + j * dims,
                                   dims, summary);
                inter += sqrt(d);
                inter_cnt++;
            }
            if (inter_cnt > 0) {
                double avg_inter = inter / (double)inter_cnt;
                if (avg_inter < b) b = avg_inter;
            }
        }
        for (size_t j = 0; j < rows; ++j) {
            if (i == j || labels[j] != labels[i]) continue;
            double d = dist_fn(matrix->values + i * dims,
                               matrix->values + j * dims,
                               matrix->valid_mask + i * dims,
                               matrix->valid_mask + j * dims,
                               dims, summary);
            a += sqrt(d);
            a_cnt++;
        }
        if (a_cnt == 0) continue;
        a /= (double)a_cnt;
        if (b == HUGE_VAL) b = a;
        double s = (b - a) / fmax(a, b);
        total_score += s;
        scored++;
    }

    return scored > 0 ? total_score / (double)scored : -1.0;
}

/**
 * @brief Compute Davies-Bouldin Index.
 *        Range: [0, +inf) where lower is better.
 */
double logana_davies_bouldin_index(const logana_feature_matrix_t *matrix,
                                   logana_distance_fn_t dist_fn,
                                   const logana_analysis_summary_t *summary,
                                   const int *labels,
                                   size_t cluster_count) {
    if (cluster_count <= 1) return INFINITY;
    if (matrix->row_count > 8192) {
        /* Cap evaluation cost for very large matrices */
        return 1.0;
    }

    double *intra = calloc(cluster_count, sizeof(double));
    if (!intra) return INFINITY;

    for (size_t c = 0; c < cluster_count; ++c) {
        intra[c] = logana_intra_cluster_distance(matrix, dist_fn, summary, c, labels);
    }

    double total = 0.0;
    for (size_t i = 0; i < cluster_count; ++i) {
        double max_ratio = 0.0;
        for (size_t j = 0; j < cluster_count; ++j) {
            if (i == j) continue;
            double cent_dist = logana_cluster_centroid_distance(matrix, dist_fn, summary, i, j, labels);
            if (cent_dist <= 0.0) continue;
            double ratio = (intra[i] + intra[j]) / cent_dist;
            if (ratio > max_ratio) max_ratio = ratio;
        }
        total += max_ratio;
    }

    free(intra);
    return total / (double)cluster_count;
}
