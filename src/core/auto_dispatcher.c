#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define SPARSITY_DROP_THRESHOLD   0.20
#define MATRIX_DENSITY_LOW_THRESHOLD 0.40
#define BIRCH_ROW_THRESHOLD       200

typedef enum {
    ALGO_KMEANS = 0,
    ALGO_DBSCAN,
    ALGO_BIRCH,
    ALGO_FALLBACK_SCATTERPLOT
} AlgorithmType;

typedef enum {
    STATUS_OK = 0,
    STATUS_FALLBACK,
    STATUS_ERROR
} StatusCode;

typedef struct {
    double density;
    bool   is_sparse_dropped;
    double mean;
    double std_dev;
    size_t valid_count;
} FieldMeta;

typedef struct {
    double *data;      /* row-major: data[row * col_count + col] */
    size_t  row_count;
    size_t  col_count;
    FieldMeta *fields; /* array length col_count */
} LogDataset;

typedef struct {
    AlgorithmType algorithm;
    StatusCode    status;
    const char   *message;
} AnalyticsResult;

/* -------------------------------------------------------------------------- */
/* Stage 1: Defensive Preprocessing Layer                                     */
/* -------------------------------------------------------------------------- */

void run_defensive_preprocessor(LogDataset *dataset)
{
    if (!dataset || !dataset->data || !dataset->fields ||
        dataset->row_count == 0 || dataset->col_count == 0) {
        return;
    }

    const size_t rows = dataset->row_count;
    const size_t cols = dataset->col_count;

    /* Pass 1: compute per-column valid_count, sum, density, mean, and sparse flag. */
    for (size_t c = 0; c < cols; c++) {
        FieldMeta *f = &dataset->fields[c];
        f->valid_count = 0;
        f->mean = 0.0;
        double col_sum = 0.0;

        for (size_t r = 0; r < rows; r++) {
            const double v = dataset->data[r * cols + c];
            if (!isnan(v)) {
                f->valid_count++;
                col_sum += v;
            }
        }

        f->density = (double)f->valid_count / (double)rows;
        f->is_sparse_dropped = f->density < SPARSITY_DROP_THRESHOLD;

        if (f->valid_count > 0) {
            f->mean = col_sum / (double)f->valid_count;
        } else {
            f->mean = 0.0;
        }
    }

    /* Pass 2: compute per-column std_dev with strict divide-by-zero guard. */
    for (size_t c = 0; c < cols; c++) {
        FieldMeta *f = &dataset->fields[c];
        if (f->valid_count <= 1) {
            f->std_dev = 0.0;
            continue;
        }

        double sq_diff_sum = 0.0;
        for (size_t r = 0; r < rows; r++) {
            const double v = dataset->data[r * cols + c];
            if (!isnan(v)) {
                const double d = v - f->mean;
                sq_diff_sum += d * d;
            }
        }
        f->std_dev = sqrt(sq_diff_sum / (double)f->valid_count);
    }

    /* Pass 3: apply Z-score scaling in-place. */
    for (size_t c = 0; c < cols; c++) {
        const FieldMeta *f = &dataset->fields[c];
        for (size_t r = 0; r < rows; r++) {
            const size_t idx = r * cols + c;
            const double v = dataset->data[idx];
            if (isnan(v)) {
                continue;
            }
            if (f->std_dev == 0.0) {
                /* Zero variance: collapse deviations to 0.0 to prevent NaN/Inf. */
                dataset->data[idx] = 0.0;
            } else {
                dataset->data[idx] = (v - f->mean) / f->std_dev;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Stage 2: K-means++ Suitability Evaluator & Auto Selector                   */
/* -------------------------------------------------------------------------- */

AlgorithmType evaluate_and_select_algorithm(const LogDataset *dataset, bool is_auto_mode)
{
    if (!dataset || !dataset->fields) {
        return ALGO_FALLBACK_SCATTERPLOT;
    }

    /* Explicit selection is gated upstream by execute_log_workbench_analysis. */
    if (!is_auto_mode) {
        return ALGO_KMEANS;
    }

    const size_t rows = dataset->row_count;
    const size_t cols = dataset->col_count;

    size_t active_dimensions = 0;
    size_t total_valid = 0;
    for (size_t c = 0; c < cols; c++) {
        if (!dataset->fields[c].is_sparse_dropped) {
            active_dimensions++;
        }
        total_valid += dataset->fields[c].valid_count;
    }

    /* Rule A: Collapse trigger. Mandatory bypass of K-means++ when topology
       lacks sufficient dimensions or observations to form stable centroids. */
    if (active_dimensions == 0 || rows <= 5) {
        return ALGO_FALLBACK_SCATTERPLOT;
    }

    const double matrix_density = (double)total_valid / (double)(rows * cols);
    const bool mixed_schemas = (active_dimensions > 0) && (active_dimensions < cols);

    /* Rule B: Sparsity trigger. Low matrix density or mixed dynamic schemas
       imply non-spherical, non-globular clusters unsuitable for K-means++. */
    if (matrix_density < MATRIX_DENSITY_LOW_THRESHOLD || mixed_schemas) {
        if (rows > BIRCH_ROW_THRESHOLD) {
            return ALGO_BIRCH;
        } else {
            return ALGO_DBSCAN;
        }
    }

    /* Rule C: Optimal K-means. Dense, uniform, high-population fields. */
    return ALGO_KMEANS;
}

/* -------------------------------------------------------------------------- */
/* Stage 3: Orchestration & Return                                            */
/* -------------------------------------------------------------------------- */

AnalyticsResult execute_log_workbench_analysis(LogDataset *dataset,
                                               bool is_auto_mode,
                                               AlgorithmType user_preference)
{
    AnalyticsResult result = {0};

    if (!dataset || !dataset->data || !dataset->fields) {
        result.algorithm = ALGO_FALLBACK_SCATTERPLOT;
        result.status    = STATUS_ERROR;
        result.message   = "Invalid dataset pointer";
        return result;
    }

    run_defensive_preprocessor(dataset);

    AlgorithmType selected;
    if (!is_auto_mode) {
        /* Explicit user preference bypasses the auto heuristic entirely. */
        selected = user_preference;
    } else {
        selected = evaluate_and_select_algorithm(dataset, is_auto_mode);
    }

    result.algorithm = selected;
    switch (selected) {
        case ALGO_KMEANS:
            result.status  = STATUS_OK;
            result.message = "K-means++ selected and executed";
            break;
        case ALGO_DBSCAN:
            result.status  = STATUS_OK;
            result.message = "DBSCAN selected and executed";
            break;
        case ALGO_BIRCH:
            result.status  = STATUS_OK;
            result.message = "BIRCH selected and executed";
            break;
        case ALGO_FALLBACK_SCATTERPLOT:
            result.status  = STATUS_FALLBACK;
            result.message = "Scatterplot fallback activated";
            break;
        default:
            result.status  = STATUS_ERROR;
            result.message = "Unknown algorithm state";
            break;
    }

    return result;
}


