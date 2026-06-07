#include "logana/math.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LOGANA_KNEE_ROW_CAP 65536

static int kd_compare_asc(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/**
 * @brief Compute k-distance graph and detect knee via 2nd-derivative maximum.
 * @param matrix Input feature matrix (must have valid rows > 0).
 * @param k Neighbor rank (typically min_samples).
 * @param dist_fn Distance function.
 * @param summary Summary for z-score distance.
 * @return Estimated eps. Falls back to median k-dist on flat graph.
 */
double logana_knee_detect_eps(const logana_feature_matrix_t *matrix,
                              size_t k,
                              logana_distance_fn_t dist_fn,
                              const logana_analysis_summary_t *summary) {
    if (!matrix || !matrix->values || !dist_fn) return 1e-6;
    size_t rows = matrix->row_count;
    size_t dims = matrix->dimensions;
    if (rows == 0 || dims == 0 || k == 0) return 1e-6;
    if (rows > LOGANA_KNEE_ROW_CAP) rows = LOGANA_KNEE_ROW_CAP;
    if (k >= rows) k = rows - 1;

    double *kdists = calloc(rows, sizeof(double));
    if (!kdists) return 1e-6;

    for (size_t i = 0; i < rows; ++i) {
        double *dists = malloc(rows * sizeof(double));
        if (!dists) { free(kdists); return 1e-6; }
        for (size_t j = 0; j < rows; ++j) {
            if (i == j) {
                dists[j] = 0.0;
            } else {
                dists[j] = dist_fn(matrix->values + i * dims,
                                   matrix->values + j * dims,
                                   matrix->valid_mask ? matrix->valid_mask + i * dims : NULL,
                                   matrix->valid_mask ? matrix->valid_mask + j * dims : NULL,
                                   dims, summary);
                if (dists[j] < 0.0) dists[j] = 0.0;
            }
        }
        qsort(dists, rows, sizeof(double), kd_compare_asc);
        kdists[i] = dists[k];
        free(dists);
    }

    qsort(kdists, rows, sizeof(double), kd_compare_asc);

    double best_eps = kdists[rows / 2];
    double best_curvature = 0.0;
    for (size_t i = 1; i + 1 < rows; ++i) {
        double y_prev = kdists[i - 1];
        double y_curr = kdists[i];
        double y_next = kdists[i + 1];
        double second_deriv = fabs(y_next - 2.0 * y_curr + y_prev);
        if (second_deriv > best_curvature) {
            best_curvature = second_deriv;
            best_eps = y_curr;
        }
    }

    if (best_eps <= 0.0 || best_curvature <= 0.0) {
        best_eps = kdists[rows / 2];
    }
    if (best_eps <= 0.0) best_eps = 1e-6;

    free(kdists);
    return best_eps;
}
